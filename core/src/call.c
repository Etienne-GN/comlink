#include "core_internal.h"
#include "bridge.h"
#include "modules.h"
#include "hdr_rules.h"
#include "history.h"
#include "oggopus.h"
#include "paths.h"
#include "quality.h"
#include "recname.h"
#include "sipuri.h"
#include "transcap.h"
#include "transcriber.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* Room for the per-call ad-hoc headers a frontend may attach. */
#define SP_MAX_EXTRA_HEADERS 32

/* Guards every call record: its fields, and its leaving the table and
   being freed, which a pjsip thread does at DISCONNECTED while API
   threads are still asking about the call. A record pointer is only
   good while this is held.
 *
 * A leaf: no pjsua call and no other core lock is taken while holding
 * it. pjsua holds PJSUA_LOCK across some callbacks, so a thread that
 * called into pjsua with this held would deadlock against a callback
 * waiting for it. One core per process (core.c), so one lock. */
static pthread_mutex_t g_rec_lock = PTHREAD_MUTEX_INITIALIZER;

static void refresh_party(sp_call_rec *rec, const pjsua_call_info *ci,
                          const char *codec);

/* Caller holds g_rec_lock. */
static sp_call_rec *rec_of(sp_core *core, sp_call_id call, int *native) {
    void *data = NULL;
    if (sp_ht_lookup(core->calls, call, native, &data) != 0)
        return NULL;
    return data;
}

static bool native_of(sp_core *core, sp_call_id call, int *native) {
    return sp_ht_lookup(core->calls, call, native, NULL) == 0;
}

static void stream_codec(pjsua_call_id native, char *out, size_t outlen) {
    out[0] = '\0';
    pjsua_stream_info si;
    if (pjsua_call_get_stream_info(native, 0, &si) == PJ_SUCCESS &&
        si.type == PJMEDIA_TYPE_AUDIO &&
        si.info.aud.fmt.encoding_name.slen > 0)
        snprintf(out, outlen, "%.*s",
                 (int)si.info.aud.fmt.encoding_name.slen,
                 si.info.aud.fmt.encoding_name.ptr);
}

static sp_status not_found(sp_core *core, sp_call_id call) {
    sp_core_detail(core, "call handle %llu is unknown or retired",
             (unsigned long long)call);
    return SP_ERR_NOT_FOUND;
}

/* Builds a msg_data carrying the caller's ad-hoc headers. The pool must
   outlive the pjsua call, so the caller releases it afterwards. */
static sp_status build_msg_data(sp_core *core, const sp_header *extra,
                                size_t n_extra, pj_pool_t **pool_out,
                                pjsua_msg_data *md) {
    pjsua_msg_data_init(md);
    *pool_out = NULL;
    if (!extra || n_extra == 0)
        return SP_OK;
    if (n_extra > SP_MAX_EXTRA_HEADERS) {
        sp_core_detail(core, "at most %d ad-hoc headers per request, got %zu",
                 SP_MAX_EXTRA_HEADERS, n_extra);
        return SP_ERR_INVALID_ARG;
    }

    pj_pool_t *pool = pjsua_pool_create("sp_hdr", 1024, 1024);
    if (!pool)
        return SP_ERR_INTERNAL;

    for (size_t i = 0; i < n_extra; i++) {
        if (!extra[i].name || !*extra[i].name || !extra[i].value) {
            pj_pool_release(pool);
            sp_core_detail(core, "ad-hoc header %zu has an empty name or value", i);
            return SP_ERR_INVALID_ARG;
        }
        if (!sp_hdr_name_ok(extra[i].name) || !sp_hdr_value_ok(extra[i].value)) {
            pj_pool_release(pool);
            sp_core_detail(core, "ad-hoc header %zu has a name that is not a "
                           "token, or a value with a control character", i);
            return SP_ERR_INVALID_ARG;
        }
        pj_str_t name = pj_str((char *)extra[i].name);
        pj_str_t value = pj_str((char *)extra[i].value);
        pjsip_generic_string_hdr *h =
            pjsip_generic_string_hdr_create(pool, &name, &value);
        if (!h) {
            pj_pool_release(pool);
            return SP_ERR_INTERNAL;
        }
        pj_list_push_back(&md->hdr_list, h);
    }
    *pool_out = pool;
    return SP_OK;
}

sp_status sp_call_make(sp_core *core, sp_account_id acc, const char *uri,
                       const sp_header *extra, size_t n_extra,
                       sp_call_id *out) {
    SP_ENTRY(core);
    if (!uri || !*uri || !out)
        return SP_ERR_INVALID_ARG;
    *out = 0;
    if (!core->pj_ready)
        return SP_ERR_STATE;

    int native_acc = PJSUA_INVALID_ID;
    if (sp_ht_lookup(core->accounts, acc, &native_acc, NULL) != 0) {
        sp_core_detail(core, "account handle %llu is unknown or retired",
                 (unsigned long long)acc);
        return SP_ERR_NOT_FOUND;
    }

    pj_pool_t *pool = NULL;
    pjsua_msg_data md;
    sp_status s = build_msg_data(core, extra, n_extra, &pool, &md);
    if (s != SP_OK)
        return s;

    pjsua_call_setting opt;
    pjsua_call_setting_default(&opt);
    opt.aud_cnt = 1;
    opt.vid_cnt = 0;
    opt.txt_cnt = 0;   /* we do not implement T.140; do not offer it */

    /* The record and its handle exist before the call is placed, and
       the record is handed to pjsua as user_data, because pjsua fires
       on_call_state while pjsua_call_make_call is still running. */
    sp_call_rec *rec = calloc(1, sizeof *rec);
    if (!rec) {
        if (pool)
            pj_pool_release(pool);
        return SP_ERR_INTERNAL;
    }
    rec->native = PJSUA_INVALID_ID;
    rec->acc = acc;
    rec->recorder = PJSUA_INVALID_ID;
    rec->rec_slot = PJSUA_INVALID_ID;

    uint64_t handle = sp_ht_add(core->calls, PJSUA_INVALID_ID, rec);
    if (handle == 0) {
        free(rec);
        if (pool)
            pj_pool_release(pool);
        return SP_ERR_INTERNAL;
    }
    rec->handle = handle;
    sp_transcriber_call_started(core->transcriber);

    /* The history record is opened before the call is placed, for the
       same reason as the handle: pjsua fires on_call_state during
       pjsua_call_make_call, and that callback is what fills in the
       Call-ID. Opening it afterwards left every outgoing record
       without one. The dialled URI is the remote party. */
    snprintf(rec->remote_uri, sizeof rec->remote_uri, "%s", uri);
    sp_sipuri_parse(rec->remote_uri, &rec->remote);

    rec->history_id = sp_history_begin(core->history, false,
                                       sp_account_id_of(core, acc),
                                       uri, NULL, NULL);

    pj_str_t dst = pj_str((char *)uri);
    pjsua_call_id native = PJSUA_INVALID_ID;
    pj_status_t st = pjsua_call_make_call(native_acc, &dst, &opt, rec, &md,
                                          &native);
    if (pool)
        pj_pool_release(pool);
    if (st != PJ_SUCCESS) {
        /* pjsua can run the whole life of a call inside
           pjsua_call_make_call. A destination it cannot reach reaches
           DISCONNECTED through on_call_state before this returns, and
           that callback retires the handle, writes the end of the
           history record and frees this one. Doing it again here was a
           double free on every unreachable destination, and a second
           sp_history_ended that overwrote the real status code with a
           zero.
        *
        * So: only finish what the callback has not already finished.
        * The callback runs on this thread, inside the call above, so
        * the record is either still in the table or gone for good --
        * there is no window between the two. */
        pthread_mutex_lock(&g_rec_lock);
        int64_t history_id = 0;
        const bool live = rec_of(core, handle, NULL) == rec;
        if (live) {
            history_id = rec->history_id;
            sp_ht_retire(core->calls, handle);
        }
        pthread_mutex_unlock(&g_rec_lock);
        if (live) {
            /* The history row stays: a call that failed to place is
               history too, and hiding it would make the log lie about
               what happened. */
            if (history_id > 0)
                sp_history_ended(core->history, history_id, 0, 0);
            free(rec);
        }
        char what[256];
        snprintf(what, sizeof what, "pjsua_call_make_call to \"%s\"", uri);
        return sp_core_fail(core, st, SP_ERR_TRANSPORT, what);
    }

    /* The call may already have ended on the SIP thread, and the
       record with it; the handle is returned all the same, retired. */
    pthread_mutex_lock(&g_rec_lock);
    if (rec_of(core, handle, NULL) == rec) {
        rec->native = native;
        sp_ht_set_native(core->calls, handle, native);
    }
    pthread_mutex_unlock(&g_rec_lock);
    *out = handle;
    return SP_OK;
}

