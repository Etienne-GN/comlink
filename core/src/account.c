#include "core_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *transport_param(sp_transport t) {
    switch (t) {
    case SP_TP_TCP: return ";transport=tcp";
    case SP_TP_TLS: return ";transport=tls";
    case SP_TP_UDP: break;
    }
    return "";
}

static sp_account_rec *rec_of(sp_core *core, sp_account_id acc) {
    void *data = NULL;
    if (sp_ht_lookup(core->accounts, acc, NULL, &data) != 0)
        return NULL;
    return data;
}

static int id_in_use(sp_core *core, const char *id) {
    uint64_t ids[PJSUA_MAX_ACC];
    size_t n = 0;
    sp_ht_snapshot(core->accounts, ids, PJSUA_MAX_ACC, &n);
    if (n > PJSUA_MAX_ACC)
        n = PJSUA_MAX_ACC;
    for (size_t i = 0; i < n; i++) {
        void *data = NULL;
        if (sp_ht_lookup(core->accounts, ids[i], NULL, &data) != 0)
            continue;
        sp_account_rec *r = data;
        if (r && r->id && strcmp(r->id, id) == 0)
            return 1;
    }
    return 0;
}

sp_status sp_account_add_locked(sp_core *core, const sp_account_cfg *cfg,
                                sp_account_id *out, bool persist) {
    if (!cfg || !out)
        return SP_ERR_INVALID_ARG;
    *out = 0;
    if (!cfg->id || !*cfg->id || !cfg->user || !*cfg->user ||
        !cfg->domain || !*cfg->domain)
        return SP_ERR_INVALID_ARG;

    /* Zero means "the default", which is what a caller that memset
       the struct has asked for; anything else has to be a value the
       configuration file could hold, because this account is going to
       be written to one. The config loader enforced this range and
       this function did not, so an account added through the API
       could produce a file the core then refused to load. */
    sp_account_cfg norm = *cfg;
    if (norm.reg_expires_s == 0)
        norm.reg_expires_s = SP_REG_EXPIRES_DEFAULT;
    if (norm.reg_expires_s < SP_REG_EXPIRES_MIN ||
        norm.reg_expires_s > SP_REG_EXPIRES_MAX) {
        sp_core_detail(core, "reg_expires_s %d is outside %d..%d",
                       norm.reg_expires_s, SP_REG_EXPIRES_MIN,
                       SP_REG_EXPIRES_MAX);
        return SP_ERR_INVALID_ARG;
    }
    cfg = &norm;

    if (id_in_use(core, cfg->id)) {
        sp_core_detail(core, "account id \"%s\" is already in use", cfg->id);
        return SP_ERR_CONFIG;
    }

    const char *tp = transport_param(cfg->transport);

    char uri[512], reg[512], proxy[512];
    snprintf(uri, sizeof uri, "sip:%s@%s%s", cfg->user, cfg->domain, tp);

    /* Bind the account to the transport it asked for. Without this,
       pjsua resolves a transport from the account URI, which has no
       port and therefore defaults to 5060/5061 and opens a connection
       to whatever is or is not listening there. That connection then
       supplies the Via and Contact, so a refused one breaks outgoing
       calls entirely. */
    pjsua_transport_id tp_id = PJSUA_INVALID_ID;
    sp_status ts = sp_core_ensure_transport(core, cfg->transport, &tp_id);
    if (ts != SP_OK)
        return ts;

    pjsua_acc_config acc;
    pjsua_acc_config_default(&acc);
    acc.id = pj_str(uri);
    acc.transport_id = tp_id;

    switch (cfg->srtp) {
    case SP_SRTP_OPTIONAL:  acc.use_srtp = PJMEDIA_SRTP_OPTIONAL; break;
    case SP_SRTP_MANDATORY: acc.use_srtp = PJMEDIA_SRTP_MANDATORY; break;
    case SP_SRTP_DISABLED:  acc.use_srtp = PJMEDIA_SRTP_DISABLED; break;
    }
    /* Left at 0 deliberately: SRTP does not additionally demand a
       secure transport, because plenty of PBXes run SDES over plain
       UDP signalling and refusing them would be surprising. The
       weakness of that combination is warned about instead. */
    acc.srtp_secure_signaling = 0;

    if (cfg->srtp == SP_SRTP_MANDATORY && cfg->transport != SP_TP_TLS) {
        sp_event ev = {0};
        char warn[256];
        snprintf(warn, sizeof warn,
                 "account \"%s\": SRTP is mandatory but the transport is not "
                 "TLS, so the SDES keys travel in cleartext SDP",
                 cfg->id);
        ev.type = SP_EV_LOG;
        ev.u.log.level = 2;
        ev.u.log.msg = warn;
        sp_core_post(core, &ev);
    }

    /* The registrar is always configured, so an account can be
       registered manually later. do_register controls only whether it
       registers at startup; without a reg_uri, pjsua could never
       register it at all, which would make manual registration
       impossible for exactly the accounts you most want to switch on. */
    if (cfg->registrar && *cfg->registrar)
        snprintf(reg, sizeof reg, "%s%s", cfg->registrar, tp);
    else
        snprintf(reg, sizeof reg, "sip:%s%s", cfg->domain, tp);
    acc.reg_uri = pj_str(reg);
    acc.reg_timeout = (unsigned)cfg->reg_expires_s;
    acc.register_on_acc_add = cfg->do_register ? PJ_TRUE : PJ_FALSE;

    if (cfg->outbound_proxy && *cfg->outbound_proxy) {
        snprintf(proxy, sizeof proxy, "%s%s", cfg->outbound_proxy, tp);
        acc.proxy[0] = pj_str(proxy);
        acc.proxy_cnt = 1;
    }

    if (cfg->password && *cfg->password) {
        acc.cred_count = 1;
        acc.cred_info[0].realm = pj_str("*");
        acc.cred_info[0].scheme = pj_str("digest");
        acc.cred_info[0].username =
            pj_str((char *)(cfg->auth_user ? cfg->auth_user : cfg->user));
        acc.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
        acc.cred_info[0].data = pj_str((char *)cfg->password);
    }

    pjsua_acc_id native = PJSUA_INVALID_ID;
    pj_status_t st = pjsua_acc_add(&acc, PJ_FALSE, &native);
    if (st != PJ_SUCCESS) {
        char what[192];
        snprintf(what, sizeof what, "pjsua_acc_add for account \"%s\"",
                 cfg->id);
        return sp_core_fail(core, st, SP_ERR_CONFIG, what);
    }

    sp_account_rec *rec = calloc(1, sizeof *rec);
    if (!rec) {
        pjsua_acc_del(native);
        return SP_ERR_INTERNAL;
    }
    char aor[384];
    snprintf(aor, sizeof aor, "%s@%s", cfg->user, cfg->domain);
    rec->id = strdup(cfg->id);
    rec->aor = strdup(aor);
    rec->domain = strdup(cfg->domain);
    rec->native = native;
    rec->reg_expires_s = cfg->reg_expires_s;
    if (!rec->id || !rec->aor || !rec->domain) {
        free(rec->id);
        free(rec->aor);
        free(rec->domain);
        free(rec);
        pjsua_acc_del(native);
        return SP_ERR_INTERNAL;
    }

    uint64_t handle = sp_ht_add(core->accounts, native, rec);
    if (handle == 0) {
        free(rec->id);
        free(rec->aor);
        free(rec->domain);
        free(rec);
        pjsua_acc_del(native);
        return SP_ERR_INTERNAL;
    }
    if (persist) {
        sp_status s = sp_config_add_account(core->cfg, cfg);
        if (s == SP_OK)
            s = sp_core_persist(core);
        if (s != SP_OK) {
            /* Roll the live account back rather than leave the running
               phone and the config file disagreeing. */
            sp_config_remove_account(core->cfg, cfg->id);
            sp_ht_retire(core->accounts, handle);
            free(rec->id);
            free(rec->aor);
            free(rec->domain);
            free(rec);
            pjsua_acc_del(native);
            *out = 0;
            return s;
        }
    }

    *out = handle;
    return SP_OK;
}

