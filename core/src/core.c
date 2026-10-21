#include "core_internal.h"
#include "modules.h"
#include "actionlog.h"
#include "contacts.h"
#include "history.h"
#include "transcriber.h"
#include "theme.h"
#include "siplog.h"
#include "store.h"
#include "paths.h"
#include "traceexport.h"
#include "sipuri.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* pjsua is a process-wide singleton and pjsip_module carries no user
   data pointer, so exactly one core may be running per process. */
static sp_core        *g_core;
static pthread_mutex_t g_core_lock = PTHREAD_MUTEX_INITIALIZER;

sp_core *sp_core_active(void) {
    return g_core;
}

sp_status sp_core_fail(sp_core *core, pj_status_t st, sp_status mapped,
                       const char *what) {
    char pjmsg[PJ_ERR_MSG_SIZE] = {0};
    pj_strerror(st, pjmsg, sizeof pjmsg);
    /* sp_core_detail takes core->lock itself; locking here too would
       self-deadlock on the non-recursive mutex. */
    sp_core_detail(core, "%s: %s (pj %d)", what ? what : "pjsua", pjmsg,
                   (int)st);
    return mapped;
}

void sp_core_detail(sp_core *core, const char *fmt, ...) {
    if (!core)
        return;
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&core->lock);
    vsnprintf(core->err_detail, sizeof core->err_detail, fmt, ap);
    pthread_mutex_unlock(&core->lock);
    va_end(ap);
}

static void set_detail(sp_core *core, const char *msg) {
    sp_core_detail(core, "%s", msg);
}

void sp_core_post(sp_core *core, const sp_event *ev) {
    if (!core)
        return;
    /* Persisted before it is queued, so the history is complete even
       if the frontend never drains the queue. The wording lives in
       actionlog.c so every frontend shows the same text. */
    if (core->actionlog) {
        char line[512];
        if (sp_actionlog_format(ev, line, sizeof line))
            sp_actionlog_write(core->actionlog, 3, line);
    }
    if (core->evq)
        sp_evq_post(core->evq, ev);
}

/* Returns a per-thread snapshot rather than the live buffer: the
   buffer is written by pjsip worker threads, so handing out a pointer
   to it would let a caller read a half-written string. The snapshot is
   valid until the same thread calls this again. */
const char *sp_last_error_detail(sp_core *core) {
    static __thread char snapshot[512];
    if (!core)
        return "";
    pthread_mutex_lock(&core->lock);
    memcpy(snapshot, core->err_detail, sizeof snapshot);
    pthread_mutex_unlock(&core->lock);
    snapshot[sizeof snapshot - 1] = '\0';
    return snapshot;
}

/* One directory setting, resolved. Both settings behave identically,
   and two copies of this would be two places for them to stop doing
   so. */
static char *resolve_dir(const char *configured, const char *config_path,
                         const char *leaf) {
    return configured ? strdup(configured)
                      : sp_path_beside(config_path, leaf);
}

void sp_core_set_jvm(void *vm) {
#if defined(PJ_ANDROID) && PJ_ANDROID != 0
    pj_jni_set_jvm(vm);
#else
    (void)vm;
#endif
}

/* Whether name is "<id>-far.pcm" or "<id>-mic.pcm" for a call still
   waiting for its transcript. */
static bool is_pending_track(const char *name, const sp_pending_call *p,
                             size_t n) {
    char *end = NULL;
    errno = 0;
    const long long id = strtoll(name, &end, 10);
    if (errno || end == name || id <= 0
        || (strcmp(end, "-far.pcm") != 0 && strcmp(end, "-mic.pcm") != 0))
        return false;
    for (size_t i = 0; i < n; i++)
        if (p[i].id == id)
            return true;
    return false;
}

/* The directory a call's audio waits in, made safe to empty: a real
   directory of this user's, never a symlink to one, 0700. Opened once
   and worked through the descriptor, so nothing can be swapped in
   between the check and the sweep. Regular files only, and not
   recursively: nothing else is ever put there. The tracks of calls
   still pending are kept, for the queue to resume. false when the
   directory cannot be trusted, and then nothing is deleted. */
static bool prepare_scratch(const char *dir, const sp_pending_call *keep,
                            size_t n_keep) {
    if (sp_path_mkdir_p(dir) != 0)
        return false;
    const int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)
        || st.st_uid != geteuid() || fchmod(fd, 0700) != 0) {
        close(fd);
        return false;
    }
    DIR *d = fdopendir(fd);
    if (!d) {
        close(fd);
        return false;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (is_pending_track(e->d_name, keep, n_keep))
            continue;
        if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0
            && S_ISREG(st.st_mode))
            unlinkat(fd, e->d_name, 0);
    }
    closedir(d);
    return true;
}

/* The scratch directory belongs to one running core at a time. A
   second instance on the same configuration shares the directory and
   the database, so emptying one and failing the pending rows in the
   other would destroy the first instance's queue. The descriptor, or
   -1 when another instance holds it or it cannot be taken. */