sp_status sp_call_answer(sp_core *core, sp_call_id call, int code,
                         const sp_header *extra, size_t n_extra) {
    SP_ENTRY(core);
    if (code < 100 || code > 699)
        return SP_ERR_INVALID_ARG;
    int native = PJSUA_INVALID_ID;
    if (!native_of(core, call, &native))
        return not_found(core, call);

    pj_pool_t *pool = NULL;
    pjsua_msg_data md;
    sp_status s = build_msg_data(core, extra, n_extra, &pool, &md);
    if (s != SP_OK)
        return s;

    pjsua_call_setting opt;
    pjsua_call_setting_default(&opt);
    opt.aud_cnt = 1;
    opt.vid_cnt = 0;
    opt.txt_cnt = 0;   /* we do not implement T.140; do not offer it */

    pj_status_t st = pjsua_call_answer2(native, &opt, (unsigned)code, NULL, &md);
    if (pool)
        pj_pool_release(pool);
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_TRANSPORT, "pjsua_call_answer2");
    return SP_OK;
}

sp_status sp_call_hangup(sp_core *core, sp_call_id call, int code) {
    SP_ENTRY(core);
    int native = PJSUA_INVALID_ID;
    if (!native_of(core, call, &native))
        return not_found(core, call);
    pj_status_t st = pjsua_call_hangup(native, (unsigned)code, NULL, NULL);
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_TRANSPORT, "pjsua_call_hangup");
    return SP_OK;
}

sp_status sp_call_set_hold(sp_core *core, sp_call_id call, bool hold) {
    SP_ENTRY(core);
    int native = PJSUA_INVALID_ID;
    if (!native_of(core, call, &native))
        return not_found(core, call);

    pj_status_t st;
    if (hold)
        st = pjsua_call_set_hold(native, NULL);
    else
        st = pjsua_call_reinvite(native, PJSUA_CALL_UNHOLD, NULL);
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_MEDIA,
                            hold ? "pjsua_call_set_hold"
                                 : "pjsua_call_reinvite (unhold)");
    pthread_mutex_lock(&g_rec_lock);
    sp_call_rec *rec = rec_of(core, call, NULL);
    if (rec)
        rec->on_hold = hold;
    pthread_mutex_unlock(&g_rec_lock);
    return SP_OK;
}

sp_status sp_call_xfer_blind(sp_core *core, sp_call_id call, const char *uri) {
    SP_ENTRY(core);
    if (!uri || !*uri)
        return SP_ERR_INVALID_ARG;
    int native = PJSUA_INVALID_ID;
    pthread_mutex_lock(&g_rec_lock);
    sp_call_rec *rec = rec_of(core, call, &native);
    const sp_account_id acc = rec ? rec->acc : 0;
    pthread_mutex_unlock(&g_rec_lock);
    if (!rec)
        return not_found(core, call);
    /* An operator types "6043" here exactly as on the dialpad. Sent
       as-is it becomes "Refer-To: 6043", which FreeSWITCH answers
       202 and then never acts on, so the target is completed against
       the call's own account, the same way a dialled number is. */
    char target[512];
    sp_status cs = sp_call_uri_complete(core, acc, uri,
                                        target, sizeof target);
    if (cs != SP_OK) {
        sp_core_detail(core, "transfer target \"%s\" cannot be completed "
                       "against the call's account", uri);
        return cs;
    }
    pj_str_t dst = pj_str(target);
    pj_status_t st = pjsua_call_xfer(native, &dst, NULL);
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_TRANSPORT, "pjsua_call_xfer");
    return SP_OK;
}

sp_status sp_call_xfer_attended(sp_core *core, sp_call_id a, sp_call_id b) {
    SP_ENTRY(core);
    /* Rejected before either lookup: transferring a call to itself is a
       caller mistake, not a missing handle. */
    if (a == b)
        return SP_ERR_INVALID_ARG;
    int na = PJSUA_INVALID_ID, nb = PJSUA_INVALID_ID;
    if (!native_of(core, a, &na))
        return not_found(core, a);
    if (!native_of(core, b, &nb))
        return not_found(core, b);
    pj_status_t st = pjsua_call_xfer_replaces(na, nb, 0, NULL);
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_TRANSPORT,
                            "pjsua_call_xfer_replaces");
    return SP_OK;
}

static int is_dtmf(const char *s) {
    for (const char *p = s; *p; p++) {
        if ((*p >= '0' && *p <= '9') || *p == '*' || *p == '#' ||
            (*p >= 'A' && *p <= 'D'))
            continue;
        return 0;
    }
    return 1;
}

sp_status sp_call_send_dtmf(sp_core *core, sp_call_id call,
                            const char *digits) {
    SP_ENTRY(core);
    /* Validated before the handle lookup, so a bad alphabet is reported
       as such rather than masked by a stale handle. */
    if (!digits || !*digits || !is_dtmf(digits))
        return SP_ERR_INVALID_ARG;
    int native = PJSUA_INVALID_ID;
    if (!native_of(core, call, &native))
        return not_found(core, call);
    pj_str_t d = pj_str((char *)digits);
    pj_status_t st = pjsua_call_dial_dtmf(native, &d);
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_MEDIA, "pjsua_call_dial_dtmf");
    return SP_OK;
}

/* A call's recorder, off the record: taken under g_rec_lock, destroyed
   after letting it go, because destroying is a pjsua call. */
typedef struct {
    pjsua_recorder_id  recorder;
    pjmedia_port      *port;
    pj_pool_t         *pool;
    pjsua_conf_port_id slot;
} rec_writer;

/* Caller holds g_rec_lock. */
static rec_writer writer_take(sp_call_rec *rec) {
    rec_writer w = { rec->recorder, rec->rec_port, rec->rec_pool,
                     rec->rec_slot };
    rec->recorder = PJSUA_INVALID_ID;
    rec->rec_port = NULL;
    rec->rec_pool = NULL;
    rec->rec_slot = PJSUA_INVALID_ID;
    return w;
}

static void writer_destroy(const rec_writer *w) {
    if (w->recorder != PJSUA_INVALID_ID)
        pjsua_recorder_destroy(w->recorder);
    if (w->port) {
        /* Mirrors pjsua_recorder_destroy's own order (pjsua_aud.c):
           remove from the bridge, then destroy the port -- which runs
           on_destroy and is what finalises the Ogg stream -- then
           release the pool pjsua_conf_add_port needed kept alive
           while the port sat in the bridge. */
        pjsua_conf_remove_port(w->slot);
        pjmedia_port_destroy(w->port);
        pj_pool_release(w->pool);
    }
}