sp_status sp_account_add(sp_core *core, const sp_account_cfg *cfg,
                         sp_account_id *out) {
    SP_ENTRY(core);
    if (!core->pj_ready)
        return SP_ERR_STATE;
    return sp_account_add_locked(core, cfg, out, true);
}

sp_status sp_account_add_transient(sp_core *core, const sp_account_cfg *cfg,
                                   sp_account_id *out) {
    SP_ENTRY(core);
    if (!core->pj_ready)
        return SP_ERR_STATE;
    return sp_account_add_locked(core, cfg, out, false);
}

sp_status sp_account_get_cfg(sp_core *core, sp_account_id acc,
                             sp_account_cfg *out) {
    SP_ENTRY(core);
    if (!out)
        return SP_ERR_INVALID_ARG;
    sp_account_rec *rec = rec_of(core, acc);
    if (!rec)
        return SP_ERR_NOT_FOUND;
    const sp_account_cfg *found = sp_config_find_account(core->cfg, rec->id);
    if (!found)
        return SP_ERR_NOT_FOUND;
    *out = *found;
    return SP_OK;
}

sp_status sp_account_remove(sp_core *core, sp_account_id acc) {
    SP_ENTRY(core);
    void *data = NULL;
    int native = PJSUA_INVALID_ID;
    if (sp_ht_lookup(core->accounts, acc, &native, &data) != 0)
        return SP_ERR_NOT_FOUND;
    sp_account_rec *rec = data;

    pj_status_t st = pjsua_acc_del(native);
    sp_ht_retire(core->accounts, acc);
    if (rec && rec->id) {
        sp_config_remove_account(core->cfg, rec->id);
        sp_status ps = sp_core_persist(core);
        if (ps != SP_OK)
            /* The account is gone from the running phone; say so
               loudly rather than pretend the removal was clean. */
            fprintf(stderr, "comlink: account removed but config not "
                            "saved: %s\n", sp_last_error_detail(core));
    }
    if (rec) {
        free(rec->id);
        free(rec->aor);
        free(rec->domain);
        free(rec);
    }
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_INTERNAL, "pjsua_acc_del");
    return SP_OK;
}

