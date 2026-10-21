#include "transcap.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bridge.h"
#include "core_internal.h"
#include "modules.h"
#include "pcmsink.h"
#include "transcriber.h"

struct sp_transcap {
    sp_core            *core;
    int64_t             history_id;
    pj_pool_t          *pool;
    pjmedia_port       *far_port;
    pjmedia_port       *mic_port;
    pjsua_conf_port_id  far_slot;
    pjsua_conf_port_id  mic_slot;
    pjsua_conf_port_id  call_slot;
    bool                mic_on;
    char                far_path[1024];
    char                mic_path[1024];
    char               *far_label;
    atomic_int          open;
    atomic_bool         ok;
};

/* Whoever lets go of the last reference: hands the call on, or throws
   it away. */
static void finish(sp_transcap *c) {
    sp_core *core = c->core;
    pthread_mutex_lock(&core->lock);
    const bool on = sp_config_module_enabled(core->cfg, SP_MODULE_TRANSCRIPT);
    pthread_mutex_unlock(&core->lock);

    if (on && atomic_load(&c->ok)) {
        sp_transcriber_submit(core->transcriber, c->history_id,
                              c->far_label, c->far_path, c->mic_path);
    } else {
        unlink(c->far_path);
        unlink(c->mic_path);
        sp_transcriber_set_state(core->transcriber, c->history_id,
                                 on ? SP_TRANSCRIPT_FAILED
                                    : SP_TRANSCRIPT_NONE,
                                 "the call's audio could not be captured");
    }
    free(c->far_label);
    free(c);
}

static void release(sp_transcap *c) {
    if (atomic_fetch_sub(&c->open, 1) == 1)
        finish(c);
}

/* On the bridge's thread, once per side, after its file is closed. */
static void side_closed(void *user, bool ok) {
    sp_transcap *c = user;
    if (!ok)
        atomic_store(&c->ok, false);
    release(c);
}

/* Each side holds a reference from the moment its sink exists, and
   gives it back from on_destroy. */
static sp_status add_side(sp_transcap *c, const char *path,
                          pjmedia_port **port, pjsua_conf_port_id *slot) {
    atomic_fetch_add(&c->open, 1);
    sp_status st = sp_pcmsink_create(c->pool, path, SP_REC_BRIDGE_CLOCK_RATE,
                                     SP_REC_BRIDGE_SAMPLES_PER_FRAME,
                                     side_closed, c, port);
    if (st != SP_OK) {
        atomic_fetch_sub(&c->open, 1);
        return st;
    }
    if (pjsua_conf_add_port(c->pool, *port, slot) != PJ_SUCCESS) {
        /* Never in the bridge, so on_destroy runs here and now and
           gives the reference back itself. */
        atomic_store(&c->ok, false);
        pjmedia_port *p = *port;
        *port = NULL;
        pjmedia_port_destroy(p);
        return SP_ERR_MEDIA;
    }
    return SP_OK;
}

/* Nothing of c is touched once the port is destroyed: the last
   on_destroy frees it, possibly before this returns. */
static void remove_side(pjmedia_port *port, pjsua_conf_port_id slot) {
    if (!port)
        return;
    /* recorder_stop's order: out of the bridge, then destroy, which
       the bridge finishes on its own clock. */
    pjsua_conf_remove_port(slot);
    pjmedia_port_destroy(port);
}

sp_transcap *sp_transcap_start(sp_core *core, int64_t history_id,
                               pjsua_conf_port_id call_slot) {
    if (!core || !core->transcriber || !core->transcript_scratch
        || history_id <= 0 || call_slot == PJSUA_INVALID_ID)
        return NULL;
    sp_transcap *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->core = core;
    c->history_id = history_id;
    c->far_slot = c->mic_slot = PJSUA_INVALID_ID;
    c->call_slot = call_slot;
    /* This function's own reference, so nothing frees c while it is
       still being built. */
    atomic_init(&c->open, 1);
    atomic_init(&c->ok, true);
    snprintf(c->far_path, sizeof c->far_path, "%s/%lld-far.pcm",
             core->transcript_scratch, (long long)history_id);
    snprintf(c->mic_path, sizeof c->mic_path, "%s/%lld-mic.pcm",
             core->transcript_scratch, (long long)history_id);
    c->pool = pjsua_pool_create("sp_transcap", 1024, 1024);
    if (!c->pool) {
        free(c);
        return NULL;
    }

    if (add_side(c, c->far_path, &c->far_port, &c->far_slot) == SP_OK
        && add_side(c, c->mic_path, &c->mic_port, &c->mic_slot) == SP_OK
        && pjsua_conf_connect(call_slot, c->far_slot) == PJ_SUCCESS) {
        atomic_fetch_sub(&c->open, 1);
        sp_transcap_hear_mic(c, true);
        return c;
    }

    /* Whatever made it into the bridge comes out; the last reference
       let go of cleans up, and the call reads FAILED. */
    atomic_store(&c->ok, false);
    pj_pool_t *pool = c->pool;
    pjmedia_port *fp = c->far_port, *mp = c->mic_port;
    pjsua_conf_port_id fs = c->far_slot, ms = c->mic_slot;
    remove_side(fp, fs);
    remove_side(mp, ms);
    release(c);
    pj_pool_release(pool);
    return NULL;
}

void sp_transcap_follow(sp_transcap *c, pjsua_conf_port_id call_slot) {
    if (!c || call_slot == PJSUA_INVALID_ID || call_slot == c->call_slot)
        return;
    if (c->call_slot != PJSUA_INVALID_ID)
        pjsua_conf_disconnect(c->call_slot, c->far_slot);
    if (pjsua_conf_connect(call_slot, c->far_slot) == PJ_SUCCESS)
        c->call_slot = call_slot;
}

void sp_transcap_hear_mic(sp_transcap *c, bool on) {
    if (!c || c->mic_on == on)
        return;
    const pj_status_t st = on ? pjsua_conf_connect(0, c->mic_slot)
                              : pjsua_conf_disconnect(0, c->mic_slot);
    if (st == PJ_SUCCESS)
        c->mic_on = on;
}

pjsua_conf_port_id sp_transcap_mic_slot(const sp_transcap *c) {
    return c ? c->mic_slot : PJSUA_INVALID_ID;
}

void sp_transcap_stop(sp_transcap *c, const char *far_label) {
    if (!c)
        return;
    c->far_label = strdup(far_label && *far_label ? far_label : "Caller");
    /* Copied out first: c may be freed by the second destroy. The
       sinks hold pools of their own; this one only backed the
       bridge's bookkeeping, released here as recorder_stop does. */
    pj_pool_t *pool = c->pool;
    pjmedia_port *fp = c->far_port, *mp = c->mic_port;
    pjsua_conf_port_id fs = c->far_slot, ms = c->mic_slot;
    remove_side(fp, fs);
    remove_side(mp, ms);
    pj_pool_release(pool);
}