/* Hands a writer built outside the lock to the record. The call may
   have ended, or another thread started recording it, in between; then
   the writer is destroyed and the status says which. */
static sp_status writer_attach(sp_core *core, sp_call_id call,
                               const rec_writer *w, const char *path) {
    sp_status st = SP_OK;
    int64_t history_id = 0;
    pthread_mutex_lock(&g_rec_lock);
    sp_call_rec *rec = rec_of(core, call, NULL);
    if (!rec) {
        st = SP_ERR_NOT_FOUND;
    } else if (rec->recorder != PJSUA_INVALID_ID || rec->rec_port) {
        st = SP_ERR_STATE;
    } else {
        rec->recorder = w->recorder;
        rec->rec_port = w->port;
        rec->rec_pool = w->pool;
        rec->rec_slot = w->slot;
        history_id = rec->history_id;
    }
    pthread_mutex_unlock(&g_rec_lock);
    if (st != SP_OK) {
        writer_destroy(w);
        return st == SP_ERR_NOT_FOUND ? not_found(core, call) : st;
    }
    if (history_id > 0)
        sp_history_recording(core->history, history_id, path);
    return SP_OK;
}

sp_status sp_call_record_start(sp_core *core, sp_call_id call,
                               const char *path) {
    /* Told, rather than left to find no file afterwards. */
    if (core && !sp_config_module_enabled(core->cfg, SP_MODULE_RECORDING)) {
        sp_core_detail(core, "the recording module is switched off");
        return SP_ERR_DISABLED;
    }

    SP_ENTRY(core);
    if (!path || !*path)
        return SP_ERR_INVALID_ARG;
    int native = PJSUA_INVALID_ID;
    pthread_mutex_lock(&g_rec_lock);
    sp_call_rec *rec = rec_of(core, call, &native);
    const bool busy = rec && (rec->recorder != PJSUA_INVALID_ID
                              || rec->rec_port != NULL);
    pthread_mutex_unlock(&g_rec_lock);
    if (!rec)
        return not_found(core, call);
    if (busy)
        return SP_ERR_STATE;

    rec_writer w = { PJSUA_INVALID_ID, NULL, NULL, PJSUA_INVALID_ID };
    pjsua_call_info ci;
    if (pjsua_call_get_info(native, &ci) != PJ_SUCCESS ||
        ci.conf_slot == PJSUA_INVALID_ID)
        return SP_ERR_MEDIA;

    if (core->cfg->recording_format == SP_REC_OPUS) {
        pj_pool_t *pool = pjsua_pool_create("sp_rec_opus", 512, 512);
        if (!pool)
            return SP_ERR_INTERNAL;

        pjmedia_port *oport = NULL;
        sp_status ost = sp_oggopus_writer_create(
            pool, path, SP_REC_BRIDGE_CLOCK_RATE, SP_REC_BRIDGE_CHANNELS,
            SP_REC_BRIDGE_SAMPLES_PER_FRAME, &oport);
        if (ost != SP_OK) {
            pj_pool_release(pool);
            sp_core_detail(core,
                "sp_oggopus_writer_create for \"%s\" failed: %s", path,
                sp_strerror(ost));
            return ost;
        }

        pjsua_conf_port_id slot = PJSUA_INVALID_ID;
        if (pjsua_conf_add_port(pool, oport, &slot) != PJ_SUCCESS) {
            pjmedia_port_destroy(oport);
            pj_pool_release(pool);
            return SP_ERR_MEDIA;
        }
        /* Both directions: remote audio and the local microphone. */
        if (pjsua_conf_connect(ci.conf_slot, slot) != PJ_SUCCESS ||
            pjsua_conf_connect(0, slot) != PJ_SUCCESS) {
            pjsua_conf_remove_port(slot);
            pjmedia_port_destroy(oport);
            pj_pool_release(pool);
            return SP_ERR_MEDIA;
        }
        w.port = oport;
        w.pool = pool;
        w.slot = slot;
        return writer_attach(core, call, &w, path);
    }

    pj_str_t file = pj_str((char *)path);
    pjsua_recorder_id id = PJSUA_INVALID_ID;
    /* The fifth argument is options, not enc_type: pjsua asserts
       enc_type == 0 and then hands options straight to
       pjmedia_wav_writer_port_create as its flags
       (pjsua_aud.c:1600). So mu-law needs no lower-level port. */
    const unsigned opts = core->cfg->recording_format == SP_REC_WAV_ULAW
                              ? PJMEDIA_FILE_WRITE_ULAW
                              : PJMEDIA_FILE_WRITE_PCM;
    pj_status_t st = pjsua_recorder_create(&file, 0, NULL, 0, opts, &id);
    if (st != PJ_SUCCESS) {
        char what[320];
        snprintf(what, sizeof what, "pjsua_recorder_create for \"%s\"", path);
        return sp_core_fail(core, st, SP_ERR_MEDIA, what);
    }

    pjsua_conf_port_id port = pjsua_recorder_get_conf_port(id);
    /* Both directions: remote audio and the local microphone. */
    if (pjsua_conf_connect(ci.conf_slot, port) != PJ_SUCCESS ||
        pjsua_conf_connect(0, port) != PJ_SUCCESS) {
        pjsua_recorder_destroy(id);
        return SP_ERR_MEDIA;
    }
    w.recorder = id;
    return writer_attach(core, call, &w, path);
}

/* Names the file and records into it. The path goes to the caller's
   buffer, so the SIP thread starting an automatic recording and an API
   thread starting one by hand do not share one. */
static sp_status record_start_named(sp_core *core, sp_call_id call,
                                    char *path, size_t pathlen) {
    if (!native_of(core, call, NULL))
        return not_found(core, call);

    const char *dir = NULL;
    sp_status gs = sp_recording_get_dir(core, &dir);
    if (gs != SP_OK)
        return gs;
    /* Made again here: a directory can be removed between being
       chosen and being recorded into. */
    if (sp_path_mkdir_p(dir) != 0) {
        sp_core_detail(core, "the recording directory could not be created");
        return SP_ERR_IO;
    }

    if (sp_recording_path(path, pathlen, dir, (unsigned long long)call,
                          core->cfg->recording_format) != 0) {
        sp_core_detail(core,
            "the configured recording directory is too long for a "
            "recording path");
        return SP_ERR_IO;
    }
    return sp_call_record_start(core, call, path);
}

sp_status sp_call_record_start_auto(sp_core *core, sp_call_id call,
                                    const char **path_out) {
    if (core && !sp_config_module_enabled(core->cfg, SP_MODULE_RECORDING)) {
        sp_core_detail(core, "the recording module is switched off");
        return SP_ERR_DISABLED;
    }
    SP_ENTRY(core);
    if (!path_out)
        return SP_ERR_INVALID_ARG;

    char path[sizeof core->rec_path_view];
    sp_status st = record_start_named(core, call, path, sizeof path);
    if (st != SP_OK)
        return st;
    memcpy(core->rec_path_view, path, sizeof path);
    *path_out = core->rec_path_view;
    return SP_OK;
}

sp_status sp_call_record_stop(sp_core *core, sp_call_id call) {
    if (core && !sp_config_module_enabled(core->cfg, SP_MODULE_RECORDING)) {
        sp_core_detail(core, "the recording module is switched off");
        return SP_ERR_DISABLED;
    }

    SP_ENTRY(core);
    rec_writer w = { PJSUA_INVALID_ID, NULL, NULL, PJSUA_INVALID_ID };
    pthread_mutex_lock(&g_rec_lock);
    sp_call_rec *rec = rec_of(core, call, NULL);
    if (rec)
        w = writer_take(rec);
    pthread_mutex_unlock(&g_rec_lock);
    if (!rec)
        return not_found(core, call);
    writer_destroy(&w);
    return SP_OK;
}