sp_status sp_account_set_registration(sp_core *core, sp_account_id acc,
                                      bool on) {
    SP_ENTRY(core);
    int native = PJSUA_INVALID_ID;
    void *data = NULL;
    if (sp_ht_lookup(core->accounts, acc, &native, &data) != 0)
        return SP_ERR_NOT_FOUND;

    pj_status_t st = pjsua_acc_set_registration(native, on ? PJ_TRUE : PJ_FALSE);
    sp_status result = SP_OK;
    if (st != PJ_SUCCESS) {
        if (!on && st == PJ_EINVALIDOP) {
            /* Asked to unregister an account holding no registration.
               The caller's goal state is already true, so this is not
               an error. Scoped to exactly this status and direction. */
            result = SP_OK;
        } else if (st == PJSIP_EBUSY) {
            /* pjsua will not change registration while a REGISTER
               transaction is outstanding. That is a state problem, not
               a transport failure, and the distinction matters: the
               caller should retry, not go looking at the network. */
            result = sp_core_fail(core, st, SP_ERR_STATE,
                                  "a REGISTER transaction is still in "
                                  "flight; the preference was saved, retry "
                                  "once it settles");
        } else {
            result = sp_core_fail(core, st, SP_ERR_TRANSPORT,
                                  "pjsua_acc_set_registration");
        }
    }

    /* The intent is recorded even when the protocol action was refused,
       so a restart honours what was asked for rather than reverting to
       whatever the file happened to say. */
    sp_account_rec *rec = data;
    if (rec && rec->id) {
        sp_account_cfg *stored = NULL;
        for (size_t i = 0; i < core->cfg->n_accounts; i++) {
            if (strcmp(core->cfg->accounts[i].id, rec->id) == 0) {
                stored = &core->cfg->accounts[i];
                break;
            }
        }
        if (stored && stored->do_register != on) {
            stored->do_register = on;
            if (sp_core_persist(core) != SP_OK)
                fprintf(stderr, "comlink: registration changed but config "
                                "not saved: %s\n",
                        sp_last_error_detail(core));
        }
        if (!on && result == SP_OK) {
            rec->registered = false;
            snprintf(rec->detail, sizeof rec->detail, "unregistered manually");
        }
    }
    return result;
}

