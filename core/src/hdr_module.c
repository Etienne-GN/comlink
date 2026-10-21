#include "core_internal.h"
#include "modules.h"
#include "hdr_rules.h"
#include "siplog.h"

#include <stdio.h>
#include <string.h>

#define SP_MAX_ACTIONS_PER_MESSAGE 64

/* A SIP message larger than this is truncated rather than
   allocated for on the signalling path. */
#define SP_SIPLOG_MAX_MESSAGE 8192

static sp_core *g_mod_core;

static void remove_all(pjsip_msg *msg, const char *name) {
    pj_str_t n = pj_str((char *)name);
    for (;;) {
        pjsip_hdr *h = pjsip_msg_find_hdr_by_name(msg, &n, NULL);
        if (!h)
            return;
        pj_list_erase(h);
    }
}

static void add_header(pj_pool_t *pool, pjsip_msg *msg, const char *name,
                       const char *value) {
    pj_str_t n = pj_str((char *)name);
    pj_str_t v = pj_str((char *)value);
    pjsip_generic_string_hdr *h = pjsip_generic_string_hdr_create(pool, &n, &v);
    if (h)
        pjsip_msg_add_hdr(msg, (pjsip_hdr *)h);
}

static void apply(pj_pool_t *pool, pjsip_msg *msg,
                  const sp_hdr_action **acts, size_t n) {
    for (size_t i = 0; i < n; i++) {
        switch (acts[i]->kind) {
        case SP_ACT_REMOVE:
            remove_all(msg, acts[i]->name);
            break;
        case SP_ACT_SET:
            /* Replace: a later set on the same name must win, per the
               documented rule semantics. */
            remove_all(msg, acts[i]->name);
            add_header(pool, msg, acts[i]->name, acts[i]->value);
            break;
        case SP_ACT_ADD:
            add_header(pool, msg, acts[i]->name, acts[i]->value);
            break;
        }
    }
}

/* Which configured account this message belongs to, matched on the
   From URI's user@host. NULL when nothing matches, which means only
   rules without an account scope apply. */
static const char *account_of(sp_core *core, pjsip_msg *msg, char *buf,
                              size_t buflen) {
    pjsip_from_hdr *from = pjsip_msg_find_hdr(msg, PJSIP_H_FROM, NULL);
    if (!from || !from->uri)
        return NULL;
    pjsip_sip_uri *uri = pjsip_uri_get_uri(from->uri);
    if (!uri)
        return NULL;
    snprintf(buf, buflen, "%.*s@%.*s", (int)uri->user.slen, uri->user.ptr,
             (int)uri->host.slen, uri->host.ptr);

    uint64_t ids[PJSUA_MAX_ACC];
    size_t n = 0;
    sp_ht_snapshot(core->accounts, ids, PJSUA_MAX_ACC, &n);
    if (n > PJSUA_MAX_ACC)
        n = PJSUA_MAX_ACC;
    for (size_t i = 0; i < n; i++) {
        void *data = NULL;
        if (sp_ht_lookup(core->accounts, ids[i], NULL, &data) != 0)
            continue;
        sp_account_rec *rec = data;
        if (rec && rec->aor && strcmp(rec->aor, buf) == 0)
            return rec->id;
    }
    return NULL;
}

static void run_rules(sp_core *core, pj_pool_t *pool, pjsip_msg *msg,
                      sp_direction dir) {
    if (!core || !core->cfg || core->cfg->n_rules == 0)
        return;

    char aor[384];
    sp_rule_ctx ctx = {0};
    ctx.direction = dir;
    if (msg->type == PJSIP_REQUEST_MSG) {
        ctx.message = SP_MSG_REQUEST;
        ctx.method = msg->line.req.method.name.ptr
                         ? msg->line.req.method.name.ptr : NULL;
        ctx.status_code = 0;
    } else {
        ctx.message = SP_MSG_RESPONSE;
        ctx.status_code = msg->line.status.code;
        /* A response's method comes from its CSeq, so rules matching a
           response by method are refused at config load instead. */
        ctx.method = NULL;
    }
    ctx.account = account_of(core, msg, aor, sizeof aor);

    /* method.name is not NUL-terminated in pjsip; copy it out. */
    char method[32] = {0};
    if (msg->type == PJSIP_REQUEST_MSG) {
        int len = (int)msg->line.req.method.name.slen;
        if (len > (int)sizeof method - 1)
            len = (int)sizeof method - 1;
        memcpy(method, msg->line.req.method.name.ptr, (size_t)len);
        ctx.method = method;
    }

    const sp_hdr_action *acts[SP_MAX_ACTIONS_PER_MESSAGE];
    size_t n = 0;
    if (sp_rules_eval(core->cfg->rules, core->cfg->n_rules, &ctx, acts,
                      SP_MAX_ACTIONS_PER_MESSAGE, &n) != SP_OK)
        return;   /* too many actions for one message; leave it untouched */
    if (n == 0)
        return;
    apply(pool, msg, acts, n);
}

/* Capture after the rules have run, so the log shows what actually
   goes on the wire rather than what we were handed. */