sp_status sp_call_get_info(sp_core *core, sp_call_id call, sp_call_info *out) {
    SP_ENTRY(core);
    if (!out)
        return SP_ERR_INVALID_ARG;
    int native = PJSUA_INVALID_ID;
    if (!native_of(core, call, &native))
        return not_found(core, call);

    pjsua_call_info ci;
    if (pjsua_call_get_info(native, &ci) != PJ_SUCCESS)
        return SP_ERR_NOT_FOUND;
    char codec[64] = "";
    if (ci.state == PJSIP_INV_STATE_CONFIRMED)
        stream_codec(native, codec, sizeof codec);

    memset(out, 0, sizeof *out);
    switch (ci.state) {
    case PJSIP_INV_STATE_NULL:         out->state = SP_CALL_NULL; break;
    case PJSIP_INV_STATE_CALLING:      out->state = SP_CALL_CALLING; break;
    case PJSIP_INV_STATE_INCOMING:     out->state = SP_CALL_INCOMING; break;
    case PJSIP_INV_STATE_EARLY:        out->state = SP_CALL_EARLY; break;
    case PJSIP_INV_STATE_CONNECTING:   out->state = SP_CALL_CONNECTING; break;
    case PJSIP_INV_STATE_CONFIRMED:    out->state = SP_CALL_CONFIRMED; break;
    case PJSIP_INV_STATE_DISCONNECTED: out->state = SP_CALL_DISCONNECTED; break;
    }
    out->last_code = ci.last_status;
    out->duration_s = ci.connect_duration.sec;

    sp_call_info_view *v = &core->call_info_view;
    pthread_mutex_lock(&g_rec_lock);
    sp_call_rec *rec = rec_of(core, call, NULL);
    if (!rec) {
        pthread_mutex_unlock(&g_rec_lock);
        return not_found(core, call);
    }
    out->acc = rec->acc;
    out->on_hold = rec->on_hold;
    out->recording = rec->recorder != PJSUA_INVALID_ID ||
                     rec->rec_port != NULL;
    out->inbound = rec->inbound;
    /* Refresh before copying out, so a caller that asks before any
       state event still gets the current identity. */
    refresh_party(rec, &ci, codec);
    memcpy(v->remote_uri, rec->remote_uri, sizeof v->remote_uri);
    memcpy(v->local_uri, rec->local_uri, sizeof v->local_uri);
    memcpy(v->codec, rec->codec, sizeof v->codec);
    v->remote = rec->remote;
    pthread_mutex_unlock(&g_rec_lock);

    out->remote_uri = v->remote_uri;
    out->remote_display = v->remote.display;
    out->remote_user = v->remote.user;
    out->remote_label = sp_sipuri_label(&v->remote);
    out->local_uri = v->local_uri;
    out->codec = v->codec[0] ? v->codec : NULL;
    return SP_OK;
}

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

/* The SDP index of the call's audio stream, or -1. pjsua indexes
   stream statistics by media index, not by stream ordinal, so a call
   that negotiated video or T.140 alongside audio would otherwise be
   read at the wrong index. */
static int audio_med_idx(const pjsua_call_info *ci) {
    for (unsigned i = 0; i < ci->media_cnt; i++)
        if (ci->media[i].type == PJMEDIA_TYPE_AUDIO)
            return (int)ci->media[i].index;
    return -1;
}

static void metric_from(sp_metric_us *out, const pj_math_stat *in) {
    /* n is what says whether any of this means anything: pjmedia
       leaves min at a sentinel until the first sample arrives. */
    if (in->n <= 0) {
        memset(out, 0, sizeof *out);
        return;
    }
    out->cur_us = (uint32_t)(in->last < 0 ? 0 : in->last);
    out->min_us = (uint32_t)(in->min  < 0 ? 0 : in->min);
    out->max_us = (uint32_t)(in->max  < 0 ? 0 : in->max);
    out->avg_us = (uint32_t)(in->mean < 0 ? 0 : in->mean);
}

/* Fills q from an RTCP snapshot and a jitter-buffer state. Split out
   because two callers produce those: the periodic sampler, which asks
   pjsua by call id, and on_stream_destroyed, which holds the stream
   directly and is the only place a final sample can be taken. */
static void quality_from(sp_call_quality *q, const pjmedia_rtcp_stat *r,
                         const pjmedia_jb_state *jb, bool final) {
    memset(q, 0, sizeof *q);
    q->valid = true;
    q->final = final;
    q->sampled_us = now_us();

    q->rx_packets    = r->rx.pkt;
    q->tx_packets    = r->tx.pkt;
    q->rx_bytes      = r->rx.bytes;
    q->tx_bytes      = r->tx.bytes;
    q->rx_lost       = r->rx.loss;
    q->tx_lost       = r->tx.loss;
    q->rx_discarded  = r->rx.discard;
    q->rx_reordered  = r->rx.reorder;
    q->rx_duplicated = r->rx.dup;

    q->rx_loss_pct = sp_qual_loss_pct(r->rx.loss, r->rx.pkt);
    q->tx_loss_pct = sp_qual_loss_pct(r->tx.loss, r->tx.pkt);

    metric_from(&q->rx_jitter, &r->rx.jitter);
    metric_from(&q->tx_jitter, &r->tx.jitter);
    metric_from(&q->rtt, &r->rtt);

    if (jb) {
        q->jb_size_frames     = jb->size;
        q->jb_prefetch_frames = jb->prefetch;
        q->jb_avg_delay_ms    = jb->avg_delay;
        q->jb_min_delay_ms    = jb->min_delay;
        q->jb_max_delay_ms    = jb->max_delay;
        q->jb_lost            = jb->lost;
        q->jb_discarded       = jb->discard;
        q->jb_empty           = jb->empty;
    }

    /* The estimate uses the average jitter rather than the last
       sample: one spike should not move the score. */
    sp_qual_estimate(q->rx_loss_pct, q->rtt.avg_us, q->rx_jitter.avg_us,
                     &q->r_factor, &q->mos);
}

/* Reads the call's live stream into fresh, then leaves the record's
   own sample -- refreshed with it when there was one -- in out. False
   when the call is gone. Silent when there is no stream to read: a call
   that has no media yet is not an error.
 *
 * A final sample is the last word on its stream, and is kept until the
 * media comes up again (on_call_media_state): a tick in between must
 * not replace it with a read of a stream that is going away. */
static bool sample_quality(sp_core *core, sp_call_id call,
                           sp_call_quality *out) {
    int native = PJSUA_INVALID_ID;
    if (!native_of(core, call, &native))
        return false;

    sp_call_quality fresh;
    bool have = false;
    pjsua_call_info ci;
    if (native != PJSUA_INVALID_ID
        && pjsua_call_get_info(native, &ci) == PJ_SUCCESS) {
        int idx = audio_med_idx(&ci);
        pjsua_stream_stat ss;
        if (idx >= 0
            && pjsua_call_get_stream_stat(native, (unsigned)idx, &ss)
                   == PJ_SUCCESS) {
            quality_from(&fresh, &ss.rtcp, &ss.jbuf, false);
            have = true;
        }
    }

    pthread_mutex_lock(&g_rec_lock);
    sp_call_rec *rec = rec_of(core, call, NULL);
    if (rec) {
        if (have && !rec->quality.final)
            rec->quality = fresh;
        *out = rec->quality;
    }
    pthread_mutex_unlock(&g_rec_lock);
    return rec != NULL;
}