sp_status sp_account_get_state(sp_core *core, sp_account_id acc,
                               sp_account_state *out) {
    SP_ENTRY(core);
    if (!out)
        return SP_ERR_INVALID_ARG;
    sp_account_rec *rec = rec_of(core, acc);
    if (!rec) {
        sp_core_detail(core, "account handle %llu is unknown or retired",
                 (unsigned long long)acc);
        return SP_ERR_NOT_FOUND;
    }
    out->registered = rec->registered;
    out->reg_code = rec->reg_code;
    out->reg_expires_s = rec->reg_expires_s;
    out->detail = rec->detail[0] ? rec->detail : NULL;
    return SP_OK;
}

sp_status sp_account_list(sp_core *core, sp_account_id *ids, size_t cap,
                          size_t *n_out) {
    SP_ENTRY(core);
    if (!n_out)
        return SP_ERR_INVALID_ARG;
    sp_ht_snapshot(core->accounts, ids, cap, n_out);
    return SP_OK;
}

const char *sp_account_id_of(sp_core *core, sp_account_id acc) {
    if (!core)
        return NULL;
    sp_account_rec *rec = rec_of(core, acc);
    return rec ? rec->id : NULL;
}

const char *sp_account_domain_of(sp_core *core, sp_account_id acc) {
    if (!core)
        return NULL;
    sp_account_rec *rec = rec_of(core, acc);
    return rec ? rec->domain : NULL;
}

static void on_reg_state2(pjsua_acc_id acc_id, pjsua_reg_info *info) {
    sp_core *core = sp_core_active();
    if (!core)
        return;

    uint64_t handle = sp_ht_by_native(core->accounts, acc_id);
    if (handle == 0)
        return;   /* account was removed; a handle we cannot resolve is
                     worse than no event */

    void *data = NULL;
    if (sp_ht_lookup(core->accounts, handle, NULL, &data) != 0)
        return;
    sp_account_rec *rec = data;

    int code = 0;
    unsigned expires = 0;
    const char *reason = NULL;
    if (info && info->cbparam) {
        code = info->cbparam->code;
        expires = info->cbparam->expiration;
        if (info->cbparam->reason.slen > 0)
            reason = info->cbparam->reason.ptr;
    }

    rec->reg_code = code;
    rec->registered = (code / 100 == 2) && info && info->renew;
    if (expires > 0)
        rec->reg_expires_s = (int)expires;
    if (rec->registered)
        rec->detail[0] = '\0';
    else
        snprintf(rec->detail, sizeof rec->detail, "%d %.*s", code,
                 (info && info->cbparam) ? (int)info->cbparam->reason.slen : 0,
                 reason ? reason : "");

    sp_event ev = {0};
    ev.type = SP_EV_ACCOUNT_STATE;
    ev.u.account.acc = handle;
    ev.u.account.reg_code = code;
    ev.u.account.registered = rec->registered;
    ev.u.account.detail = rec->detail[0] ? rec->detail : NULL;
    sp_core_post(core, &ev);
}

void sp_account_install_callbacks(pjsua_callback *cb) {
    cb->on_reg_state2 = &on_reg_state2;
}