static int lock_scratch(const char *dir) {
    if (sp_path_mkdir_p(dir) != 0)
        return -1;
    const int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0)
        return -1;
    const int fd = openat(dfd, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW
                                         | O_CLOEXEC, 0600);
    close(dfd);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool is_regular(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Calls that were still waiting when the last run ended -- quit,
   crash or power cut -- go back in the queue if both their tracks
   survived. One that lost its audio says so instead of waiting for a
   transcript nothing will produce. */
static void resume_pending(sp_core *core, const sp_pending_call *p,
                           size_t n) {
    for (size_t i = 0; i < n; i++) {
        char far[1024], mic[1024];
        const int a = core->transcript_scratch
            ? snprintf(far, sizeof far, "%s/%lld-far.pcm",
                       core->transcript_scratch, (long long)p[i].id) : -1;
        const int b = core->transcript_scratch
            ? snprintf(mic, sizeof mic, "%s/%lld-mic.pcm",
                       core->transcript_scratch, (long long)p[i].id) : -1;
        const bool paths = a > 0 && (size_t)a < sizeof far
                           && b > 0 && (size_t)b < sizeof mic;
        if (core->transcriber && paths && is_regular(far) && is_regular(mic)) {
            sp_party who;
            sp_sipuri_parse(p[i].remote_uri ? p[i].remote_uri : "", &who);
            sp_transcriber_submit(core->transcriber, p[i].id,
                                  sp_sipuri_label(&who), far, mic);
            continue;
        }
        if (paths) {
            unlink(far);
            unlink(mic);
        }
        sp_history_transcript_failed(core->history, p[i].id,
                                     "the call's audio was lost before it "
                                     "was transcribed");
    }
}

sp_status sp_core_create(const char *config_path, sp_core **out) {
    if (!out || !config_path)
        return SP_ERR_INVALID_ARG;
    *out = NULL;

    sp_core *core = calloc(1, sizeof *core);
    if (!core)
        return SP_ERR_INTERNAL;
    if (pthread_mutex_init(&core->lock, NULL) != 0) {
        free(core);
        return SP_ERR_INTERNAL;
    }
    core->scratch_lock = -1;

    core->config_path = strdup(config_path);
    if (!core->config_path) {
        pthread_mutex_destroy(&core->lock);
        free(core);
        return SP_ERR_INTERNAL;
    }

    char err[512] = {0};
    sp_status st = sp_config_load(config_path, &core->cfg, err, sizeof err);
    if (st != SP_OK) {
        snprintf(core->err_detail, sizeof core->err_detail, "%s", err);
        /* Written directly: no other thread exists yet, and the
           core is about to be freed. */
        /* The caller has no core to read the detail from, so also emit
           it: a config error that vanishes is worse than a noisy one. */
        fprintf(stderr, "comlink: config: %s\n", err);
        free(core->config_path);
        pthread_mutex_destroy(&core->lock);
        free(core);
        return st;
    }

    /* The database lives beside the config unless told otherwise, so
       the frontend keeps owning path conventions. */
    char dbpath[1024];
    if (core->cfg->db_file && *core->cfg->db_file) {
        snprintf(dbpath, sizeof dbpath, "%s", core->cfg->db_file);
    } else {
        snprintf(dbpath, sizeof dbpath, "%s", config_path);
        char *slash = strrchr(dbpath, '/');
        if (slash)
            snprintf(slash + 1, sizeof dbpath - (size_t)(slash + 1 - dbpath),
                     "comlink.db");
        else
            snprintf(dbpath, sizeof dbpath, "comlink.db");
    }

    core->recording_dir = resolve_dir(core->cfg->recording_dir,
                                      core->config_path, "recordings");
    core->trace_export_dir = resolve_dir(core->cfg->sip_trace_export_dir,
                                         core->config_path, "traces");
    /* Made at startup so the defaults exist without anyone having
       chosen them. Failure is not fatal to startup -- a directory
       that cannot be made is worth refusing a recording over, not
       worth refusing to start the phone over -- but it must not be
       silent: reported the same way sp_store_open's failure is,
       below, so the operator's first sign of it is not SP_ERR_IO in
       the middle of a call. */
    if (core->recording_dir && sp_path_mkdir_p(core->recording_dir) != 0)
        fprintf(stderr, "comlink: recording directory not created (%s): %s\n",
                core->recording_dir, strerror(errno));
    if (core->trace_export_dir && sp_path_mkdir_p(core->trace_export_dir) != 0)
        fprintf(stderr,
                "comlink: trace export directory not created (%s): %s\n",
                core->trace_export_dir, strerror(errno));
    if (!core->recording_dir || !core->trace_export_dir) {
        sp_core_destroy(core);
        return SP_ERR_INTERNAL;
    }

    char dberr[512] = {0};
    core->store = sp_store_open(dbpath, dberr, sizeof dberr);
    if (!core->store) {
        /* Logging is not worth failing startup over: the phone must
           still work. Reported loudly instead. */
        fprintf(stderr, "comlink: no log database (%s): %s\n",
                dbpath, dberr);
    } else {
        if (core->cfg->sip_log) {
            core->siplog = sp_siplog_create(core->store,
                                            core->cfg->sip_log_redact,
                                            core->cfg->sip_log_max);
            if (!core->siplog)
                fprintf(stderr, "comlink: SIP log unavailable\n");
        }
        /* Twice the SIP cap, so one setting bounds both tables and
           there is no second knob to forget about. */
        core->actionlog = sp_actionlog_create(core->store,
                                              core->cfg->sip_log_max * 2);
        if (!core->actionlog)
            fprintf(stderr, "comlink: action log unavailable\n");
        /* Calls are far rarer than messages, so history keeps more
           per row-count: a thousand calls is a long history. */
        core->history = sp_history_create(core->store, 1000);
        if (!core->history)
            fprintf(stderr, "comlink: call history unavailable\n");
        /* Uncapped: a contact book is what an operator put there, not
           a log that accumulates. */
        core->contacts = sp_contacts_create(core->store);
        if (!core->contacts)
            fprintf(stderr, "comlink: contact book unavailable\n");
    }

    /* Whatever a previous run left waiting is picked up again; only
       the instance holding the directory's lock touches it. */
    core->transcript_scratch = sp_path_beside(config_path,
                                              "transcript-scratch");
    if (core->transcript_scratch) {
        core->scratch_lock = lock_scratch(core->transcript_scratch);
        if (core->scratch_lock < 0) {
            fprintf(stderr, "comlink: another instance owns the transcript "
                            "queue, calls are not transcribed here (%s)\n",
                    core->transcript_scratch);
            free(core->transcript_scratch);
            core->transcript_scratch = NULL;
        }
    }
    sp_pending_call *pending = NULL;
    size_t n_pending = 0;
    if (core->scratch_lock >= 0
        && sp_history_transcript_pending(core->history, &pending,
                                         &n_pending) != 0)
        fprintf(stderr, "comlink: pending transcripts could not be read\n");
    if (core->transcript_scratch
        && !prepare_scratch(core->transcript_scratch, pending, n_pending)) {
        fprintf(stderr, "comlink: transcript directory not usable, "
                        "calls are not transcribed (%s)\n",
                core->transcript_scratch);
        free(core->transcript_scratch);
        core->transcript_scratch = NULL;
    }
    if (core->history && core->transcript_scratch) {
        core->transcriber = sp_transcriber_create(core);
        if (!core->transcriber)
            fprintf(stderr, "comlink: call transcripts unavailable\n");
    }
    sp_history_pending_free(pending, n_pending);

    /* The sound library is filesystem-only, so it does not depend on
       the database having opened. */
    core->sounds = sp_sound_lib_create(config_path);
    for (int i = 0; i < 3; i++)
        core->players[i] = PJSUA_INVALID_ID;
    sp_sound_timer_init(core);
    sp_playback_state_init(&core->playback);

    core->accounts = sp_ht_create();
    core->calls = sp_ht_create();
    core->evq = sp_evq_create();
    if (!core->accounts || !core->calls || !core->evq) {
        sp_core_destroy(core);
        return SP_ERR_INTERNAL;
    }

    if (core->cfg->n_rules > 0) {
        core->stats = calloc(core->cfg->n_rules, sizeof *core->stats);
        if (!core->stats) {
            sp_core_destroy(core);
            return SP_ERR_INTERNAL;
        }
        core->n_stats = core->cfg->n_rules;
    }

    for (int i = 0; i < 3; i++)
        core->tp[i] = PJSUA_INVALID_ID;

    core->state = SP_STATE_NEW;
    *out = core;
    return SP_OK;
}

sp_status sp_core_set_event_cb(sp_core *core, sp_event_cb cb, void *user) {
    SP_ENTRY(core);
    pthread_mutex_lock(&core->lock);
    core->cb = cb;
    core->cb_user = user;
    pthread_mutex_unlock(&core->lock);
    return SP_OK;
}

static pjsip_transport_type_e pj_transport_of(sp_transport t) {
    switch (t) {
    case SP_TP_TCP: return PJSIP_TRANSPORT_TCP;
    case SP_TP_TLS: return PJSIP_TRANSPORT_TLS;
    case SP_TP_UDP: break;
    }
    return PJSIP_TRANSPORT_UDP;
}

static const char *transport_name(sp_transport t) {
    switch (t) {
    case SP_TP_TCP: return "TCP";
    case SP_TP_TLS: return "TLS";
    case SP_TP_UDP: break;
    }
    return "UDP";
}

sp_status sp_core_ensure_transport(sp_core *core, sp_transport want,
                                   pjsua_transport_id *out) {
    if (!core || !out)
        return SP_ERR_INVALID_ARG;
    if (core->tp[want] != PJSUA_INVALID_ID) {
        *out = core->tp[want];
        return SP_OK;
    }

    pjsua_transport_config tcfg;
    pjsua_transport_config_default(&tcfg);
    tcfg.port = (unsigned)core->cfg->sip_port;

    if (want == SP_TP_TLS) {
        if (core->cfg->tls_cert_file)
            tcfg.tls_setting.cert_file = pj_str(core->cfg->tls_cert_file);
        if (core->cfg->tls_key_file)
            tcfg.tls_setting.privkey_file = pj_str(core->cfg->tls_key_file);
        if (core->cfg->tls_ca_file)
            tcfg.tls_setting.ca_list_file = pj_str(core->cfg->tls_ca_file);
        /* Self-signed certificates are the norm on a homelab PBX, so
           peer verification is not demanded. A deliberate, recorded
           choice: demanding it would break the common case, and
           pretending to verify would be worse than either. */
        tcfg.tls_setting.verify_server = PJ_FALSE;
        tcfg.tls_setting.verify_client = PJ_FALSE;
    }

    pjsua_transport_id id = PJSUA_INVALID_ID;
    pj_status_t st = pjsua_transport_create(pj_transport_of(want), &tcfg, &id);
    if (st != PJ_SUCCESS) {
        char what[128];
        snprintf(what, sizeof what, "%s transport on port %d",
                 transport_name(want), core->cfg->sip_port);
        sp_event ev = {0};
        ev.type = SP_EV_TRANSPORT_ERROR;
        ev.u.transport.detail = what;
        sp_core_post(core, &ev);
        return sp_core_fail(core, st, SP_ERR_TRANSPORT, what);
    }
    core->tp[want] = id;
    *out = id;
    return SP_OK;
}

/* Creates one transport per distinct type the configured accounts use.
   A failure is reported and fails the start: no silent fallback to UDP,
   because an account asking for TLS must never end up in the clear. */
static sp_status create_transports(sp_core *core) {
    bool want[3] = { false, false, false };
    for (size_t i = 0; i < core->cfg->n_accounts; i++)
        want[core->cfg->accounts[i].transport] = true;
    if (core->cfg->n_accounts == 0)
        want[SP_TP_UDP] = true;

    for (int t = 0; t < 3; t++) {
        if (!want[t])
            continue;
        pjsua_transport_id id = PJSUA_INVALID_ID;
        sp_status s = sp_core_ensure_transport(core, (sp_transport)t, &id);
        if (s != SP_OK)
            return s;
    }
    return SP_OK;
}

static sp_status apply_codec_priority(sp_core *core) {
    /* Order in the list is the offer order; the config layer has
       already turned it into descending priorities, with zero for a
       codec switched off. */
    for (size_t i = 0; i < core->cfg->n_codecs; i++) {
        const sp_codec_cfg *c = &core->cfg->codecs[i];
        pj_str_t id = pj_str(c->id);
        pj_status_t st = pjsua_codec_set_priority(&id,
                                                  (pj_uint8_t)c->priority);
        if (st != PJ_SUCCESS) {
            char what[160];
            snprintf(what, sizeof what, "codecs entry \"%s\"", c->id);
            return sp_core_fail(core, st, SP_ERR_CONFIG, what);
        }
        if ((st = sp_media_apply_codec_params(core, c)) != SP_OK)
            return st;
    }
    return SP_OK;
}

/* pjsip's own log, into the event stream.
 *
 * It went to stdout, which on Android goes nowhere at all: an APK
 * refusing a call with PJSIP_ETPNOTAVAIL had no trace of why on the
 * device. A softphone that is also a diagnostic instrument cannot
 * have its most useful log reachable only from a terminal.
 *
 * Installing a log callback REPLACES pjsua's console writer rather
 * than adding to it, so this writes the line out itself first.
 * Without that, raising log_level to 5 to chase a registration
 * problem produced less output than before and nothing said why --
 * the one change meant to surface pjsip's log would have hidden most
 * of it from the desktop and the CLI.
 *
 * Levels 0 to 3 reach the event stream. Level 4 and up is pjsip
 * printing whole SIP messages, which is what the SIP trace already
 * captures -- with redaction, which this path has none of.
 *
 * Not through sp_core_post: that writes the action log to SQLite, and
 * a database write per log line, from inside pjsip's own lock, is a
 * stall waiting to become a deadlock. Straight to the queue. */
static void pj_log_to_events(int level, const char *data, int len) {
    /* Everything still goes where it went before -- except on Android
       above level 3. There stdout ends in logcat, which bug reports
       carry off the handset, and level 4 and up is whole SIP messages
       with their Authorization headers. The redacted SIP trace is the
       place for those. */
#if defined(PJ_ANDROID) && PJ_ANDROID != 0
    if (level <= 3)
        pj_log_write(level, data, len);
#else
    pj_log_write(level, data, len);
#endif

    sp_core *core = sp_core_active();
    if (!core || !core->evq || level > 3 || len <= 0)
        return;

    char msg[512];
    int n = len < (int)sizeof msg - 1 ? len : (int)sizeof msg - 1;
    memcpy(msg, data, (size_t)n);
    msg[n] = '\0';
    while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r'))
        msg[--n] = '\0';
    if (n == 0)
        return;

    sp_event ev = {0};
    ev.type = SP_EV_LOG;
    ev.u.log.level = level;
    ev.u.log.msg = msg;
    sp_evq_post(core->evq, &ev);
}

sp_status sp_core_start(sp_core *core) {
    SP_ENTRY(core);

    pthread_mutex_lock(&core->lock);
    if (core->state != SP_STATE_NEW) {
        pthread_mutex_unlock(&core->lock);
        set_detail(core, "core has already been started");
        return SP_ERR_STATE;
    }
    pthread_mutex_unlock(&core->lock);

    pthread_mutex_lock(&g_core_lock);
    if (g_core) {
        pthread_mutex_unlock(&g_core_lock);
        set_detail(core,
                   "another core is already running in this process; pjsua "
                   "is a process-wide singleton");
        return SP_ERR_STATE;
    }
    g_core = core;
    pthread_mutex_unlock(&g_core_lock);

    /* pjsua_create() logs before pjsua_init() can apply the logging
       config, so the level has to be set first or a configured level
       of 0 still prints startup chatter. */
    pj_log_set_level(core->cfg->log_level);

    /* An operator who asked for no audio device must not be held up by
       one. pjsua_init initialises every audio driver before any of our
       settings are read, and ALSA's "default" is the pipewire ctl
       plugin on a desktop Debian box: with the pipewire socket
       unreachable, that plugin's failure path joins a thread nothing
       wakes and the phone hangs at startup. A headless box is exactly
       where that happens and exactly where "none" is used.
       See docs/known-issues.md.

       Pointing ALSA at a configuration that defines nothing means the
       ALSA driver finds no devices and loads no plugins. The null
       audio device, compiled in via config_site.h, is what keeps the
       audio subsystem valid with no real driver: without it
       pjsua_init fails with PJMEDIA_EAUD_SYSERR.

       Only when the operator asked for it, and never overriding a
       value they set themselves. */
    if (sp_config_wants_no_audio(core->cfg) && !getenv("ALSA_CONFIG_PATH")) {
        const char *conf = getenv("COMLINK_ALSA_CONF");
        if (!conf || !*conf)
            conf = SP_ALSA_NONE_CONF;
        setenv("ALSA_CONFIG_PATH", conf, 1);
    }

    pj_status_t st = pjsua_create();
    if (st != PJ_SUCCESS) {
        pthread_mutex_lock(&g_core_lock); g_core = NULL;
        pthread_mutex_unlock(&g_core_lock);
        return sp_core_fail(core, st, SP_ERR_INTERNAL, "pjsua_create");
    }
    core->pj_ready = true;

    pjsua_config ua;
    pjsua_logging_config log;
    pjsua_media_config media;
    pjsua_config_default(&ua);
    pjsua_logging_config_default(&log);
    pjsua_media_config_default(&media);

    /* pjsua suppresses silence by default. A diagnostic phone should
       keep the stream running, so the operator's jitter and loss
       figures are measured over a continuous flow rather than over
       whatever leaked through during speech. */
    media.no_vad = core->cfg->silence_suppression ? PJ_FALSE : PJ_TRUE;

    /* Also set here, not only through sp_media_apply_dsp below: pjsua
       builds the echo canceller during init, and one built with the
       wrong settings would be torn down and rebuilt a moment later. */
    media.ec_tail_len = core->cfg->echo_cancel
                            ? (unsigned)core->cfg->echo_tail_ms : 0u;
    media.ec_options = sp_media_ec_options(core->cfg);

    log.level = (unsigned)core->cfg->log_level;
    log.console_level = (unsigned)core->cfg->log_level;
    /* Also into the event stream, so the phone can show its own
       reason for refusing a call rather than keeping it in a terminal
       that does not exist on a handset. */
    log.cb = pj_log_to_events;

    sp_account_install_callbacks(&ua.cb);
    sp_call_install_callbacks(&ua.cb);
    sp_core_install_ip_change_callback(&ua.cb);

    st = pjsua_init(&ua, &log, &media);
    if (st != PJ_SUCCESS) {
        sp_status s = sp_core_fail(core, st, SP_ERR_INTERNAL, "pjsua_init");
        pjsua_destroy();
        core->pj_ready = false;
        pthread_mutex_lock(&g_core_lock); g_core = NULL;
        pthread_mutex_unlock(&g_core_lock);
        return s;
    }

    sp_status s = create_transports(core);
    if (s != SP_OK)
        goto unwind;

    st = pjsua_start();
    if (st != PJ_SUCCESS) {
        s = sp_core_fail(core, st, SP_ERR_INTERNAL, "pjsua_start");
        goto unwind;
    }

    if ((s = apply_codec_priority(core)) != SP_OK)
        goto unwind;

    if ((s = sp_hdr_module_register(core)) != SP_OK)
        goto unwind;

    if ((s = sp_media_apply_config(core)) != SP_OK)
        goto unwind;

    /* Volumes, before anything can be heard. Not fatal for the same
       reason as the processing settings below. */
    if (sp_media_apply_levels(core) != SP_OK)
        sp_actionlog_add(core, 2, "volume settings could not be applied");

    /* Not fatal. A phone that refuses to start because it could not
       configure an echo canceller is worse than one that starts with
       the stack's defaults and says so. */
    if (sp_media_apply_dsp(core) != SP_OK)
        sp_actionlog_add(core, 2,
                         "audio processing settings could not be applied; "
                         "the media stack's defaults are in use");

    if (sp_evq_start(core->evq, core->cb, core->cb_user) != 0) {
        s = SP_ERR_INTERNAL;
        set_detail(core, "could not start the event dispatch thread");
        goto unwind;
    }

    /* The config file is the single source of truth for accounts. */
    for (size_t i = 0; i < core->cfg->n_accounts; i++) {
        sp_account_id id = 0;
        /* Already in the config, so nothing to persist. */
        s = sp_account_add_locked(core, &core->cfg->accounts[i], &id, false);
        if (s != SP_OK)
            goto unwind;
    }

    /* A trace switched on for one debugging session is easy to forget
       for months, so this warns on every start, not just the first. */
    if (core->cfg->sip_log && !core->cfg->sip_log_redact) {
        sp_event ev = {0};
        ev.type = SP_EV_LOG;
        ev.u.log.level = 2;
        ev.u.log.msg = "SIP log redaction is OFF: credentials are being "
                       "written to the database in cleartext";
        sp_core_post(core, &ev);
    }

    /* A phone that will not place calls because it cannot measure them
       would be worse than a phone with no measurements, so a sampler
       that fails to schedule is recorded and stepped over. */
    if (sp_metrics_start(core) != SP_OK)
        sp_actionlog_add(core, 2,
                         "audio quality sampling could not be started");

    pthread_mutex_lock(&core->lock);
    core->state = SP_STATE_RUNNING;
    pthread_mutex_unlock(&core->lock);

    /* Here rather than at create: the worker reports to the event
       queue and checks the calls table, and both exist from now on.
       Once per core; a restart within one process has nothing new. */
    if (core->scratch_lock >= 0 && !core->pending_resumed) {
        core->pending_resumed = true;
        sp_pending_call *pending = NULL;
        size_t n_pending = 0;
        if (sp_history_transcript_pending(core->history, &pending,
                                          &n_pending) == 0)
            resume_pending(core, pending, n_pending);
        sp_history_pending_free(pending, n_pending);
    }
    return SP_OK;

unwind:
    sp_metrics_stop(core);
    sp_hdr_module_unregister();
    sp_evq_stop(core->evq);
    pjsua_destroy();
    core->pj_ready = false;
    pthread_mutex_lock(&g_core_lock); g_core = NULL;
    pthread_mutex_unlock(&g_core_lock);
    return s;
}

sp_status sp_core_stop(sp_core *core) {
    SP_ENTRY(core);
    pthread_mutex_lock(&core->lock);
    if (core->state != SP_STATE_RUNNING) {
        pthread_mutex_unlock(&core->lock);
        set_detail(core, "core is not running");
        return SP_ERR_STATE;
    }
    core->state = SP_STATE_STOPPED;
    pthread_mutex_unlock(&core->lock);

    /* Before pjsua_destroy: a queued fire would otherwise run against
       a core being torn down. */
    sp_metrics_stop(core);
    sp_sound_stop_all(core);
    /* Before pjsua_destroy, on the same rule as sp_sound_stop_all: a
       player alive across a core stop is a use-after-free waiting for
       the next sp_playback_start. */
    sp_playback_stop(core);
    sp_hdr_module_unregister();
    sp_evq_stop(core->evq);
    pjsua_destroy();
    core->pj_ready = false;

    pthread_mutex_lock(&g_core_lock);
    if (g_core == core)
        g_core = NULL;
    pthread_mutex_unlock(&g_core_lock);
    return SP_OK;
}

void sp_core_destroy(sp_core *core) {
    if (!core)
        return;
    if (core->state == SP_STATE_RUNNING)
        sp_core_stop(core);
    /* Redundant whenever the branch above ran -- sp_core_stop already
       tore the player down -- and a no-op when nothing was ever
       loaded. Kept explicit so a future change to the RUNNING guard
       above cannot silently reopen the use-after-free this guards
       against. */
    sp_playback_stop(core);

    /* Free the per-handle records before dropping the tables. Nothing
       else is running by now, but snapshotting keeps one enumeration
       idiom across the code base. */
    uint64_t acc_ids[PJSUA_MAX_ACC];
    size_t n_acc = 0;
    sp_ht_snapshot(core->accounts, acc_ids, PJSUA_MAX_ACC, &n_acc);
    if (n_acc > PJSUA_MAX_ACC)
        n_acc = PJSUA_MAX_ACC;
    for (size_t i = 0; i < n_acc; i++) {
        void *data = NULL;
        if (sp_ht_lookup(core->accounts, acc_ids[i], NULL, &data) != 0)
            continue;
        sp_account_rec *rec = data;
        if (rec) {
            free(rec->id);
            free(rec->aor);
            free(rec->domain);
            free(rec);
        }
    }
    sp_ht_destroy(core->accounts);

    uint64_t call_ids[PJSUA_MAX_CALLS];
    size_t n_call = 0;
    sp_ht_snapshot(core->calls, call_ids, PJSUA_MAX_CALLS, &n_call);
    if (n_call > PJSUA_MAX_CALLS)
        n_call = PJSUA_MAX_CALLS;
    for (size_t i = 0; i < n_call; i++) {
        void *data = NULL;
        if (sp_ht_lookup(core->calls, call_ids[i], NULL, &data) == 0)
            free(data);
    }
    sp_ht_destroy(core->calls);
    /* After the calls, whose capture ports may still hand it a job as
       pjsua tears the bridge down; before the queue and the history,
       which it reports to. */
    sp_transcriber_destroy(core->transcriber);
    core->transcriber = NULL;
    free(core->transcript_scratch);
    if (core->scratch_lock >= 0)
        close(core->scratch_lock);
    sp_evq_destroy(core->evq);
    sp_contacts_destroy(core->contacts);
    sp_history_destroy(core->history);
    sp_actionlog_destroy(core->actionlog);
    sp_siplog_destroy(core->siplog);
    sp_store_close(core->store);
    sp_config_free(core->cfg);
    free(core->config_path);
    free(core->recording_dir);
    free(core->trace_export_dir);
    free(core->stats);
    sp_sound_lib_destroy(core->sounds);
    core->sounds = NULL;
    sp_media_free_devices(core);
    free((void *)core->codec_names);
    free(core->codec_params);
    free(core->module_view);
    pthread_mutex_destroy(&core->lock);
    free(core);
}

sp_status sp_core_persist(sp_core *core) {
    char err[512] = {0};
    sp_status st = sp_config_save(core->cfg, core->config_path, err, sizeof err);
    if (st != SP_OK)
        sp_core_detail(core, "%s", err);
    return st;
}

sp_status sp_theme_get(sp_core *core, sp_theme *out) {
    SP_ENTRY(core);
    if (!out)
        return SP_ERR_INVALID_ARG;
    return sp_theme_resolve(core->cfg->theme_preset, core->cfg->theme_accent,
                            core->cfg->theme_font, core->cfg->theme_scale,
                            out);
}

sp_status sp_theme_set(sp_core *core, sp_theme_preset preset,
                       const char *accent_hex, const char *font_family,
                       sp_text_scale scale) {
    SP_ENTRY(core);

    /* Validated before anything is stored, so a rejected theme leaves
       the config exactly as it was. */
    sp_theme probe;
    sp_status st = sp_theme_resolve(preset, accent_hex, font_family, scale,
                                    &probe);
    if (st != SP_OK) {
        sp_core_detail(core, "theme rejected: accent must be #RRGGBB");
        return st;
    }

    char *accent = NULL, *font = NULL;
    if (accent_hex && *accent_hex) {
        accent = strdup(accent_hex);
        if (!accent)
            return SP_ERR_INTERNAL;
    }
    if (font_family && *font_family) {
        font = strdup(font_family);
        if (!font) {
            free(accent);
            return SP_ERR_INTERNAL;
        }
    }

    free(core->cfg->theme_accent);
    free(core->cfg->theme_font);
    core->cfg->theme_accent = accent;
    core->cfg->theme_font = font;
    core->cfg->theme_preset = preset;
    free(core->cfg->theme_preset_unknown);
    core->cfg->theme_preset_unknown = NULL;
    core->cfg->theme_scale = scale;
    return sp_core_persist(core);
}

sp_status sp_modules_get(sp_core *core, const sp_module **out,
                         size_t *n_out) {
    SP_ENTRY(core);
    if (!out || !n_out)
        return SP_ERR_INVALID_ARG;

    const int n = sp_module_count();
    free(core->module_view);
    core->module_view = calloc((size_t)n, sizeof *core->module_view);
    if (!core->module_view)
        return SP_ERR_INTERNAL;

    for (int i = 0; i < n; i++) {
        core->module_view[i].id = sp_module_id_at(i);
        core->module_view[i].label = sp_module_label_at(i);
        core->module_view[i].removable = sp_module_removable_at(i);
        core->module_view[i].enabled =
            sp_config_module_enabled(core->cfg, sp_module_id_at(i));
    }
    *out = core->module_view;
    *n_out = (size_t)n;
    return SP_OK;
}

sp_status sp_module_set(sp_core *core, const char *id, bool enabled) {
    SP_ENTRY(core);
    const int i = sp_module_index_of(id);
    if (i < 0) {
        sp_core_detail(core, "no module called \"%s\"", id ? id : "");
        return SP_ERR_NOT_FOUND;
    }
    if (!enabled && !sp_module_removable_at(i)) {
        sp_core_detail(core, "module \"%s\" cannot be switched off", id);
        return SP_ERR_INVALID_ARG;
    }

    core->cfg->modules[i] = enabled;
    /* sip_log is the same switch under its old name, and the two are
       kept in step so nothing reads a stale one. */
    if (strcmp(id, SP_MODULE_SIP_TRACE) == 0)
        core->cfg->sip_log = enabled;

    /* The one exception to the rule below: a transcript is somebody's
       words being collected, and switching it off has to stop the
       collecting now, not at the end of the call. What was already
       transcribed stays. */
    if (strcmp(id, SP_MODULE_TRANSCRIPT) == 0 && !enabled) {
        if (core->pj_ready)
            sp_call_stop_captures(core);
        sp_transcriber_module_off(core->transcriber);
    }

    /* Nothing is torn down here on purpose. A module switched off
       stops being started; what it already produced -- recordings,
       history rows, figures -- is the operator's, not ours to
       remove. Sampling stops at the next start, the same rule the
       codec settings follow. */
    return sp_core_persist(core);
}

bool sp_module_is_enabled(sp_core *core, const char *id) {
    if (!core || !core->cfg)
        return false;
    return sp_config_module_enabled(core->cfg, id);
}

sp_status sp_core_get_settings(sp_core *core, sp_settings *out) {
    SP_ENTRY(core);
    if (!out)
        return SP_ERR_INVALID_ARG;
    out->sip_log = core->cfg->sip_log;
    out->sip_log_redact = core->cfg->sip_log_redact;
    out->sip_log_max = core->cfg->sip_log_max;
    out->log_level = core->cfg->log_level;
    out->sip_port = core->cfg->sip_port;
    return SP_OK;
}

/* A monotonic millisecond clock for the IP-change deadline. Monotonic
   rather than wall-clock: a handset crossing midnight or syncing its
   time must not make a change in flight look either finished or
   eternal. */
static uint64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* How far pjsua has got with an IP change.
 *
 * The listeners are restarted on a timer, and a transport that is
 * restarting is refused with PJSIP_ETPNOTAVAIL. Until pjsua says it
 * has finished, a second change would leave every transport in that
 * state and every call refused -- which is exactly what a handset
 * crossing a boundary produces, because the system reports the change
 * more than once. */
static void on_ip_change_progress(pjsua_ip_change_op op, pj_status_t status,
                                  const pjsua_ip_change_op_info *info) {
    (void)status;
    (void)info;
    sp_core *core = sp_core_active();
    if (!core)
        return;
    if (op == PJSUA_IP_CHANGE_OP_COMPLETED)
        atomic_store(&core->ip_change_started_ms, 0);
}

void sp_core_install_ip_change_callback(pjsua_callback *cb) {
    cb->on_ip_change_progress = on_ip_change_progress;
}

/* Wifi to LTE, or a cable pulled.
 *
 * pjsua_handle_ip_change is the operation this is: it restarts the
 * transport listeners, shuts down the TCP and TLS ones that are now
 * pointing at an address that has gone, re-registers every account
 * and re-invites the calls that are up. Doing any of that by hand
 * here would be a second implementation of something pjsip already
 * sequences correctly.
 *
 * The listener is deliberately NOT restarted.
 *
 * Our transports bind to INADDR_ANY -- only a port is configured, never
 * an address -- so there is nothing about them that a new address
 * invalidates. What does need to change is each account's Contact and
 * its registration, which is what pjsua does either way. Restarting
 * the listener destroys and re-creates the socket, and on Android the
 * re-creation left the transport unusable: every call after it was
 * refused with PJSIP_ETPNOTAVAIL, from a phone that had never left
 * the network it started on. Measured on the emulator, not assumed.
 *
 * A second call while one is still running is SP_ERR_STATE rather
 * than a second restart. pjsua only logs "still in progress" and
 * carries on, and what it carries on into is every listener marked
 * restarting and every call refused with PJSIP_ETPNOTAVAIL. Measured
 * on the emulator: two changes arrived a moment apart at startup and
 * the next call could not be placed at all. */
sp_status sp_core_network_changed(sp_core *core) {
    SP_ENTRY(core);
    if (core->state != SP_STATE_RUNNING)
        return SP_ERR_STATE;
    /* Claimed atomically, because the flag is cleared from a pjsip
       thread: a plain read-then-write here is the race the guard
       exists to prevent.

       And with a deadline, because pjsua only reports COMPLETED when
       every account has finished, and an account added while a change
       is in flight is never part of that count -- so the flag could
       otherwise latch for the life of the process and the phone would
       stop handing over with nothing on screen to say so. */
    const int64_t now = (int64_t)(mono_us() / 1000);
    const int64_t started = atomic_load(&core->ip_change_started_ms);
    if (started != 0 && now - started < SP_IP_CHANGE_TIMEOUT_MS) {
        set_detail(core, "a network change is already being handled");
        return SP_ERR_STATE;
    }
    int64_t expect = started;
    if (!atomic_compare_exchange_strong(&core->ip_change_started_ms,
                                        &expect, now)) {
        set_detail(core, "a network change is already being handled");
        return SP_ERR_STATE;
    }

    pjsua_ip_change_param p;
    pjsua_ip_change_param_default(&p);
    p.restart_listener = PJ_FALSE;

    pj_status_t st = pjsua_handle_ip_change(&p);
    if (st != PJ_SUCCESS) {
        atomic_store(&core->ip_change_started_ms, 0);
        return sp_core_fail(core, st, SP_ERR_TRANSPORT,
                            "pjsua_handle_ip_change");
    }
    return SP_OK;
}

sp_status sp_presets_list(sp_core *core, const sp_header_preset **out,
                          size_t *n_out) {
    SP_ENTRY(core);
    if (!out || !n_out)
        return SP_ERR_INVALID_ARG;
    *out = core->cfg->presets;
    *n_out = core->cfg->n_presets;
    return SP_OK;
}

sp_status sp_header_names_list(sp_core *core, const char ***out,
                               size_t *n_out) {
    SP_ENTRY(core);
    if (!out || !n_out)
        return SP_ERR_INVALID_ARG;
    *out = (const char **)core->cfg->header_names;
    *n_out = core->cfg->n_header_names;
    return SP_OK;
}

/* Remembering without persisting, so a caller doing several can save
   once at the end rather than rewriting the config per name. */
static sp_status remember_name(sp_core *core, const char *name) {
    if (!name || !*name)
        return SP_ERR_INVALID_ARG;
    for (size_t i = 0; i < core->cfg->n_header_names; i++)
        if (strcasecmp(core->cfg->header_names[i], name) == 0)
            return SP_OK;   /* already known */

    char **grown = realloc(core->cfg->header_names,
                           (core->cfg->n_header_names + 1) * sizeof *grown);
    if (!grown)
        return SP_ERR_INTERNAL;
    core->cfg->header_names = grown;
    grown[core->cfg->n_header_names] = strdup(name);
    if (!grown[core->cfg->n_header_names])
        return SP_ERR_INTERNAL;
    core->cfg->n_header_names++;
    return SP_OK;
}

sp_status sp_header_names_remember(sp_core *core, const char *name) {
    SP_ENTRY(core);
    sp_status st = remember_name(core, name);
    if (st != SP_OK)
        return st;
    return sp_core_persist(core);
}

sp_status sp_presets_save(sp_core *core, const char *name,
                          const sp_header *headers, size_t n_headers) {
    SP_ENTRY(core);
    if (!name || !*name || !headers || n_headers == 0)
        return SP_ERR_INVALID_ARG;
    for (size_t i = 0; i < n_headers; i++) {
        if (!headers[i].name || !*headers[i].name || !headers[i].value) {
            sp_core_detail(core,
                           "preset \"%s\": header %zu needs a name and a "
                           "value", name, i);
            return SP_ERR_INVALID_ARG;
        }
    }

    /* Built fully before anything is replaced, so a failure leaves the
       existing preset intact. */
    sp_header *hv = calloc(n_headers, sizeof *hv);
    if (!hv)
        return SP_ERR_INTERNAL;
    for (size_t i = 0; i < n_headers; i++) {
        hv[i].name = strdup(headers[i].name);
        hv[i].value = strdup(headers[i].value);
        if (!hv[i].name || !hv[i].value) {
            for (size_t j = 0; j <= i; j++) {
                free((void *)hv[j].name);
                free((void *)hv[j].value);
            }
            free(hv);
            return SP_ERR_INTERNAL;
        }
    }

    sp_header_preset *slot = NULL;
    for (size_t i = 0; i < core->cfg->n_presets; i++) {
        if (strcmp(core->cfg->presets[i].name, name) == 0) {
            slot = &core->cfg->presets[i];
            break;
        }
    }

    if (slot) {
        /* Replace rather than append, so saving over a name does not
           leave a duplicate to hunt down. */
        for (size_t j = 0; j < slot->n_headers; j++) {
            free((void *)slot->headers[j].name);
            free((void *)slot->headers[j].value);
        }
        free((void *)slot->headers);
        slot->headers = hv;
        slot->n_headers = n_headers;
    } else {
        sp_header_preset *grown =
            realloc(core->cfg->presets,
                    (core->cfg->n_presets + 1) * sizeof *grown);
        if (!grown) {
            for (size_t j = 0; j < n_headers; j++) {
                free((void *)hv[j].name);
                free((void *)hv[j].value);
            }
            free(hv);
            return SP_ERR_INTERNAL;
        }
        core->cfg->presets = grown;
        slot = &grown[core->cfg->n_presets];
        slot->name = strdup(name);
        slot->headers = hv;
        slot->n_headers = n_headers;
        if (!slot->name)
            return SP_ERR_INTERNAL;
        core->cfg->n_presets++;
    }

    /* Teach the names, so the editor offers them next time. */
    for (size_t i = 0; i < n_headers; i++)
        remember_name(core, headers[i].name);

    return sp_core_persist(core);
}

sp_status sp_presets_delete(sp_core *core, const char *name) {
    SP_ENTRY(core);
    if (!name || !*name)
        return SP_ERR_INVALID_ARG;
    for (size_t i = 0; i < core->cfg->n_presets; i++) {
        if (strcmp(core->cfg->presets[i].name, name) != 0)
            continue;
        sp_header_preset *p = &core->cfg->presets[i];
        free((void *)p->name);
        for (size_t j = 0; j < p->n_headers; j++) {
            free((void *)p->headers[j].name);
            free((void *)p->headers[j].value);
        }
        free((void *)p->headers);
        memmove(p, p + 1,
                (core->cfg->n_presets - i - 1) * sizeof *p);
        core->cfg->n_presets--;
        return sp_core_persist(core);
    }
    sp_core_detail(core, "no header preset called \"%s\"", name);
    return SP_ERR_NOT_FOUND;
}

sp_status sp_siplog_read(sp_core *core, const char *call_id, size_t limit,
                         const sp_sip_message **out, size_t *n_out) {
    SP_ENTRY(core);
    return sp_siplog_query(core->siplog, call_id, limit, out, n_out);
}

sp_status sp_siplog_clear(sp_core *core) {
    SP_ENTRY(core);
    return sp_siplog_wipe(core->siplog);
}

sp_status sp_actionlog_read(sp_core *core, size_t limit,
                            const sp_action_entry **out, size_t *n_out) {
    SP_ENTRY(core);
    return sp_actionlog_query(core->actionlog, limit, out, n_out);
}

sp_status sp_actionlog_add(sp_core *core, int level, const char *text) {
    SP_ENTRY(core);
    if (!text || !*text)
        return SP_ERR_INVALID_ARG;
    sp_actionlog_write(core->actionlog, level, text);
    return SP_OK;
}

sp_status sp_actionlog_clear(sp_core *core) {
    SP_ENTRY(core);
    return sp_actionlog_wipe(core->actionlog);
}

sp_status sp_history_read(sp_core *core, size_t limit,
                          const sp_call_record **out, size_t *n_out) {
    SP_ENTRY(core);
    return sp_history_query(core->history, limit, out, n_out);
}

sp_status sp_history_clear(sp_core *core) {
    SP_ENTRY(core);
    return sp_history_wipe(core->history);
}

sp_status sp_transcript_read(sp_core *core, int64_t history_id,
                             const sp_transcript_line **out, size_t *n_out) {
    SP_ENTRY(core);
    if (!out || !n_out)
        return SP_ERR_INVALID_ARG;
    return sp_history_transcript_read(core->history, history_id, out, n_out);
}

bool sp_transcript_available(sp_core *core) {
    return core && sp_transcriber_has_engine(core->transcriber);
}

sp_status sp_transcript_get_model(sp_core *core, char *out, size_t outlen) {
    SP_ENTRY(core);
    if (!out || outlen == 0)
        return SP_ERR_INVALID_ARG;
    pthread_mutex_lock(&core->lock);
    const char *m = core->cfg->transcript_model;
    const int n = snprintf(out, outlen, "%s", m ? m : "");
    pthread_mutex_unlock(&core->lock);
    if (n < 0 || (size_t)n >= outlen) {
        *out = '\0';
        return SP_ERR_INVALID_ARG;
    }
    return SP_OK;
}

sp_status sp_transcript_set_model(sp_core *core, const char *path) {
    SP_ENTRY(core);
    if (path && *path) {
        if (*path != '/') {
            sp_core_detail(core, "the model must be an absolute path");
            return SP_ERR_INVALID_ARG;
        }
        if (access(path, R_OK) != 0) {
            sp_core_detail(core, "the model file cannot be read");
            return SP_ERR_NOT_FOUND;
        }
    }
    char *stored = (path && *path) ? strdup(path) : NULL;
    if (path && *path && !stored)
        return SP_ERR_INTERNAL;
    pthread_mutex_lock(&core->lock);
    free(core->cfg->transcript_model);
    core->cfg->transcript_model = stored;
    pthread_mutex_unlock(&core->lock);
    return sp_core_persist(core);
}

sp_status sp_transcript_get_scratch_dir(sp_core *core, const char **dir) {
    SP_ENTRY(core);
    if (!dir)
        return SP_ERR_INVALID_ARG;
    *dir = core->transcript_scratch;
    return *dir ? SP_OK : SP_ERR_INTERNAL;
}

sp_status sp_contacts_read(sp_core *core, const char *search, size_t limit,
                           const sp_contact **out, size_t *n_out) {
    SP_ENTRY(core);
    return sp_contacts_query(core->contacts, search, limit, out, n_out);
}

sp_status sp_contact_read_one(sp_core *core, int64_t id,
                              const sp_contact **out) {
    SP_ENTRY(core);
    return sp_contacts_get(core->contacts, id, out);
}

sp_status sp_contact_save(sp_core *core, const sp_contact *in,
                          int64_t *id_out) {
    SP_ENTRY(core);
    return sp_contacts_save(core->contacts, in, id_out);
}

sp_status sp_contact_delete(sp_core *core, int64_t id) {
    SP_ENTRY(core);
    return sp_contacts_delete(core->contacts, id);
}

sp_status sp_contact_lookup(sp_core *core, const char *uri,
                            const sp_contact **out) {
    SP_ENTRY(core);
    return sp_contacts_lookup(core->contacts, uri, out);
}

sp_status sp_contact_picture(sp_core *core, int64_t id, const void **data,
                             size_t *len, const char **type) {
    SP_ENTRY(core);
    return sp_contacts_picture(core->contacts, id, data, len, type);
}

sp_status sp_rules_stats(sp_core *core, const sp_rule_stat **out,
                         size_t *n_out) {
    SP_ENTRY(core);
    if (!out || !n_out)
        return SP_ERR_INVALID_ARG;
    for (size_t i = 0; i < core->n_stats; i++) {
        core->stats[i].name = core->cfg->rules[i].name;
        core->stats[i].hits = atomic_load_explicit(
            &core->cfg->rules[i].hits, memory_order_relaxed);
    }
    *out = core->stats;
    *n_out = core->n_stats;
    return SP_OK;
}

sp_status sp_ui_get_window_size(sp_core *core, int *w, int *h) {
    SP_ENTRY(core);
    if (!w || !h)
        return SP_ERR_INVALID_ARG;
    pthread_mutex_lock(&core->lock);
    *w = core->cfg->window_width;
    *h = core->cfg->window_height;
    pthread_mutex_unlock(&core->lock);
    return SP_OK;
}

sp_status sp_ui_save_window_size(sp_core *core, int w, int h) {
    SP_ENTRY(core);
    /* Zero would read back as "never set", so it is an error rather
       than a way to clear the setting. */
    if (w < 1 || h < 1 || w > 32767 || h > 32767)
        return SP_ERR_INVALID_ARG;
    pthread_mutex_lock(&core->lock);
    const bool same = core->cfg->window_width == w &&
                      core->cfg->window_height == h;
    core->cfg->window_width = w;
    core->cfg->window_height = h;
    pthread_mutex_unlock(&core->lock);
    /* Nothing changed, so nothing is written: closing the window
       should not rewrite the config on every run. */
    if (same)
        return SP_OK;
    return sp_core_persist(core);
}

sp_status sp_log_set_order(sp_core *core, sp_order order) {
    SP_ENTRY(core);
    if (order != SP_ORDER_NEWEST_FIRST && order != SP_ORDER_OLDEST_FIRST)
        return SP_ERR_INVALID_ARG;
    pthread_mutex_lock(&core->lock);
    core->log_order = order;
    /* One setting for all three, so the operator does not have to sort
       the SIP log and the history separately to line them up. */
    sp_siplog_set_order(core->siplog, order);
    sp_actionlog_set_order(core->actionlog, order);
    sp_history_set_order(core->history, order);
    pthread_mutex_unlock(&core->lock);
    return SP_OK;
}

sp_order sp_log_get_order(sp_core *core) {
    if (!core)
        return SP_ORDER_NEWEST_FIRST;
    pthread_mutex_lock(&core->lock);
    sp_order o = core->log_order;
    pthread_mutex_unlock(&core->lock);
    return o;
}

bool sp_ui_get_show_activity_log(sp_core *core) {
    if (!core)
        return true;
    pthread_mutex_lock(&core->lock);
    bool v = core->cfg->show_activity_log;
    pthread_mutex_unlock(&core->lock);
    return v;
}

sp_status sp_ui_set_show_activity_log(sp_core *core, bool show) {
    SP_ENTRY(core);
    pthread_mutex_lock(&core->lock);
    const bool same = core->cfg->show_activity_log == show;
    core->cfg->show_activity_log = show;
    pthread_mutex_unlock(&core->lock);
    if (same)
        return SP_OK;
    return sp_core_persist(core);
}

bool sp_ui_get_show_call_log(sp_core *core) {
    if (!core)
        return false;
    pthread_mutex_lock(&core->lock);
    bool v = core->cfg->show_call_log;
    pthread_mutex_unlock(&core->lock);
    return v;
}

sp_status sp_ui_set_show_call_log(sp_core *core, bool show) {
    SP_ENTRY(core);
    pthread_mutex_lock(&core->lock);
    const bool same = core->cfg->show_call_log == show;
    core->cfg->show_call_log = show;
    pthread_mutex_unlock(&core->lock);
    if (same)
        return SP_OK;
    return sp_core_persist(core);
}

bool sp_recording_get_auto(sp_core *core) {
    if (!core)
        return false;
    pthread_mutex_lock(&core->lock);
    bool v = core->cfg->auto_record;
    pthread_mutex_unlock(&core->lock);
    return v;
}

sp_status sp_recording_set_auto(sp_core *core, bool on) {
    SP_ENTRY(core);
    pthread_mutex_lock(&core->lock);
    const bool same = core->cfg->auto_record == on;
    core->cfg->auto_record = on;
    pthread_mutex_unlock(&core->lock);
    if (same)
        return SP_OK;
    return sp_core_persist(core);
}

/* One implementation behind both directory settings. They differ only
   in which field they read and which leaf they fall back to. */
static sp_status get_dir(sp_core *core, char *const *slot,
                         const char **out) {
    if (!out)
        return SP_ERR_INVALID_ARG;
    pthread_mutex_lock(&core->lock);
    *out = *slot;
    pthread_mutex_unlock(&core->lock);
    return *out ? SP_OK : SP_ERR_INTERNAL;
}

static sp_status set_dir(sp_core *core, char **slot, char **cfg_slot,
                         const char *leaf, const char *dir) {
    if (dir && *dir && *dir != '/') {
        sp_core_detail(core, "the directory must be an absolute path");
        return SP_ERR_INVALID_ARG;
    }
    char *resolved = (dir && *dir) ? strdup(dir)
                                   : sp_path_beside(core->config_path, leaf);
    if (!resolved)
        return SP_ERR_INTERNAL;
    if (sp_path_mkdir_p(resolved) != 0) {
        sp_core_detail(core, "the directory could not be created");
        free(resolved);
        return SP_ERR_IO;
    }
    char *stored = (dir && *dir) ? strdup(dir) : NULL;
    if (dir && *dir && !stored) {
        free(resolved);
        return SP_ERR_INTERNAL;
    }
    pthread_mutex_lock(&core->lock);
    free(*slot);
    *slot = resolved;
    free(*cfg_slot);
    *cfg_slot = stored;
    pthread_mutex_unlock(&core->lock);
    return sp_core_persist(core);
}

sp_status sp_recording_get_dir(sp_core *core, const char **dir) {
    SP_ENTRY(core);
    return get_dir(core, &core->recording_dir, dir);
}

sp_status sp_recording_set_dir(sp_core *core, const char *dir) {
    SP_ENTRY(core);
    return set_dir(core, &core->recording_dir, &core->cfg->recording_dir,
                   "recordings", dir);
}

sp_status sp_trace_get_export_dir(sp_core *core, const char **dir) {
    SP_ENTRY(core);
    return get_dir(core, &core->trace_export_dir, dir);
}

sp_status sp_trace_set_export_dir(sp_core *core, const char *dir) {
    SP_ENTRY(core);
    return set_dir(core, &core->trace_export_dir,
                   &core->cfg->sip_trace_export_dir, "traces", dir);
}

/* <dir>/trace-<safe call-id>-<stamp>.txt, checked before writing so a
   Call-ID or a directory that does not fit is refused rather than
   handed back truncated -- the same discipline sp_recording_path uses
   in recname.c, and the reason that function exists at all.
 *
 * Returns 0 on success, -1 when call_id itself is the problem (too
 * long to fit sp_trace_safe_name's buffer), or -2 for any other
 * failure (an export directory too long to fit the composed path).
 * The caller turns -1 into SP_ERR_INVALID_ARG and -2 into SP_ERR_IO
 * -- a bad Call-ID and a disk/configuration problem are not the same
 * class of error and should not share one status. */
static int compose_trace_path(char *out, size_t outlen, const char *dir,
                              const char *call_id) {
    if (outlen > 0)
        out[0] = '\0';

    char safe[256];
    if (sp_trace_safe_name(safe, sizeof safe, call_id) != 0)
        return -1;

    char stamp[32] = "00000000-000000";
    const time_t now = time(NULL);
    struct tm tmv;
    if (localtime_r(&now, &tmv)) {
        /* Same fallback discipline as sp_recording_name: POSIX leaves
           stamp indeterminate on failure rather than unchanged. */
        if (strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv) == 0)
            strcpy(stamp, "00000000-000000");
    }

    char name[320];
    const int nn = snprintf(name, sizeof name, "trace-%s-%s.txt", safe,
                            stamp);
    if (nn < 0 || (size_t)nn >= sizeof name)
        return -2;

    const size_t dlen = strlen(dir);
    const size_t nlen = strlen(name);
    const size_t need = dlen + 1 + nlen + 1;
    if (need > outlen)
        return -2;
    /* Copied rather than printed, and not for speed.
     *
     * snprintf here is provably safe -- `need` is checked against
     * outlen on the line above -- but _FORTIFY_SOURCE cannot follow
     * that reasoning: it sees an unbounded dir and a 320-byte name
     * going into a 1024-byte destination and warns about the range it
     * cannot rule out. The package build turns that into an error, so
     * the hardened build failed where the developer's did not, which
     * is the third time this project has met that pattern.
     *
     * With the lengths already computed and checked, memcpy states the
     * same bound in a form the compiler can verify. */
    memcpy(out, dir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, name, nlen + 1);
    return 0;
}