/* What the bridge is carrying right now, per direction.
 *
 * Deliberately not gated on the audio_quality module: that module is
 * about RTCP statistics and the figures kept in history. This is the
 * live signal, it is what the meter on the call panel draws, and a
 * phone that cannot show whether audio is arriving is a worse phone
 * regardless of whether anyone wants the statistics.
 */
sp_status sp_call_get_signal(sp_core *core, sp_call_id call,
                             sp_signal_level *out) {
    SP_ENTRY(core);
    if (!out)
        return SP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    int native = PJSUA_INVALID_ID;
    if (!native_of(core, call, &native))
        return not_found(core, call);

    pjsua_call_info ci;
    if (native == PJSUA_INVALID_ID
        || pjsua_call_get_info(native, &ci) != PJ_SUCCESS
        || ci.conf_slot == PJSUA_INVALID_ID) {
        /* Between answering and media coming up there is no slot.
           A normal moment, not a fault: valid says so. */
        return SP_OK;
    }

    unsigned tx = 0, rx = 0;
    if (pjsua_conf_get_signal_level(ci.conf_slot, &tx, &rx) == PJ_SUCCESS) {
        out->tx = tx;
        out->rx = rx;
        out->valid = true;
    }
    return SP_OK;
}

sp_status sp_call_get_quality(sp_core *core, sp_call_id call,
                              sp_call_quality *out) {
    if (core && !sp_config_module_enabled(core->cfg,
                                          SP_MODULE_AUDIO_QUALITY)) {
        sp_core_detail(core, "the audio quality module is switched off");
        return SP_ERR_DISABLED;
    }

    SP_ENTRY(core);
    if (!out)
        return SP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    if (!sample_quality(core, call, out))
        return not_found(core, call);
    return SP_OK;
}

/* One sample per live call, then reschedule. Fires on a pjsua timer
   thread. */
static void metrics_tick(pj_timer_heap_t *th, pj_timer_entry *e) {
    (void)th;
    sp_core *core = (sp_core *)e->user_data;
    if (!core)
        return;
    /* Same reason as the SIP capture: the timer was armed when the
       module was on, and switching it off has to stop the sampling
       now rather than at the next start. */
    if (!sp_config_module_enabled(core->cfg, SP_MODULE_AUDIO_QUALITY))
        return;

    /* Only the headline numbers are carried out of the locked section:
       sp_core_post runs the event queue, and holding core->lock across
       it is how this project deadlocked itself once already. Copying a
       whole sp_event per call would also put several kilobytes on a
       pjsua worker stack. */
    struct {
        sp_call_id call;
        double     mos;
        double     loss;
        uint32_t   jitter_us;
        uint32_t   rtt_us;
        bool       final;
    } snap[PJSUA_MAX_CALLS];
    size_t n_snap = 0;
    int interval = 0;

    pthread_mutex_lock(&core->lock);
    if (!core->metrics_on) {
        pthread_mutex_unlock(&core->lock);
        return;
    }
    interval = core->cfg->metrics_interval_ms;

    pthread_mutex_unlock(&core->lock);

    uint64_t ids[PJSUA_MAX_CALLS];
    size_t n = 0;
    sp_ht_snapshot(core->calls, ids, PJSUA_MAX_CALLS, &n);
    if (n > PJSUA_MAX_CALLS)
        n = PJSUA_MAX_CALLS;
    for (size_t i = 0; i < n; i++) {
        sp_call_quality q;
        if (!sample_quality(core, ids[i], &q) || !q.valid)
            continue;
        snap[n_snap].call      = ids[i];
        snap[n_snap].mos       = q.mos;
        snap[n_snap].loss      = q.rx_loss_pct;
        snap[n_snap].jitter_us = q.rx_jitter.avg_us;
        snap[n_snap].rtt_us    = q.rtt.avg_us;
        snap[n_snap].final     = q.final;
        n_snap++;
    }

    for (size_t i = 0; i < n_snap; i++) {
        sp_event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = SP_EV_CALL_QUALITY;
        ev.u.quality.call         = snap[i].call;
        ev.u.quality.mos          = snap[i].mos;
        ev.u.quality.rx_loss_pct  = snap[i].loss;
        ev.u.quality.rx_jitter_us = snap[i].jitter_us;
        ev.u.quality.rtt_us       = snap[i].rtt_us;
        ev.u.quality.final        = snap[i].final;
        sp_core_post(core, &ev);
    }

    pj_time_val delay = { interval / 1000, interval % 1000 };
    if (pjsua_schedule_timer(&core->metrics_timer, &delay) != PJ_SUCCESS) {
        pthread_mutex_lock(&core->lock);
        core->metrics_on = false;
        pthread_mutex_unlock(&core->lock);
    }
}

sp_status sp_metrics_start(sp_core *core) {
    if (!core)
        return SP_ERR_INVALID_ARG;
    /* Off means no timer, not a timer whose samples are discarded. */
    if (!sp_config_module_enabled(core->cfg, SP_MODULE_AUDIO_QUALITY))
        return SP_OK;
    int interval = core->cfg->metrics_interval_ms;
    if (interval <= 0)
        return SP_OK;

    pj_timer_entry_init(&core->metrics_timer, 0, core, &metrics_tick);
    pj_time_val delay = { interval / 1000, interval % 1000 };

    pthread_mutex_lock(&core->lock);
    core->metrics_on = true;
    pthread_mutex_unlock(&core->lock);

    if (pjsua_schedule_timer(&core->metrics_timer, &delay) != PJ_SUCCESS) {
        pthread_mutex_lock(&core->lock);
        core->metrics_on = false;
        pthread_mutex_unlock(&core->lock);
        return sp_core_fail(core, PJ_EUNKNOWN, SP_ERR_INTERNAL,
                            "pjsua_schedule_timer");
    }
    return SP_OK;
}

void sp_metrics_stop(sp_core *core) {
    if (!core)
        return;
    pthread_mutex_lock(&core->lock);
    bool was_on = core->metrics_on;
    core->metrics_on = false;
    pthread_mutex_unlock(&core->lock);
    if (!was_on)
        return;
    /* Taking the lock above waited out any fire already in progress,
       so after this the entry cannot be rescheduled. */
    pjsua_cancel_timer(&core->metrics_timer);
}

sp_status sp_call_list(sp_core *core, sp_call_id *ids, size_t cap,
                       size_t *n_out) {
    SP_ENTRY(core);
    if (!n_out)
        return SP_ERR_INVALID_ARG;
    sp_ht_snapshot(core->calls, ids, cap, n_out);
    return SP_OK;
}

/* --- pjsua callbacks ------------------------------------------------ */