sp_status sp_account_rename(sp_core *core, sp_account_id acc,
                            const char *new_id, size_t *rules_updated) {
    SP_ENTRY(core);
    if (rules_updated)
        *rules_updated = 0;
    if (!new_id || !*new_id)
        return SP_ERR_INVALID_ARG;

    /* Detail messages are built after the lock is released: every one
       of them takes core->lock itself, and taking it twice on one path
       is how this project deadlocked itself once already. */
    enum { OK, GONE, TAKEN, NOMEM, SAME } outcome = OK;
    size_t moved = 0;
    char old_id[128] = {0};

    pthread_mutex_lock(&core->lock);
    void *data = NULL;
    if (sp_ht_lookup(core->accounts, acc, NULL, &data) != 0 || !data) {
        outcome = GONE;
    } else {
        sp_account_rec *rec = data;
        snprintf(old_id, sizeof old_id, "%s", rec->id ? rec->id : "");
        if (rec->id && strcmp(rec->id, new_id) == 0) {
            outcome = SAME;
        } else if (sp_config_find_account(core->cfg, new_id)) {
            outcome = TAKEN;
        } else {
            char *copy = strdup(new_id);
            if (!copy) {
                outcome = NOMEM;
            } else {
                /* The configured account entry. */
                for (size_t i = 0; i < core->cfg->n_accounts; i++) {
                    sp_account_cfg *a = &core->cfg->accounts[i];
                    if (!a->id || strcmp(a->id, old_id) != 0)
                        continue;
                    char *ac = strdup(new_id);
                    if (ac) {
                        free((void *)a->id);
                        a->id = ac;
                    }
                    break;
                }
                /* Every rule scoped to the old name follows it. */
                for (size_t i = 0; i < core->cfg->n_rules; i++) {
                    sp_hdr_rule *r = &core->cfg->rules[i];
                    if (!r->account || strcmp(r->account, old_id) != 0)
                        continue;
                    char *rc = strdup(new_id);
                    if (!rc)
                        continue;
                    free(r->account);
                    r->account = rc;
                    moved++;
                }
                free(rec->id);
                rec->id = copy;
            }
        }
    }
    pthread_mutex_unlock(&core->lock);

    switch (outcome) {
    case GONE:
        sp_core_detail(core, "account handle %llu is unknown or retired",
                       (unsigned long long)acc);
        return SP_ERR_NOT_FOUND;
    case TAKEN:
        sp_core_detail(core, "an account called \"%s\" already exists",
                       new_id);
        return SP_ERR_INVALID_ARG;
    case NOMEM:
        return SP_ERR_INTERNAL;
    case SAME:
        return SP_OK;      /* nothing to do is not a failure */
    case OK:
        break;
    }

    if (rules_updated)
        *rules_updated = moved;
    return sp_core_persist(core);
}

static bool blank(const char *s) {
    return !s || !*s;
}

sp_status sp_account_validate(const sp_account_cfg *cfg, char *detail,
                              size_t detail_len) {
    const char *why = NULL;
    if (!cfg)
        why = "There is nothing to check.";
    else if (blank(cfg->id))
        why = "Name is required.";
    else if (strchr(cfg->id, ' '))
        why = "Name cannot contain spaces.";
    else if (blank(cfg->user))
        why = "User is required.";
    else if (blank(cfg->domain))
        why = "Domain is required.";
    else if (cfg->do_register && blank(cfg->password))
        why = "A password is needed to register. Untick registration if "
              "this account only places direct IP calls.";

    const bool bad_expiry =
        !why && cfg->reg_expires_s != 0 &&
        (cfg->reg_expires_s < SP_REG_EXPIRES_MIN ||
         cfg->reg_expires_s > SP_REG_EXPIRES_MAX);

    if (detail && detail_len) {
        if (bad_expiry)
            snprintf(detail, detail_len,
                     "Registration expiry must be between %d and %d "
                     "seconds.", SP_REG_EXPIRES_MIN, SP_REG_EXPIRES_MAX);
        else
            snprintf(detail, detail_len, "%s", why ? why : "");
    }
    return (why || bad_expiry) ? SP_ERR_INVALID_ARG : SP_OK;
}

/* Deep copy of an sp_account_cfg, so the original survives the removal
   that frees the configuration it pointed into. */
typedef struct {
    char *id, *user, *domain, *auth_user, *password;
    char *registrar, *outbound_proxy, *stun_server;
    sp_account_cfg cfg;
} cfg_copy;

static char *dup_or_null(const char *s) {
    return s ? strdup(s) : NULL;
}

static void cfg_copy_free(cfg_copy *c) {
    free(c->id); free(c->user); free(c->domain); free(c->auth_user);
    free(c->password); free(c->registrar); free(c->outbound_proxy);
    free(c->stun_server);
    memset(c, 0, sizeof *c);
}