static void capture(sp_core *core, pjsip_msg *msg, const char *direction) {
    if (!core || !core->siplog)
        return;
    /* Checked here, not only where the log is created: the module can
       be switched off while the phone is running, and the operator
       should not have to restart to be believed. */
    if (!sp_config_module_enabled(core->cfg, SP_MODULE_SIP_TRACE))
        return;

    char buf[SP_SIPLOG_MAX_MESSAGE];
    pj_ssize_t len = pjsip_msg_print(msg, buf, sizeof buf);
    if (len <= 0)
        return;

    char method[32] = {0};
    int status_code = 0;
    if (msg->type == PJSIP_REQUEST_MSG) {
        int n = (int)msg->line.req.method.name.slen;
        if (n > (int)sizeof method - 1)
            n = (int)sizeof method - 1;
        memcpy(method, msg->line.req.method.name.ptr, (size_t)n);
    } else {
        status_code = msg->line.status.code;
    }

    char call_id[192] = {0};
    pjsip_cid_hdr *cid = pjsip_msg_find_hdr(msg, PJSIP_H_CALL_ID, NULL);
    if (cid && cid->id.slen > 0) {
        int n = (int)cid->id.slen;
        if (n > (int)sizeof call_id - 1)
            n = (int)sizeof call_id - 1;
        memcpy(call_id, cid->id.ptr, (size_t)n);
    }

    char aor[384];
    const char *account = account_of(core, msg, aor, sizeof aor);

    sp_siplog_capture(core->siplog, buf, (size_t)len, direction,
                      method[0] ? method : NULL, status_code,
                      call_id[0] ? call_id : NULL, account);

    sp_event ev = {0};
    ev.type = SP_EV_SIP_MESSAGE;
    ev.u.sip.outbound = (direction[0] == 't');
    /* Explicit precision: the event's buffers are smaller than the
       ones the database gets, and truncating there is intended rather
       than accidental. */
    snprintf(ev.u.sip.method, sizeof ev.u.sip.method, "%.*s",
             (int)sizeof ev.u.sip.method - 1, method);
    ev.u.sip.status_code = status_code;
    snprintf(ev.u.sip.call_id, sizeof ev.u.sip.call_id, "%.*s",
             (int)sizeof ev.u.sip.call_id - 1, call_id);
    sp_core_post(core, &ev);
}

static pj_status_t on_tx_request(pjsip_tx_data *tdata) {
    sp_core *core = g_mod_core;
    if (!core)
        return PJ_SUCCESS;
    run_rules(core, tdata->pool, tdata->msg, SP_DIR_TX);
    /* The message may already have been serialised into the tx buffer,
       so the cached print must be dropped or the edit never reaches
       the wire. */
    pjsip_tx_data_invalidate_msg(tdata);
    capture(core, tdata->msg, "tx");
    return PJ_SUCCESS;
}

static pj_status_t on_tx_response(pjsip_tx_data *tdata) {
    sp_core *core = g_mod_core;
    if (!core)
        return PJ_SUCCESS;
    run_rules(core, tdata->pool, tdata->msg, SP_DIR_TX);
    pjsip_tx_data_invalidate_msg(tdata);
    capture(core, tdata->msg, "tx");
    return PJ_SUCCESS;
}

static pj_bool_t on_rx_request(pjsip_rx_data *rdata) {
    sp_core *core = g_mod_core;
    if (core) {
        run_rules(core, rdata->tp_info.pool, rdata->msg_info.msg, SP_DIR_RX);
        capture(core, rdata->msg_info.msg, "rx");
    }
    return PJ_FALSE;   /* let pjsua carry on handling it */
}

static pj_bool_t on_rx_response(pjsip_rx_data *rdata) {
    sp_core *core = g_mod_core;
    if (core) {
        run_rules(core, rdata->tp_info.pool, rdata->msg_info.msg, SP_DIR_RX);
        capture(core, rdata->msg_info.msg, "rx");
    }
    return PJ_FALSE;
}

static pjsip_module sp_mod = {
    NULL, NULL,                          /* prev, next */
    { "mod-comlink-hdr", 17 },         /* name */
    -1,                                  /* id */
    PJSIP_MOD_PRIORITY_TRANSPORT_LAYER,  /* closest to the wire on tx */
    NULL,                                /* load */
    NULL,                                /* start */
    NULL,                                /* stop */
    NULL,                                /* unload */
    &on_rx_request,
    &on_rx_response,
    &on_tx_request,
    &on_tx_response,
    NULL                                 /* on_tsx_state */
};

sp_status sp_hdr_module_register(sp_core *core) {
    if (!core)
        return SP_ERR_INVALID_ARG;
    if (g_mod_core)
        return SP_ERR_STATE;
    pj_status_t st = pjsip_endpt_register_module(pjsua_get_pjsip_endpt(),
                                                 &sp_mod);
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_INTERNAL,
                            "pjsip_endpt_register_module");
    g_mod_core = core;
    return SP_OK;
}

void sp_hdr_module_unregister(void) {
    if (!g_mod_core)
        return;
    pjsip_endpt_unregister_module(pjsua_get_pjsip_endpt(), &sp_mod);
    g_mod_core = NULL;
}