static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id,
                             pjsip_rx_data *rdata) {
    (void)rdata;
    sp_core *core = sp_core_active();
    if (!core)
        return;

    sp_call_rec *rec = calloc(1, sizeof *rec);
    if (!rec) {
        pjsua_call_hangup(call_id, 500, NULL, NULL);
        return;
    }
    rec->native = call_id;
    rec->acc = sp_ht_by_native(core->accounts, acc_id);
    rec->inbound = true;
    rec->recorder = PJSUA_INVALID_ID;
    rec->rec_slot = PJSUA_INVALID_ID;

    pjsua_call_info ci;
    char from[256] = {0}, local[256] = {0}, cid[256] = {0};
    if (pjsua_call_get_info(call_id, &ci) == PJ_SUCCESS) {
        snprintf(from, sizeof from, "%.*s", (int)ci.remote_info.slen,
                 ci.remote_info.ptr);
        snprintf(local, sizeof local, "%.*s", (int)ci.local_info.slen,
                 ci.local_info.ptr);
        snprintf(cid, sizeof cid, "%.*s", (int)ci.call_id.slen,
                 ci.call_id.ptr);
    }
    snprintf(rec->remote_uri, sizeof rec->remote_uri, "%s", from);
    snprintf(rec->local_uri, sizeof rec->local_uri, "%s", local);
    sp_sipuri_parse(rec->remote_uri, &rec->remote);
    rec->history_id = sp_history_begin(core->history, true,
                                       sp_account_id_of(core, rec->acc),
                                       from, local, cid);

    /* Complete before it is in the table, where an API thread can
       find it. */
    pthread_mutex_lock(&g_rec_lock);
    uint64_t handle = sp_ht_add(core->calls, call_id, rec);
    rec->handle = handle;
    pthread_mutex_unlock(&g_rec_lock);
    if (handle == 0) {
        if (rec->history_id > 0)
            sp_history_ended(core->history, rec->history_id, 500, 0);
        free(rec);
        pjsua_call_hangup(call_id, 500, NULL, NULL);
        return;
    }
    sp_transcriber_call_started(core->transcriber);
    pjsua_call_set_user_data(call_id, rec);

    /* Ring. Before the event is posted, so the phone is audible even
       if a frontend is slow to draw its call display. */
    sp_sound_start(core, SP_SOUND_RINGTONE);

    /* A call that is merely ringing already means the operator is
       about to answer it; any leftover playback from before this
       call arrived must be silent by the time they do. */
    sp_playback_stop(core);

    sp_event ev = {0};
    ev.type = SP_EV_INCOMING_CALL;
    ev.u.incoming.call = handle;
    ev.u.incoming.acc = sp_ht_by_native(core->accounts, acc_id);
    ev.u.incoming.from_uri = from;
    sp_core_post(core, &ev);
}

/* Refreshes the cached identity and codec. Called on every state
   change, because remote_info is not final until the dialog is. The
   codec is the caller's to fetch (stream_codec), being a pjsua call.
   Caller holds g_rec_lock. */
static void refresh_party(sp_call_rec *rec, const pjsua_call_info *ci,
                          const char *codec) {
    if (!rec || !ci)
        return;
    if (ci->remote_info.slen > 0)
        snprintf(rec->remote_uri, sizeof rec->remote_uri, "%.*s",
                 (int)ci->remote_info.slen, ci->remote_info.ptr);
    if (ci->local_info.slen > 0)
        snprintf(rec->local_uri, sizeof rec->local_uri, "%.*s",
                 (int)ci->local_info.slen, ci->local_info.ptr);
    sp_sipuri_parse(rec->remote_uri, &rec->remote);

    if (codec && *codec && !rec->codec[0])
        snprintf(rec->codec, sizeof rec->codec, "%s", codec);
}

/* user_data is authoritative: it is set before the native id is
   known. sp_ht_by_native is the fallback for anything pjsua created
   without going through us. */
static sp_call_rec *rec_from_native(sp_core *core, pjsua_call_id call_id,
                                    uint64_t *handle_out) {
    sp_call_rec *rec = pjsua_call_get_user_data(call_id);
    if (rec && rec->handle) {
        void *check = NULL;
        if (sp_ht_lookup(core->calls, rec->handle, NULL, &check) == 0 &&
            check == rec) {
            if (handle_out)
                *handle_out = rec->handle;
            return rec;
        }
    }
    uint64_t h = sp_ht_by_native(core->calls, call_id);
    if (h == 0)
        return NULL;
    void *data = NULL;
    if (sp_ht_lookup(core->calls, h, NULL, &data) != 0)
        return NULL;
    if (handle_out)
        *handle_out = h;
    return data;
}

static sp_call_state map_state(pjsip_inv_state st) {
    switch (st) {
    case PJSIP_INV_STATE_CALLING:      return SP_CALL_CALLING;
    case PJSIP_INV_STATE_INCOMING:     return SP_CALL_INCOMING;
    case PJSIP_INV_STATE_EARLY:        return SP_CALL_EARLY;
    case PJSIP_INV_STATE_CONNECTING:   return SP_CALL_CONNECTING;
    case PJSIP_INV_STATE_CONFIRMED:    return SP_CALL_CONFIRMED;
    case PJSIP_INV_STATE_DISCONNECTED: return SP_CALL_DISCONNECTED;
    case PJSIP_INV_STATE_NULL:         break;
    }
    return SP_CALL_NULL;
}

/* The one go auto-record has at a call: answered, with its audio in
   the bridge, and not already recording. Called from both callbacks,
   because which of "confirmed" and "media active" comes last depends
   on whether the far end sent early media. */
static void maybe_auto_record(sp_core *core, sp_call_rec *rec,
                              uint64_t handle, const pjsua_call_info *ci) {
    if (!rec || rec->auto_record_tried || !sp_recording_get_auto(core))
        return;
    if (ci->state != PJSIP_INV_STATE_CONFIRMED
        || ci->media_status != PJSUA_CALL_MEDIA_ACTIVE
        || ci->conf_slot == PJSUA_INVALID_ID)
        return;
    rec->auto_record_tried = true;
    pthread_mutex_lock(&g_rec_lock);
    const bool busy = rec->recorder != PJSUA_INVALID_ID || rec->rec_port;
    pthread_mutex_unlock(&g_rec_lock);
    if (busy)
        return;
    char path[sizeof core->rec_path_view];
    if (record_start_named(core, handle, path, sizeof path) == SP_OK)
        sp_actionlog_add(core, 1, "recording the call automatically");
    else
        sp_actionlog_add(core, 2, "automatic recording could not start");
}

/* A call's capture, and who is using it.
 *
 * rec->cap is set and used from pjsip's thread and taken away from
 * whichever thread ends the call or switches the module off -- the UI
 * thread, when hangup or the module switch comes from there -- and the
 * record is freed once it has left the table. g_rec_lock covers only
 * the handoff: no pjsua call is ever made while holding it, because
 * pjsua takes PJSUA_LOCK inside its own calls and may hold it while
 * calling on_call_state, which takes this lock. A thread that takes a
 * capture away while another is using it leaves the stop to the user,
 * which does it on its way out. One core per process (core.c), so one
 * lock. */
struct sp_cap_ref {
    sp_transcap *cap;
    int          users;
    bool         stop_pending;
    char         label[128];
};

/* Under g_rec_lock: takes the capture off the record. Returns it if the
   caller is to stop it now, NULL if a user will. */
static struct sp_cap_ref *detach_locked(sp_call_rec *rec) {
    struct sp_cap_ref *r = rec->cap;
    if (!r)
        return NULL;
    rec->cap = NULL;
    /* The name the call screen showed for them. */
    snprintf(r->label, sizeof r->label, "%s", sp_sipuri_label(&rec->remote));
    if (r->users > 0) {
        r->stop_pending = true;
        return NULL;
    }
    return r;
}

static void stop_ref(struct sp_cap_ref *r) {
    if (!r)
        return;
    sp_transcap_stop(r->cap, r->label);
    free(r);
}

/* Once per call, on the same rule as auto-record. Without a model or
   an engine nothing is captured at all: audio written to disk for a
   transcript that cannot happen is audio kept for no reason. Once
   capturing, the microphone side follows the call: heard while its
   media is active, silent while it is held. */