static bool cfg_copy_take(cfg_copy *c, const sp_account_cfg *src) {
    memset(c, 0, sizeof *c);
    c->cfg = *src;
    c->id = dup_or_null(src->id);
    c->user = dup_or_null(src->user);
    c->domain = dup_or_null(src->domain);
    c->auth_user = dup_or_null(src->auth_user);
    c->password = dup_or_null(src->password);
    c->registrar = dup_or_null(src->registrar);
    c->outbound_proxy = dup_or_null(src->outbound_proxy);
    c->stun_server = dup_or_null(src->stun_server);
    if ((src->id && !c->id) || (src->user && !c->user) ||
        (src->domain && !c->domain) || (src->auth_user && !c->auth_user) ||
        (src->password && !c->password) ||
        (src->registrar && !c->registrar) ||
        (src->outbound_proxy && !c->outbound_proxy) ||
        (src->stun_server && !c->stun_server)) {
        cfg_copy_free(c);
        return false;
    }
    c->cfg.id = c->id;
    c->cfg.user = c->user;
    c->cfg.domain = c->domain;
    c->cfg.auth_user = c->auth_user;
    c->cfg.password = c->password;
    c->cfg.registrar = c->registrar;
    c->cfg.outbound_proxy = c->outbound_proxy;
    c->cfg.stun_server = c->stun_server;
    return true;
}

sp_status sp_account_update(sp_core *core, sp_account_id acc,
                            const sp_account_cfg *cfg, sp_account_id *out,
                            size_t *rules_updated) {
    if (!cfg || !out)
        return SP_ERR_INVALID_ARG;
    *out = 0;
    if (rules_updated)
        *rules_updated = 0;

    /* Snapshot before anything is removed: sp_account_get_cfg hands
       back pointers into the configuration that the removal frees. */
    sp_account_cfg found = {0};
    sp_status st = sp_account_get_cfg(core, acc, &found);
    if (st != SP_OK)
        return st;
    cfg_copy before;
    if (!cfg_copy_take(&before, &found))
        return SP_ERR_INTERNAL;

    cfg_copy wanted;
    if (!cfg_copy_take(&wanted, cfg)) {
        cfg_copy_free(&before);
        return SP_ERR_INTERNAL;
    }
    /* Added back under the OLD name and renamed afterwards. The other
       order would, on a failure between the two, leave header rules
       scoped to a name no account carries. */
    wanted.cfg.id = before.cfg.id;

    st = sp_account_remove(core, acc);
    if (st != SP_OK) {
        cfg_copy_free(&before);
        cfg_copy_free(&wanted);
        return st;
    }

    sp_account_id added = 0;
    st = sp_account_add(core, &wanted.cfg, &added);
    if (st != SP_OK) {
        sp_account_id restored = 0;
        const sp_status rb = sp_account_add(core, &before.cfg, &restored);
        if (rb != SP_OK)
            sp_core_detail(core, "the account could not be updated, and "
                                 "restoring the previous settings also "
                                 "failed");
        cfg_copy_free(&before);
        cfg_copy_free(&wanted);
        return st;
    }

    if (cfg->id && before.cfg.id && strcmp(cfg->id, before.cfg.id) != 0) {
        /* A rename that fails leaves the rest of the edit standing:
           the settings are already the new ones and only the name did
           not move. Reported, not rolled back -- rolling back a
           successful add to undo a failed rename would be the more
           destructive choice. */
        const sp_status rn =
            sp_account_rename(core, added, cfg->id, rules_updated);
        if (rn != SP_OK) {
            *out = added;
            cfg_copy_free(&before);
            cfg_copy_free(&wanted);
            return rn;
        }
    }

    *out = added;
    cfg_copy_free(&before);
    cfg_copy_free(&wanted);
    return SP_OK;
}

/* The credential, without the configuration file ever seeing it.
 *
 * pjsua has no "set the password" call, so the account's whole config
 * is read back, the one credential replaced, and the config applied
 * again. Read back rather than rebuilt from ours: pjsua fills in
 * defaults and derived values at add time, and rebuilding would
 * quietly reset whichever of them we do not think to copy. */