sp_status sp_trace_export(sp_core *core, const char *call_id,
                          const char **path_out) {
    SP_ENTRY(core);
    if (!call_id || !*call_id || !path_out)
        return SP_ERR_INVALID_ARG;

    const sp_sip_message *msgs = NULL;
    size_t n = 0;
    /* 100000 is sp_siplog_query's own ceiling before it falls back to
       its default of 100 (see siplog.c) -- the largest limit that
       still means "everything the log layer will hand back", not a
       number picked here. */
    const sp_status rs = sp_siplog_read(core, call_id, 100000, &msgs, &n);
    if (rs != SP_OK)
        return rs;
    if (n == 0) {
        sp_core_detail(core, "no SIP messages found for call \"%s\"",
                       call_id);
        return SP_ERR_NOT_FOUND;
    }

    const char *dir = NULL;
    sp_status ds = sp_trace_get_export_dir(core, &dir);
    if (ds != SP_OK)
        return ds;
    /* Made again here: a directory can be removed between being
       chosen and being exported into. */
    if (sp_path_mkdir_p(dir) != 0) {
        sp_core_detail(core, "the trace export directory could not be "
                             "created");
        return SP_ERR_IO;
    }

    const int cp = compose_trace_path(core->trace_export_path,
                                      sizeof core->trace_export_path, dir,
                                      call_id);
    if (cp == -1) {
        sp_core_detail(core,
            "the call id is too long for a trace file name");
        return SP_ERR_INVALID_ARG;
    }
    if (cp != 0) {
        sp_core_detail(core,
            "the export directory is too long for a trace path");
        return SP_ERR_IO;
    }

    /* Sized from what is actually being written, plus slack per
       message for the separator line sp_trace_format adds around each
       one and for the header/warning block ahead of them. */
    size_t need = 1024;
    for (size_t i = 0; i < n; i++)
        need += (msgs[i].raw ? strlen(msgs[i].raw) : 0) +
                (msgs[i].method ? strlen(msgs[i].method) : 0) + 128;

    char *buf = malloc(need);
    if (!buf)
        return SP_ERR_INTERNAL;

    const int written = sp_trace_format(buf, need, msgs, n, call_id);
    if (written < 0) {
        free(buf);
        sp_core_detail(core, "the trace did not fit its buffer");
        return SP_ERR_INTERNAL;
    }

    /* Written to a temporary name and renamed into place, so a write
       that fails partway -- disk full, quota, a network mount hiccup
       -- cannot leave a truncated file sitting at the real export
       path. sound.c's sp_sound_import and config.c's config save use
       the same pattern: a rename within one directory is atomic, so
       a reader either sees a complete file or no file at all. */
    char tmp_path[sizeof core->trace_export_path + 8];
    const int tn = snprintf(tmp_path, sizeof tmp_path, "%s.tmp",
                            core->trace_export_path);
    if (tn < 0 || (size_t)tn >= sizeof tmp_path) {
        free(buf);
        sp_core_detail(core, "trace path too long for a temporary file");
        return SP_ERR_IO;
    }

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        sp_core_detail(core, "%s: %s", tmp_path, strerror(errno));
        free(buf);
        return SP_ERR_IO;
    }
    const size_t wrote = fwrite(buf, 1, (size_t)written, f);
    free(buf);
    bool ok = wrote == (size_t)written;
    /* Flushed and synced before the rename below -- the same
       discipline config.c's config save uses -- so a crash or power
       loss between the write and the rename cannot leave an empty
       file sitting at the export path. */
    if (ok && fflush(f) != 0)
        ok = false;
    if (ok && fsync(fileno(f)) != 0)
        ok = false;
    if (fclose(f) != 0)
        ok = false;
    if (!ok) {
        sp_core_detail(core, "%s: write failed", tmp_path);
        remove(tmp_path);
        return SP_ERR_IO;
    }

    if (rename(tmp_path, core->trace_export_path) != 0) {
        sp_core_detail(core, "%s: %s", core->trace_export_path,
                       strerror(errno));
        remove(tmp_path);
        return SP_ERR_IO;
    }

    *path_out = core->trace_export_path;
    return SP_OK;
}

sp_status sp_recording_get_format(sp_core *core, sp_recording_format *out) {
    SP_ENTRY(core);
    if (!out)
        return SP_ERR_INVALID_ARG;
    pthread_mutex_lock(&core->lock);
    *out = core->cfg->recording_format;
    pthread_mutex_unlock(&core->lock);
    return SP_OK;
}

sp_status sp_recording_set_format(sp_core *core, sp_recording_format fmt) {
    SP_ENTRY(core);
    if (fmt != SP_REC_WAV && fmt != SP_REC_WAV_ULAW && fmt != SP_REC_OPUS)
        return SP_ERR_INVALID_ARG;
    pthread_mutex_lock(&core->lock);
    const bool same = core->cfg->recording_format == fmt;
    core->cfg->recording_format = fmt;
    pthread_mutex_unlock(&core->lock);
    if (same)
        return SP_OK;
    return sp_core_persist(core);
}