static void maybe_capture(sp_core *core, sp_call_rec *rec,
                          const pjsua_call_info *ci) {
    if (!rec || rec->history_id <= 0)
        return;
    const bool live = ci->media_status == PJSUA_CALL_MEDIA_ACTIVE
                      && ci->conf_slot != PJSUA_INVALID_ID;

    pthread_mutex_lock(&g_rec_lock);
    struct sp_cap_ref *r = rec->cap;
    if (r)
        r->users++;
    pthread_mutex_unlock(&g_rec_lock);
    if (r) {
        if (live)
            sp_transcap_follow(r->cap, ci->conf_slot);
        sp_transcap_hear_mic(r->cap, live);
        pthread_mutex_lock(&g_rec_lock);
        const bool stop = --r->users == 0 && r->stop_pending;
        pthread_mutex_unlock(&g_rec_lock);
        if (stop)
            stop_ref(r);
        return;
    }

    if (!live || rec->transcript_tried
        || !sp_config_module_enabled(core->cfg, SP_MODULE_TRANSCRIPT))
        return;
    rec->transcript_tried = true;
    char model[4096];
    const char *why = NULL;
    if (!sp_transcriber_has_engine(core->transcriber))
        why = "this build has no speech engine; the call is not transcribed";
    else if (sp_transcript_get_model(core, model, sizeof model) != SP_OK
             || !*model)
        why = "no speech model; the call is not transcribed";
    if (why) {
        sp_history_transcript_failed(core->history, rec->history_id, why);
        sp_actionlog_add(core, 2, why);
        return;
    }
    struct sp_cap_ref *made = calloc(1, sizeof *made);
    if (made)
        made->cap = sp_transcap_start(core, rec->history_id, ci->conf_slot);
    if (!made || !made->cap) {
        free(made);
        sp_actionlog_add(core, 2, "the transcript capture could not start");
        return;
    }
    pthread_mutex_lock(&g_rec_lock);
    rec->cap = made;
    pthread_mutex_unlock(&g_rec_lock);
    sp_history_transcript_state(core->history, rec->history_id,
                                SP_TRANSCRIPT_PENDING);
}

void sp_call_stop_captures(sp_core *core) {
    struct sp_cap_ref *now[PJSUA_MAX_CALLS];
    size_t n_now = 0;
    uint64_t ids[PJSUA_MAX_CALLS];
    size_t n = 0;

    /* A record found here cannot be freed before the lock is let go:
       on_call_state retires it under the same lock first. */
    pthread_mutex_lock(&g_rec_lock);
    sp_ht_snapshot(core->calls, ids, PJSUA_MAX_CALLS, &n);
    if (n > PJSUA_MAX_CALLS)
        n = PJSUA_MAX_CALLS;
    for (size_t i = 0; i < n; i++) {
        void *data = NULL;
        if (sp_ht_lookup(core->calls, ids[i], NULL, &data) != 0 || !data)
            continue;
        struct sp_cap_ref *r = detach_locked((sp_call_rec *)data);
        if (r)
            now[n_now++] = r;
    }
    pthread_mutex_unlock(&g_rec_lock);

    for (size_t i = 0; i < n_now; i++)
        stop_ref(now[i]);
}

pjsua_conf_port_id sp_call_capture_mic_slot(sp_core *core, sp_call_id call) {
    pjsua_conf_port_id slot = PJSUA_INVALID_ID;
    pthread_mutex_lock(&g_rec_lock);
    void *data = NULL;
    if (sp_ht_lookup(core->calls, call, NULL, &data) == 0 && data) {
        const sp_call_rec *rec = data;
        if (rec->cap)
            slot = sp_transcap_mic_slot(rec->cap->cap);
    }
    pthread_mutex_unlock(&g_rec_lock);
    return slot;
}

static void on_call_state(pjsua_call_id call_id, pjsip_event *e) {
    (void)e;
    sp_core *core = sp_core_active();
    if (!core)
        return;
    uint64_t handle = 0;
    sp_call_rec *rec = rec_from_native(core, call_id, &handle);
    if (handle == 0)
        return;

    pjsua_call_info ci;
    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS)
        return;

    char codec[64] = "";
    if (ci.state == PJSIP_INV_STATE_CONFIRMED)
        stream_codec(call_id, codec, sizeof codec);

    /* This thread is the one that frees the record, below, so it may
       keep the pointer; the lock is for the API threads reading the
       fields meanwhile. */
    sp_call_quality quality = {0};
    if (rec) {
        pthread_mutex_lock(&g_rec_lock);
        rec->last_code = ci.last_status;
        refresh_party(rec, &ci, codec);
        quality = rec->quality;
        pthread_mutex_unlock(&g_rec_lock);
    }

    if (rec && rec->history_id > 0) {
        /* The Call-ID exists as soon as the INVITE does, so fill it in
           at the first opportunity; the update is a no-op afterwards. */
        if (ci.call_id.slen > 0) {
            char cid[256];
            snprintf(cid, sizeof cid, "%.*s", (int)ci.call_id.slen,
                     ci.call_id.ptr);
            sp_history_set_call_id(core->history, rec->history_id, cid);
        }
        if (ci.state == PJSIP_INV_STATE_CONFIRMED)
            sp_history_answered(core->history, rec->history_id,
                                codec[0] ? codec : NULL);
        if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
            /* The snapshot was taken in on_stream_destroyed, which has
               already run by the time DISCONNECTED arrives; this only
               persists what is cached. Sampling here instead would
               read a stream that no longer exists. */
            sp_history_quality(core->history, rec->history_id, &quality);
            sp_history_ended(core->history, rec->history_id,
                             (int)ci.last_status,
                             (uint32_t)ci.connect_duration.sec);
        }
    }

    /* Ringback, and knowing when to shut up.
       Only for an outbound call that is ringing without early media:
       when the far end sends its own ringing audio, playing ours on
       top of it is two ringbacks at once. */
    if (rec && !rec->inbound) {
        const bool ringing = ci.state == PJSIP_INV_STATE_EARLY ||
                             ci.state == PJSIP_INV_STATE_CALLING;
        const bool early_media =
            ci.media_status == PJSUA_CALL_MEDIA_ACTIVE ||
            ci.media_status == PJSUA_CALL_MEDIA_REMOTE_HOLD;
        if (ringing && !early_media)
            sp_sound_start(core, SP_SOUND_RINGBACK);
        else
            sp_sound_stop(core, SP_SOUND_RINGBACK);
    }
    maybe_auto_record(core, rec, handle, &ci);
    if (ci.state == PJSIP_INV_STATE_CONFIRMED ||
        ci.state == PJSIP_INV_STATE_DISCONNECTED) {
        sp_sound_stop(core, SP_SOUND_RINGTONE);
        sp_sound_stop(core, SP_SOUND_RINGBACK);
    }
    /* Any call existing at all is the hazard sp_playback_start refuses
       to create; this stops one that was already running before this
       call came up, on every state change rather than only once
       confirmed, so it never becomes audible into a live microphone.
       Cheap when nothing is loaded: see sp_playback_stop. */
    sp_playback_stop(core);
    /* A call that ended without ever being answered: rejected, busy,
       or nobody there. The one case where a tone tells the operator
       something they cannot see. */
    if (ci.state == PJSIP_INV_STATE_DISCONNECTED &&
        ci.connect_duration.sec == 0 && ci.connect_duration.msec == 0)
        sp_sound_start(core, SP_SOUND_NOTIFICATION);

    sp_event ev = {0};
    ev.type = SP_EV_CALL_STATE;
    ev.u.call_state.call = handle;
    ev.u.call_state.state = map_state(ci.state);
    ev.u.call_state.last_code = ci.last_status;
    sp_core_post(core, &ev);

    if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
        /* Retire after posting, so the frontend can still resolve the
           call that just ended. Destroy the recorder here or a call
           that ends mid-recording leaks the writer -- and, for Opus,
           leaves the file with no end-of-stream page. */
        /* The recorder and the capture are taken and the record retired
           under g_rec_lock, so another thread finds either all of it or
           none; the stops and the free come after. */
        rec_writer w = { PJSUA_INVALID_ID, NULL, NULL, PJSUA_INVALID_ID };
        pthread_mutex_lock(&g_rec_lock);
        struct sp_cap_ref *cap = rec ? detach_locked(rec) : NULL;
        if (rec)
            w = writer_take(rec);
        sp_ht_retire(core->calls, handle);
        pthread_mutex_unlock(&g_rec_lock);
        writer_destroy(&w);
        stop_ref(cap);
        free(rec);
    }
}