sp_status sp_account_set_password(sp_core *core, sp_account_id acc,
                                  const char *password) {
    SP_ENTRY(core);
    void *data = NULL;
    int native = PJSUA_INVALID_ID;
    if (sp_ht_lookup(core->accounts, acc, &native, &data) != 0)
        return SP_ERR_NOT_FOUND;

    sp_account_rec *rec = data;
    pj_pool_t *pool = pjsua_pool_create("setpw", 1024, 1024);
    if (!pool)
        return SP_ERR_INTERNAL;

    pjsua_acc_config cfg;
    pjsua_acc_config_default(&cfg);
    pj_status_t st = pjsua_acc_get_config(native, pool, &cfg);
    if (st != PJ_SUCCESS) {
        pj_pool_release(pool);
        return sp_core_fail(core, st, SP_ERR_NOT_FOUND,
                            "pjsua_acc_get_config");
    }

    if (password && *password) {
        /* The account's auth user, or its user.
         *
         * NOT rec->aor: that is "user@domain", and a credential filed
         * under it is answered 401 by every registrar for ever while
         * looking like a working phone from every other angle. The
         * account added without a password -- which is the whole
         * point of this entry point -- has no existing credential to
         * copy a username from, so it comes from the configuration
         * the account was added with, and from the AOR's user part
         * only for a transient account that is in no configuration. */
        char user[192] = {0};
        const sp_account_cfg *stored =
            rec && rec->id ? sp_config_find_account(core->cfg, rec->id)
                           : NULL;
        if (stored && stored->auth_user && *stored->auth_user)
            snprintf(user, sizeof user, "%s", stored->auth_user);
        else if (stored && stored->user && *stored->user)
            snprintf(user, sizeof user, "%s", stored->user);
        else if (cfg.cred_count > 0 && cfg.cred_info[0].username.slen > 0)
            snprintf(user, sizeof user, "%.*s",
                     (int)cfg.cred_info[0].username.slen,
                     cfg.cred_info[0].username.ptr);
        else if (rec && rec->aor) {
            const char *at = strchr(rec->aor, '@');
            snprintf(user, sizeof user, "%.*s",
                     at ? (int)(at - rec->aor) : (int)strlen(rec->aor),
                     rec->aor);
        }

        const pj_str_t who = pj_strdup3(pool, user);
        cfg.cred_count = 1;
        cfg.cred_info[0].realm = pj_str("*");
        cfg.cred_info[0].scheme = pj_str("digest");
        cfg.cred_info[0].username = who;
        cfg.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
        cfg.cred_info[0].data = pj_strdup3(pool, password);
    } else {
        cfg.cred_count = 0;
    }

    st = pjsua_acc_modify(native, &cfg);
    pj_pool_release(pool);
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_CONFIG, "pjsua_acc_modify");
    return SP_OK;
}

sp_status sp_account_auth_user(sp_core *core, sp_account_id acc,
                               char *out, size_t outlen) {
    SP_ENTRY(core);
    if (!out || outlen == 0)
        return SP_ERR_INVALID_ARG;
    *out = '\0';

    void *data = NULL;
    int native = PJSUA_INVALID_ID;
    if (sp_ht_lookup(core->accounts, acc, &native, &data) != 0)
        return SP_ERR_NOT_FOUND;

    pj_pool_t *pool = pjsua_pool_create("authuser", 512, 512);
    if (!pool)
        return SP_ERR_INTERNAL;
    pjsua_acc_config cfg;
    pjsua_acc_config_default(&cfg);
    pj_status_t st = pjsua_acc_get_config(native, pool, &cfg);
    if (st == PJ_SUCCESS && cfg.cred_count > 0) {
        const pj_str_t *u = &cfg.cred_info[0].username;
        size_t n = (size_t)u->slen < outlen - 1 ? (size_t)u->slen
                                                : outlen - 1;
        memcpy(out, u->ptr, n);
        out[n] = '\0';
    }
    pj_pool_release(pool);
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_NOT_FOUND,
                            "pjsua_acc_get_config");
    return SP_OK;
}