static void on_call_media_state(pjsua_call_id call_id) {
    sp_core *core = sp_core_active();
    if (!core)
        return;
    /* Media is up, so nothing is ringing any more. Belt as well as
       braces: on_call_state covers the usual path, but early media on
       an outbound call arrives here first. */
    sp_sound_stop(core, SP_SOUND_RINGBACK);
    uint64_t handle = 0;
    sp_call_rec *rec = rec_from_native(core, call_id, &handle);
    if (handle == 0)
        return;

    pjsua_call_info ci;
    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS)
        return;

    bool active = (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE);
    if (active && ci.conf_slot != PJSUA_INVALID_ID) {
        pjsua_conf_connect(ci.conf_slot, 0);
        pjsua_conf_connect(0, ci.conf_slot);
    }
    if (rec) {
        pthread_mutex_lock(&g_rec_lock);
        /* What the far end agreed to, which a refused re-INVITE leaves
           as it was whatever sp_call_set_hold assumed. */
        if (active)
            rec->on_hold = false;
        else if (ci.media_status == PJSUA_CALL_MEDIA_LOCAL_HOLD)
            rec->on_hold = true;
        /* A stream that came back after a hold or a re-INVITE is
           sampled again; the final sample of the one before it has
           been reported. */
        if (active)
            rec->quality.final = false;
        pthread_mutex_unlock(&g_rec_lock);
    }
    maybe_auto_record(core, rec, handle, &ci);
    maybe_capture(core, rec, &ci);

    sp_event ev = {0};
    ev.type = SP_EV_CALL_MEDIA_STATE;
    ev.u.media.call = handle;
    ev.u.media.active = active;
    sp_core_post(core, &ev);
}

static void on_dtmf_digit(pjsua_call_id call_id, int digit) {
    sp_core *core = sp_core_active();
    if (!core)
        return;
    uint64_t handle = sp_ht_by_native(core->calls, call_id);
    if (handle == 0)
        return;
    sp_event ev = {0};
    ev.type = SP_EV_DTMF_RECEIVED;
    ev.u.dtmf.call = handle;
    ev.u.dtmf.digit = (char)digit;
    sp_core_post(core, &ev);
}

static void on_call_transfer_status(pjsua_call_id call_id, int st_code,
                                    const pj_str_t *st_text, pj_bool_t final,
                                    pj_bool_t *p_cont) {
    (void)final;
    (void)p_cont;
    sp_core *core = sp_core_active();
    if (!core)
        return;
    uint64_t handle = sp_ht_by_native(core->calls, call_id);
    if (handle == 0)
        return;
    char reason[192] = {0};
    if (st_text && st_text->slen > 0)
        snprintf(reason, sizeof reason, "%.*s", (int)st_text->slen,
                 st_text->ptr);
    sp_event ev = {0};
    ev.type = SP_EV_XFER_STATUS;
    ev.u.xfer.call = handle;
    ev.u.xfer.code = st_code;
    ev.u.xfer.reason = reason[0] ? reason : NULL;
    sp_core_post(core, &ev);
}

/* pjsua calls this once the audio session has left the conference
   bridge and immediately before the stream is destroyed. It is the
   last moment the RTCP counters exist: by the time the application
   sees PJSIP_INV_STATE_DISCONNECTED they are gone, so a snapshot taken
   there would be empty. Same shape as the two bugs this project has
   already had, where a callback fires earlier than intuition says. */
static void on_stream_destroyed(pjsua_call_id call_id, pjmedia_stream *strm,
                                unsigned stream_idx) {
    (void)stream_idx;
    sp_core *core = sp_core_active();
    if (!core || !strm)
        return;

    pjmedia_rtcp_stat rtcp;
    if (pjmedia_stream_get_stat(strm, &rtcp) != PJ_SUCCESS)
        return;
    pjmedia_jb_state jb;
    bool have_jb = pjmedia_stream_get_stat_jbuf(strm, &jb) == PJ_SUCCESS;

    sp_call_quality last;
    quality_from(&last, &rtcp, have_jb ? &jb : NULL, true);

    /* g_rec_lock, a leaf, and not core->lock: pjsua holds PJSUA_LOCK
       across this callback, and core->lock is held elsewhere across
       calls into pjsua. */
    pthread_mutex_lock(&g_rec_lock);
    sp_call_rec *rec = rec_from_native(core, call_id, NULL);
    if (rec)
        rec->quality = last;
    pthread_mutex_unlock(&g_rec_lock);
}

void sp_call_install_callbacks(pjsua_callback *cb) {
    cb->on_incoming_call = &on_incoming_call;
    cb->on_call_state = &on_call_state;
    cb->on_call_media_state = &on_call_media_state;
    cb->on_dtmf_digit = &on_dtmf_digit;
    cb->on_call_transfer_status = &on_call_transfer_status;
    cb->on_stream_destroyed = &on_stream_destroyed;
}

/* What three keystrokes mean. Kept here rather than in a frontend
   because it is a product decision, and because a second frontend
   deciding it differently is how one platform dials a different
   extension from the other. */
sp_status sp_call_uri_complete(sp_core *core, sp_account_id acc,
                               const char *input, char *out, size_t outlen) {
    if (!input || !out || outlen == 0)
        return SP_ERR_INVALID_ARG;
    *out = '\0';

    while (*input == ' ' || *input == '\t')
        input++;
    size_t n = strlen(input);
    while (n > 0 && (input[n - 1] == ' ' || input[n - 1] == '\t'))
        n--;
    if (n == 0)
        return SP_ERR_INVALID_ARG;
    /* A transfer target goes into Refer-To as it is, where a line
       break would start a header of its own. */
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)input[i] < 0x20 || input[i] == 0x7f)
            return SP_ERR_INVALID_ARG;

    /* A truncated URI dials somebody else, so every path below
       refuses rather than writes a prefix. */
    const char *fmt;
    const char *domain = NULL;
    if ((n > 4 && strncmp(input, "sip:", 4) == 0)
        || (n > 5 && strncmp(input, "sips:", 5) == 0)) {
        fmt = "%.*s";
    } else if (memchr(input, '@', n)) {
        fmt = "sip:%.*s";
    } else {
        domain = sp_account_domain_of(core, acc);
        if (!domain)
            return SP_ERR_NOT_FOUND;
        fmt = NULL;
    }

    int wrote;
    if (fmt) {
        wrote = snprintf(out, outlen, fmt, (int)n, input);
    } else {
        /* What was typed is the user part, and "#" -- as in "*72#" --
           is not allowed bare in one: pjsip refuses the URI. */
        char user[384];
        size_t u = 0;
        for (size_t i = 0; i < n; i++) {
            const unsigned char c = (unsigned char)input[i];
            const bool plain = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
                               || (c >= 'a' && c <= 'z')
                               || strchr("-_.!~*'()&=+$,;?/", c);
            if (u + 4 > sizeof user)
                return SP_ERR_INVALID_ARG;
            if (plain)
                user[u++] = (char)c;
            else
                u += (size_t)snprintf(user + u, 4, "%%%02X", c);
        }
        user[u] = '\0';
        wrote = snprintf(out, outlen, "sip:%s@%s", user, domain);
    }
    if (wrote < 0 || (size_t)wrote >= outlen) {
        *out = '\0';
        return SP_ERR_INVALID_ARG;
    }
    return SP_OK;
}
