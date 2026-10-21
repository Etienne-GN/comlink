#include "config.h"
#include "modules.h"
#include "theme.h"
#include "yaml_tree.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const TOP_KEYS[] = {
    "log_level", "sip_port", "audio_device", "codec_priority",
    "codecs", "modules",
    "audio_capture_device", "audio_playback_device",
    "accounts", "header_rules",
    "tls_cert_file", "tls_key_file", "tls_ca_file",
    "db_file", "sip_log", "sip_log_redact", "sip_log_max",
    "metrics_interval_ms", "silence_suppression",
    "echo_cancel", "echo_tail_ms", "noise_suppression", "auto_gain",
    "comfort_noise", "packet_loss_concealment",
    "ringtone", "ringback", "notification_sound",
    "tx_volume", "rx_volume",
    "window_width", "window_height", "show_activity_log",
    "show_call_log", "auto_record",
    "theme", "header_presets", "header_names",
    "recording", "sip_trace", "transcript", NULL
};

static const char *const ACCOUNT_KEYS[] = {
    "id", "user", "domain", "auth_user", "password", "registrar",
    "outbound_proxy", "transport", "reg_expires_s", "stun_server",
    "srtp", "register", NULL
};

bool sp_config_module_enabled(const sp_config *cfg, const char *id) {
    const int i = sp_module_index_of(id);
    if (i < 0 || i >= (int)(sizeof cfg->modules / sizeof cfg->modules[0]))
        return true;
    return cfg->modules[i];
}

static const char *const CODEC_KEYS[] = {
    "id", "enabled", "bitrate", "complexity", "expected_loss_pct",
    "ptime_ms", "cbr", NULL
};

static const char *const PRESET_KEYS[] = { "name", "headers", NULL };
static const char *const HEADER_KEYS[] = { "name", "value", NULL };

static const char *const THEME_KEYS[] = {
    "preset", "accent", "font", "text_scale", NULL
};

static const char *const RECORDING_KEYS[] = { "dir", "format", NULL };
static const char *const SIP_TRACE_KEYS[] = { "export_dir", NULL };
static const char *const TRANSCRIPT_KEYS[] = { "model", NULL };

static const char *recording_format_name(sp_recording_format f) {
    if (f == SP_REC_WAV_ULAW) return "wav_ulaw";
    if (f == SP_REC_OPUS)     return "opus";
    return "wav";
}

static int recording_format_from_name(const char *s,
                                      sp_recording_format *out) {
    if (!strcmp(s, "wav"))      { *out = SP_REC_WAV;      return 0; }
    if (!strcmp(s, "wav_ulaw")) { *out = SP_REC_WAV_ULAW; return 0; }
    if (!strcmp(s, "opus"))     { *out = SP_REC_OPUS;     return 0; }
    return -1;
}

static int known(const char *const *set, const char *key) {
    for (size_t i = 0; set[i]; i++)
        if (strcmp(set[i], key) == 0)
            return 1;
    return 0;
}

static sp_status reject_unknown(const sp_yaml_node *map,
                               const char *const *set, const char *what,
                               char *err, size_t errlen) {
    for (size_t i = 0; i < sp_yaml_len(map); i++) {
        const char *key = sp_yaml_key_at(map, i);
        if (!known(set, key)) {
            snprintf(err, errlen, "line %d: unknown %s key \"%s\"",
                     sp_yaml_line(map), what, key);
            return SP_ERR_CONFIG;
        }
    }
    return SP_OK;
}

/* The top level's counterpart to reject_unknown: names every key this
   build does not know and carries on. Returns how many it found, which
   is what the tests assert on -- stderr is not something a test should
   have to parse. */
size_t sp_config_warn_unknown_top(const sp_yaml_node *map) {
    size_t n = 0;
    for (size_t i = 0; i < sp_yaml_len(map); i++) {
        const char *key = sp_yaml_key_at(map, i);
        if (known(TOP_KEYS, key))
            continue;
        n++;
        fprintf(stderr,
                "comlink: config line %d: ignoring unknown top-level key "
                "\"%s\" -- a newer Comlink, or a typo. That setting is "
                "not in effect.\n",
                sp_yaml_line(sp_yaml_get(map, key)), key);
    }
    return n;
}

static void warn_unknown_top_level(const sp_yaml_node *map) {
    (void)sp_config_warn_unknown_top(map);
}

static const char *str_of(const sp_yaml_node *map, const char *key) {
    return sp_yaml_scalar(sp_yaml_get(map, key));
}

static sp_status int_of(const sp_yaml_node *map, const char *key, int dflt,
                        int lo, int hi, int *out, char *err, size_t errlen) {
    const char *s = str_of(map, key);
    if (!s) {
        *out = dflt;
        return SP_OK;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || (end && *end != '\0') || v < lo || v > hi) {
        snprintf(err, errlen, "line %d: %s must be an integer %d..%d, got \"%s\"",
                 sp_yaml_line(sp_yaml_get(map, key)), key, lo, hi, s);
        return SP_ERR_CONFIG;
    }
    *out = (int)v;
    return SP_OK;
}

static sp_status bool_of(const sp_yaml_node *map, const char *key, bool dflt,
                         bool *out, char *err, size_t errlen) {
    const char *s = str_of(map, key);
    if (!s) {
        *out = dflt;
        return SP_OK;
    }
    if (!strcmp(s, "true") || !strcmp(s, "yes") || !strcmp(s, "1")) {
        *out = true;
        return SP_OK;
    }
    if (!strcmp(s, "false") || !strcmp(s, "no") || !strcmp(s, "0")) {
        *out = false;
        return SP_OK;
    }
    snprintf(err, errlen, "line %d: %s must be true or false, got \"%s\"",
             sp_yaml_line(sp_yaml_get(map, key)), key, s);
    return SP_ERR_CONFIG;
}

/* strdup that tolerates NULL, so optional keys stay NULL. */
static char *dup_or_null(const char *s) {
    return s ? strdup(s) : NULL;
}

static sp_status absolute_dir(const sp_yaml_node *map, const char *key,
                              const char *what, char **out,
                              char *err, size_t errlen) {
    const char *v = sp_yaml_scalar(sp_yaml_get(map, key));
    if (!v || !*v) {
        *out = NULL;
        return SP_OK;
    }
    if (*v != '/') {
        snprintf(err, errlen,
                 "line %d: %s must be an absolute path, got \"%s\"",
                 sp_yaml_line(sp_yaml_get(map, key)), what, v);
        return SP_ERR_CONFIG;
    }
    *out = strdup(v);
    return *out ? SP_OK : SP_ERR_INTERNAL;
}

static sp_status parse_transport(const sp_yaml_node *acc, sp_transport *out,
                                 char *err, size_t errlen) {
    const char *s = str_of(acc, "transport");
    if (!s) {
        *out = SP_TP_UDP;
        return SP_OK;
    }
    if (!strcmp(s, "udp")) { *out = SP_TP_UDP; return SP_OK; }
    if (!strcmp(s, "tcp")) { *out = SP_TP_TCP; return SP_OK; }
    if (!strcmp(s, "tls")) { *out = SP_TP_TLS; return SP_OK; }
    snprintf(err, errlen,
             "line %d: transport must be udp, tcp or tls, got \"%s\"",
             sp_yaml_line(sp_yaml_get(acc, "transport")), s);
    return SP_ERR_CONFIG;
}

static sp_status parse_account(const sp_yaml_node *acc, sp_account_cfg *out,
                               char *err, size_t errlen) {
    sp_status st = reject_unknown(acc, ACCOUNT_KEYS, "account", err, errlen);
    if (st != SP_OK)
        return st;

    const char *id = str_of(acc, "id");
    const char *user = str_of(acc, "user");
    const char *domain = str_of(acc, "domain");
    if (!id || !*id) {
        snprintf(err, errlen, "line %d: account is missing required key \"id\"",
                 sp_yaml_line(acc));
        return SP_ERR_CONFIG;
    }
    if (!user || !*user) {
        snprintf(err, errlen,
                 "line %d: account \"%s\" is missing required key \"user\"",
                 sp_yaml_line(acc), id);
        return SP_ERR_CONFIG;
    }
    if (!domain || !*domain) {
        snprintf(err, errlen,
                 "line %d: account \"%s\" is missing required key \"domain\"",
                 sp_yaml_line(acc), id);
        return SP_ERR_CONFIG;
    }

    if ((st = parse_transport(acc, &out->transport, err, errlen)) != SP_OK)
        return st;
    if ((st = int_of(acc, "reg_expires_s", SP_REG_EXPIRES_DEFAULT,
                     SP_REG_EXPIRES_MIN, SP_REG_EXPIRES_MAX,
                     &out->reg_expires_s, err, errlen)) != SP_OK)
        return st;
    if ((st = bool_of(acc, "register", true, &out->do_register,
                      err, errlen)) != SP_OK)
        return st;

    const char *srtp = str_of(acc, "srtp");
    if (!srtp || !strcmp(srtp, "disabled"))
        out->srtp = SP_SRTP_DISABLED;
    else if (!strcmp(srtp, "optional"))
        out->srtp = SP_SRTP_OPTIONAL;
    else if (!strcmp(srtp, "mandatory"))
        out->srtp = SP_SRTP_MANDATORY;
    else {
        snprintf(err, errlen,
                 "line %d: account \"%s\": srtp must be disabled, optional "
                 "or mandatory, got \"%s\"",
                 sp_yaml_line(sp_yaml_get(acc, "srtp")), id, srtp);
        return SP_ERR_CONFIG;
    }

    const char *auth_user = str_of(acc, "auth_user");
    out->id = dup_or_null(id);
    out->user = dup_or_null(user);
    out->domain = dup_or_null(domain);
    out->auth_user = dup_or_null(auth_user ? auth_user : user);
    out->password = dup_or_null(str_of(acc, "password"));
    out->registrar = dup_or_null(str_of(acc, "registrar"));
    out->outbound_proxy = dup_or_null(str_of(acc, "outbound_proxy"));
    out->stun_server = dup_or_null(str_of(acc, "stun_server"));
    return SP_OK;
}

static sp_status parse_str_list(const sp_yaml_node *seq, char ***out,
                                size_t *n_out) {
    size_t n = sp_yaml_len(seq);
    *out = NULL;
    *n_out = 0;
    if (n == 0)
        return SP_OK;
    char **v = calloc(n, sizeof *v);
    if (!v)
        return SP_ERR_INTERNAL;
    for (size_t i = 0; i < n; i++) {
        const char *s = sp_yaml_scalar(sp_yaml_at(seq, i));
        v[i] = dup_or_null(s ? s : "");
    }
    *out = v;
    *n_out = n;
    return SP_OK;
}

static void account_fields_free(sp_account_cfg *a) {
    /* Fields are const in the public ABI but owned here. */
    free((void *)a->id);
    free((void *)a->user);
    free((void *)a->domain);
    free((void *)a->auth_user);
    free((void *)a->password);
    free((void *)a->registrar);
    free((void *)a->outbound_proxy);
    free((void *)a->stun_server);
    memset(a, 0, sizeof *a);
}

sp_status sp_config_add_account(sp_config *cfg, const sp_account_cfg *in) {
    if (!cfg || !in || !in->id)
        return SP_ERR_INVALID_ARG;
    sp_account_cfg *grown =
        realloc(cfg->accounts, (cfg->n_accounts + 1) * sizeof *grown);
    if (!grown)
        return SP_ERR_INTERNAL;
    cfg->accounts = grown;

    sp_account_cfg *a = &cfg->accounts[cfg->n_accounts];
    memset(a, 0, sizeof *a);
    a->transport = in->transport;
    a->reg_expires_s = in->reg_expires_s;
    a->do_register = in->do_register;
    a->srtp = in->srtp;
    a->id = dup_or_null(in->id);
    a->user = dup_or_null(in->user);
    a->domain = dup_or_null(in->domain);
    a->auth_user = dup_or_null(in->auth_user ? in->auth_user : in->user);
    a->password = dup_or_null(in->password);
    a->registrar = dup_or_null(in->registrar);
    a->outbound_proxy = dup_or_null(in->outbound_proxy);
    a->stun_server = dup_or_null(in->stun_server);
    if (!a->id || !a->user || !a->domain) {
        account_fields_free(a);
        return SP_ERR_INTERNAL;
    }
    cfg->n_accounts++;
    return SP_OK;
}

int sp_config_remove_account(sp_config *cfg, const char *id) {
    if (!cfg || !id)
        return -1;
    for (size_t i = 0; i < cfg->n_accounts; i++) {
        if (strcmp(cfg->accounts[i].id, id) != 0)
            continue;
        account_fields_free(&cfg->accounts[i]);
        /* Order matters: it is the order accounts appear in the file
           and the order handles are issued at startup. */
        memmove(&cfg->accounts[i], &cfg->accounts[i + 1],
                (cfg->n_accounts - i - 1) * sizeof cfg->accounts[0]);
        cfg->n_accounts--;
        return 0;
    }
    return -1;
}

const sp_account_cfg *sp_config_find_account(const sp_config *cfg,
                                             const char *id) {
    if (!cfg || !id)
        return NULL;
    for (size_t i = 0; i < cfg->n_accounts; i++)
        if (strcmp(cfg->accounts[i].id, id) == 0)
            return &cfg->accounts[i];
    return NULL;
}

void sp_config_presets_free(sp_header_preset *presets, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free((void *)presets[i].name);
        for (size_t j = 0; j < presets[i].n_headers; j++) {
            free((void *)presets[i].headers[j].name);
            free((void *)presets[i].headers[j].value);
        }
        free((void *)presets[i].headers);
    }
    free(presets);
}

/* Priorities descend from just under pjsua's maximum so the first
   entry outranks anything the stack defaulted, and the list can be
   long without the last entry colliding with disabled (zero). */
#define SP_CODEC_PRIO_TOP 254

static int codec_prio_for(size_t index, size_t total) {
    (void)total;
    int p = SP_CODEC_PRIO_TOP - (int)index;
    return p < 1 ? 1 : p;
}

static void codecs_free(sp_codec_cfg *v, size_t n) {
    for (size_t i = 0; i < n; i++)
        free(v[i].id);
    free(v);
}

static void codec_defaults(sp_codec_cfg *c) {
    c->enabled = true;
    c->bitrate = -1;
    c->complexity = -1;
    c->expected_loss_pct = -1;
    c->ptime_ms = -1;
    c->cbr = -1;
}

/* Reads an optional integer, leaving it at -1 when absent. Out of
   range names the key and the value: a parameter the operator asked
   for that cannot be honoured is a mistake worth reporting, not one
   to round into the nearest legal thing. */
static sp_status codec_int(sp_yaml_node *node, const char *key, int lo,
                           int hi, int *out, char *err, size_t errlen) {
    const char *raw = sp_yaml_scalar(sp_yaml_get(node, key));
    if (!raw || !*raw)
        return SP_OK;
    char *end = NULL;
    long v = strtol(raw, &end, 10);
    if (end == raw || (end && *end) || v < lo || v > hi) {
        snprintf(err, errlen, "line %d: %s must be %d..%d, got \"%s\"",
                 sp_yaml_line(node), key, lo, hi, raw);
        return SP_ERR_CONFIG;
    }
    *out = (int)v;
    return SP_OK;
}

static sp_status parse_codecs(sp_yaml_node *seq, sp_codec_cfg **out,
                              size_t *n_out, char *err, size_t errlen) {
    *out = NULL;
    *n_out = 0;
    size_t n = sp_yaml_len(seq);
    if (n == 0)
        return SP_OK;

    sp_codec_cfg *v = calloc(n, sizeof *v);
    if (!v)
        return SP_ERR_INTERNAL;

    for (size_t i = 0; i < n; i++) {
        sp_yaml_node *node = sp_yaml_at(seq, i);
        codec_defaults(&v[i]);

        sp_status st = reject_unknown(node, CODEC_KEYS, "codec", err, errlen);
        if (st != SP_OK) { codecs_free(v, i); return st; }

        const char *id = sp_yaml_scalar(sp_yaml_get(node, "id"));
        if (!id || !*id) {
            snprintf(err, errlen, "line %d: a codec needs an id",
                     sp_yaml_line(node));
            codecs_free(v, i);
            return SP_ERR_CONFIG;
        }
        for (size_t j = 0; j < i; j++)
            if (strcasecmp(v[j].id, id) == 0) {
                snprintf(err, errlen,
                         "line %d: codec \"%s\" is listed twice",
                         sp_yaml_line(node), id);
                codecs_free(v, i);
                return SP_ERR_CONFIG;
            }
        v[i].id = dup_or_null(id);
        if (!v[i].id) { codecs_free(v, i); return SP_ERR_INTERNAL; }

        const char *en = sp_yaml_scalar(sp_yaml_get(node, "enabled"));
        if (en && *en)
            v[i].enabled = (strcasecmp(en, "true") == 0
                            || strcmp(en, "1") == 0);

        struct { const char *key; int lo, hi; int *dst; } ints[] = {
            { "bitrate",           1000, 512000, &v[i].bitrate },
            { "complexity",           0,     10, &v[i].complexity },
            { "expected_loss_pct",    0,    100, &v[i].expected_loss_pct },
            { "ptime_ms",            10,    200, &v[i].ptime_ms },
        };
        for (size_t k = 0; k < sizeof ints / sizeof ints[0]; k++) {
            st = codec_int(node, ints[k].key, ints[k].lo, ints[k].hi,
                           ints[k].dst, err, errlen);
            if (st != SP_OK) { codecs_free(v, i + 1); return st; }
        }

        const char *cbr = sp_yaml_scalar(sp_yaml_get(node, "cbr"));
        if (cbr && *cbr)
            v[i].cbr = (strcasecmp(cbr, "true") == 0
                        || strcmp(cbr, "1") == 0) ? 1 : 0;

        v[i].priority = v[i].enabled ? codec_prio_for(i, n) : 0;
    }
    *out = v;
    *n_out = n;
    return SP_OK;
}

static sp_status parse_presets(sp_yaml_node *seq, sp_header_preset **out,
                               size_t *n_out, char *err, size_t errlen) {
    *out = NULL;
    *n_out = 0;
    size_t n = sp_yaml_len(seq);
    if (n == 0)
        return SP_OK;

    sp_header_preset *v = calloc(n, sizeof *v);
    if (!v)
        return SP_ERR_INTERNAL;

    for (size_t i = 0; i < n; i++) {
        sp_yaml_node *node = sp_yaml_at(seq, i);
        sp_status st = reject_unknown(node, PRESET_KEYS, "header preset",
                                      err, errlen);
        if (st != SP_OK) {
            sp_config_presets_free(v, i);
            return st;
        }
        const char *name = sp_yaml_scalar(sp_yaml_get(node, "name"));
        if (!name || !*name) {
            snprintf(err, errlen, "line %d: header preset needs a name",
                     sp_yaml_line(node));
            sp_config_presets_free(v, i);
            return SP_ERR_CONFIG;
        }
        v[i].name = dup_or_null(name);

        sp_yaml_node *hs = sp_yaml_get(node, "headers");
        size_t hn = sp_yaml_len(hs);
        if (hn == 0) {
            snprintf(err, errlen,
                     "line %d: header preset \"%s\" has no headers",
                     sp_yaml_line(node), name);
            sp_config_presets_free(v, i + 1);
            return SP_ERR_CONFIG;
        }
        sp_header *hv = calloc(hn, sizeof *hv);
        if (!hv) {
            sp_config_presets_free(v, i + 1);
            return SP_ERR_INTERNAL;
        }
        v[i].headers = hv;
        for (size_t j = 0; j < hn; j++) {
            sp_yaml_node *h = sp_yaml_at(hs, j);
            sp_status hst = reject_unknown(h, HEADER_KEYS, "preset header",
                                           err, errlen);
            if (hst != SP_OK) {
                v[i].n_headers = j;
                sp_config_presets_free(v, i + 1);
                return hst;
            }
            const char *hname = sp_yaml_scalar(sp_yaml_get(h, "name"));
            const char *hval = sp_yaml_scalar(sp_yaml_get(h, "value"));
            if (!hname || !*hname || !hval) {
                snprintf(err, errlen,
                         "line %d: preset \"%s\" header needs name and value",
                         sp_yaml_line(h), name);
                v[i].n_headers = j;
                sp_config_presets_free(v, i + 1);
                return SP_ERR_CONFIG;
            }
            hv[j].name = dup_or_null(hname);
            hv[j].value = dup_or_null(hval);
            v[i].n_headers = j + 1;
        }
    }
    *out = v;
    *n_out = n;
    return SP_OK;
}

void sp_config_free(sp_config *cfg) {
    if (!cfg)
        return;
    for (size_t i = 0; i < cfg->n_accounts; i++)
        account_fields_free(&cfg->accounts[i]);
    free(cfg->accounts);
    sp_rules_free(cfg->rules, cfg->n_rules);
    codecs_free(cfg->codecs, cfg->n_codecs);
    free(cfg->audio_device);
    free(cfg->ringtone);
    free(cfg->ringback);
    free(cfg->notification_sound);
    free(cfg->audio_capture_device);
    free(cfg->audio_playback_device);
    free(cfg->tls_cert_file);
    free(cfg->tls_key_file);
    free(cfg->tls_ca_file);
    free(cfg->db_file);
    free(cfg->theme_preset_unknown);
    free(cfg->theme_accent);
    free(cfg->theme_font);
    free(cfg->recording_dir);
    free(cfg->sip_trace_export_dir);
    free(cfg->transcript_model);
    sp_config_presets_free(cfg->presets, cfg->n_presets);
    for (size_t i = 0; i < cfg->n_header_names; i++)
        free(cfg->header_names[i]);
    free(cfg->header_names);
    free(cfg);
}

static bool is_none(const char *s) {
    return s && strcmp(s, "none") == 0;
}

bool sp_config_wants_no_audio(const sp_config *cfg) {
    if (!cfg)
        return false;
    /* Any of the three: a per-direction "none" is as much a request
       for no sound card as the global one. */
    return is_none(cfg->audio_device) ||
           is_none(cfg->audio_capture_device) ||
           is_none(cfg->audio_playback_device);
}

sp_status sp_config_load(const char *path, sp_config **out,
                         char *err, size_t errlen) {
    if (!out || !path) {
        snprintf(err, errlen, "invalid argument");
        return SP_ERR_INVALID_ARG;
    }
    *out = NULL;

    sp_yaml_node *root = sp_yaml_parse_file(path, err, errlen);
    if (!root)
        return SP_ERR_CONFIG;

    if (sp_yaml_kind_of(root) != SP_YAML_MAP) {
        snprintf(err, errlen, "top level of %s must be a mapping", path);
        sp_yaml_free(root);
        return SP_ERR_CONFIG;
    }

    /* Top-level keys warn rather than refuse, and only here: inside a
       known mapping an unknown key is still fatal.
     *
     * The reason is rollback. A newer Comlink writes keys an older one
     * has never heard of, so refusing them means downgrading the
     * package gives you a phone that will not start -- discovered at
     * the worst possible moment, since rolling back is the safety net
     * rather than the risk. A 0.7.0 build once wrote
     * "recording:" and the installed 0.6.0 refused to boot on it.
     *
     * The cost is accepted deliberately: a misspelt top-level key now
     * starts the phone with that setting silently absent, and the
     * warning below is the only notice. It is loud, it names the key,
     * and it repeats on every start. A typo inside header_rules --
     * the failure this strictness was written for -- is unaffected,
     * because those live inside mappings. */
    warn_unknown_top_level(root);
    sp_status st = SP_OK;

    sp_config *cfg = calloc(1, sizeof *cfg);
    if (!cfg) {
        sp_yaml_free(root);
        snprintf(err, errlen, "out of memory");
        return SP_ERR_INTERNAL;
    }

    if ((st = int_of(root, "log_level", 3, 0, 5, &cfg->log_level,
                     err, errlen)) != SP_OK)
        goto fail;
    if ((st = int_of(root, "sip_port", 0, 0, 65535, &cfg->sip_port,
                     err, errlen)) != SP_OK)
        goto fail;

    cfg->audio_device = dup_or_null(str_of(root, "audio_device"));
    cfg->audio_capture_device =
        dup_or_null(str_of(root, "audio_capture_device"));
    cfg->audio_playback_device =
        dup_or_null(str_of(root, "audio_playback_device"));
    cfg->tls_cert_file = dup_or_null(str_of(root, "tls_cert_file"));
    cfg->tls_key_file = dup_or_null(str_of(root, "tls_key_file"));
    cfg->tls_ca_file = dup_or_null(str_of(root, "tls_ca_file"));
    cfg->db_file = dup_or_null(str_of(root, "db_file"));
    cfg->ringtone = dup_or_null(str_of(root, "ringtone"));
    cfg->ringback = dup_or_null(str_of(root, "ringback"));
    cfg->notification_sound =
        dup_or_null(str_of(root, "notification_sound"));

    /* --- modules ----------------------------------------------------
       Each at its default unless the config says otherwise: a file
       written before modules existed described a phone with all of
       what it had then, and has to keep meaning that. */
    for (int mi = 0; mi < sp_module_count(); mi++)
        cfg->modules[mi] = sp_module_default_at(mi);

    sp_yaml_node *mods = sp_yaml_get(root, "modules");
    sp_yaml_node *sip_log_node = sp_yaml_get(root, "sip_log");
    sp_yaml_node *metrics_node = sp_yaml_get(root, "metrics_interval_ms");

    /* sip_log and metrics_interval_ms: 0 are the old spellings of two
       of these. One or the other, never both -- the same rule the
       codecs list keeps, for the same reason. */
    if (mods && sip_log_node) {
        snprintf(err, errlen,
                 "line %d: sip_log and modules.sip_trace both set; "
                 "modules replaces sip_log, so keep one",
                 sp_yaml_line(mods));
        st = SP_ERR_CONFIG;
        goto fail;
    }

    if (mods) {
        const size_t n_keys = sp_yaml_len(mods);
        for (size_t k = 0; k < n_keys; k++) {
            const char *key = sp_yaml_key_at(mods, k);
            const int mi = sp_module_index_of(key);
            if (mi < 0) {
                snprintf(err, errlen, "line %d: unknown module \"%s\"",
                         sp_yaml_line(mods), key ? key : "");
                st = SP_ERR_CONFIG;
                goto fail;
            }
            const char *raw = sp_yaml_scalar(sp_yaml_get(mods, key));
            const bool on = !raw || !*raw
                            || strcasecmp(raw, "true") == 0
                            || strcmp(raw, "1") == 0;
            if (!on && !sp_module_removable_at(mi)) {
                snprintf(err, errlen,
                         "line %d: module \"%s\" cannot be switched off",
                         sp_yaml_line(mods), key);
                st = SP_ERR_CONFIG;
                goto fail;
            }
            cfg->modules[mi] = on;
        }
    }

    if ((st = bool_of(root, "sip_log", true, &cfg->sip_log,
                      err, errlen)) != SP_OK)
        goto fail;
    if (sip_log_node)
        cfg->modules[sp_module_index_of(SP_MODULE_SIP_TRACE)] = cfg->sip_log;
    else
        cfg->sip_log = sp_config_module_enabled(cfg, SP_MODULE_SIP_TRACE);
    if ((st = bool_of(root, "sip_log_redact", true, &cfg->sip_log_redact,
                      err, errlen)) != SP_OK)
        goto fail;
    if ((st = int_of(root, "sip_log_max", 5000, 0, 1000000,
                     &cfg->sip_log_max, err, errlen)) != SP_OK)
        goto fail;
    if ((st = int_of(root, "metrics_interval_ms", 2000, 0, 1000000,
                     &cfg->metrics_interval_ms, err, errlen)) != SP_OK)
        goto fail;
    /* Zero used to mean "off". That is the audio_quality module's job
       now, so a zero migrates into it and the interval falls back to
       its default: the key keeps only its real meaning, the rate. */
    if (metrics_node && cfg->metrics_interval_ms == 0) {
        cfg->modules[sp_module_index_of(SP_MODULE_AUDIO_QUALITY)] = false;
        cfg->metrics_interval_ms = 2000;
    }
    if (cfg->metrics_interval_ms != 0 &&
        (cfg->metrics_interval_ms < 100 ||
         cfg->metrics_interval_ms > 60000)) {
        snprintf(err, errlen,
                 "line %d: metrics_interval_ms must be 0 to disable "
                 "sampling, or 100..60000, got %d",
                 sp_yaml_line(sp_yaml_get(root, "metrics_interval_ms")),
                 cfg->metrics_interval_ms);
        st = SP_ERR_CONFIG;
        goto fail;
    }
    if ((st = bool_of(root, "silence_suppression", false,
                      &cfg->silence_suppression, err, errlen)) != SP_OK)
        goto fail;
    if ((st = int_of(root, "tx_volume", 100, 0, 200,
                     &cfg->tx_volume_pct, err, errlen)) != SP_OK)
        goto fail;
    if ((st = int_of(root, "rx_volume", 100, 0, 200,
                     &cfg->rx_volume_pct, err, errlen)) != SP_OK)
        goto fail;
    if ((st = bool_of(root, "echo_cancel", true,
                      &cfg->echo_cancel, err, errlen)) != SP_OK)
        goto fail;
    if ((st = int_of(root, "echo_tail_ms", 200, 0, 500,
                     &cfg->echo_tail_ms, err, errlen)) != SP_OK)
        goto fail;
    if ((st = bool_of(root, "noise_suppression", false,
                      &cfg->noise_suppression, err, errlen)) != SP_OK)
        goto fail;
    if ((st = bool_of(root, "auto_gain", false,
                      &cfg->auto_gain, err, errlen)) != SP_OK)
        goto fail;
    if ((st = bool_of(root, "comfort_noise", true,
                      &cfg->comfort_noise, err, errlen)) != SP_OK)
        goto fail;
    if ((st = bool_of(root, "packet_loss_concealment", true,
                      &cfg->packet_loss_concealment, err, errlen)) != SP_OK)
        goto fail;
    /* 0 means unset. The upper bound is generous rather than tuned: it
       exists to reject a corrupted file, not to have an opinion about
       how large a window may be. */
    if ((st = int_of(root, "window_width", 0, 0, 32767,
                     &cfg->window_width, err, errlen)) != SP_OK)
        goto fail;
    if ((st = int_of(root, "window_height", 0, 0, 32767,
                     &cfg->window_height, err, errlen)) != SP_OK)
        goto fail;
    if ((st = bool_of(root, "show_activity_log", true,
                      &cfg->show_activity_log, err, errlen)) != SP_OK)
        goto fail;
    if ((st = bool_of(root, "show_call_log", false,
                      &cfg->show_call_log, err, errlen)) != SP_OK)
        goto fail;
    if ((st = bool_of(root, "auto_record", false,
                      &cfg->auto_record, err, errlen)) != SP_OK)
        goto fail;

    sp_yaml_node *codecs_node = sp_yaml_get(root, "codecs");
    sp_yaml_node *prio_node = sp_yaml_get(root, "codec_priority");
    if (codecs_node && prio_node) {
        snprintf(err, errlen,
                 "line %d: codec_priority and codecs both set; codecs "
                 "replaces codec_priority, so keep one",
                 sp_yaml_line(codecs_node));
        st = SP_ERR_CONFIG;
        goto fail;
    }
    if (codecs_node) {
        if ((st = parse_codecs(codecs_node, &cfg->codecs, &cfg->n_codecs,
                               err, errlen)) != SP_OK)
            goto fail;
    } else if (prio_node) {
        /* The old key, read into the new shape. Nothing else in the
           core knows codec_priority existed, and saving writes codecs,
           so the file migrates itself the first time it is written. */
        char **names = NULL;
        size_t n_names = 0;
        if ((st = parse_str_list(prio_node, &names, &n_names)) != SP_OK) {
            snprintf(err, errlen, "out of memory reading codec_priority");
            goto fail;
        }
        if (n_names > 0) {
            cfg->codecs = calloc(n_names, sizeof *cfg->codecs);
            if (!cfg->codecs) {
                for (size_t i = 0; i < n_names; i++)
                    free(names[i]);
                free(names);
                st = SP_ERR_INTERNAL;
                goto fail;
            }
            for (size_t i = 0; i < n_names; i++) {
                codec_defaults(&cfg->codecs[i]);
                cfg->codecs[i].id = names[i];   /* ownership moves */
                cfg->codecs[i].priority = codec_prio_for(i, n_names);
            }
            cfg->n_codecs = n_names;
        }
        free(names);
    }

    /* Theme defaults, then the mapping if present. */
    cfg->theme_preset = SP_THEME_DARK;
    cfg->theme_scale = SP_TEXT_MEDIUM;
    sp_yaml_node *th = sp_yaml_get(root, "theme");
    if (th) {
        if ((st = reject_unknown(th, THEME_KEYS, "theme", err, errlen))
                != SP_OK)
            goto fail;
        const char *pname = sp_yaml_scalar(sp_yaml_get(th, "preset"));
        /* An unknown preset is a look, not a fault: it is what a newer
           Comlink wrote, and refusing to start over it left a phone
           that could not be opened after going back a version. The
           window is drawn in the default and the name is kept. */
        if (pname && sp_theme_preset_from_name(pname, &cfg->theme_preset)
                         != 0) {
            fprintf(stderr,
                    "comlink: config line %d: unknown theme preset \"%s\" "
                    "-- a newer Comlink, or a typo. Drawing in the default "
                    "theme; the setting is kept as it is.\n",
                    sp_yaml_line(sp_yaml_get(th, "preset")), pname);
            cfg->theme_preset = SP_THEME_DARK;
            cfg->theme_preset_unknown = dup_or_null(pname);
        }
        const char *sname = sp_yaml_scalar(sp_yaml_get(th, "text_scale"));
        if (sname && sp_text_scale_from_name(sname, &cfg->theme_scale) != 0) {
            snprintf(err, errlen,
                     "line %d: theme text_scale must be small, medium, large "
                     "or extra_large, got \"%s\"",
                     sp_yaml_line(sp_yaml_get(th, "text_scale")), sname);
            st = SP_ERR_CONFIG;
            goto fail;
        }
        const char *accent = sp_yaml_scalar(sp_yaml_get(th, "accent"));
        if (accent && *accent && sp_theme_validate_hex(accent) != 0) {
            snprintf(err, errlen,
                     "line %d: theme accent must be #RRGGBB, got \"%s\"",
                     sp_yaml_line(sp_yaml_get(th, "accent")), accent);
            st = SP_ERR_CONFIG;
            goto fail;
        }
        cfg->theme_accent = dup_or_null(accent);
        cfg->theme_font = dup_or_null(sp_yaml_scalar(sp_yaml_get(th, "font")));
    }

    /* Recording defaults, then the mapping if present. */
    cfg->recording_format = SP_REC_WAV;
    sp_yaml_node *recn = sp_yaml_get(root, "recording");
    if (recn) {
        if ((st = reject_unknown(recn, RECORDING_KEYS, "recording",
                                 err, errlen)) != SP_OK)
            goto fail;
        if ((st = absolute_dir(recn, "dir", "recording dir",
                               &cfg->recording_dir, err, errlen)) != SP_OK)
            goto fail;
        const char *fmt = sp_yaml_scalar(sp_yaml_get(recn, "format"));
        if (fmt && recording_format_from_name(fmt,
                                              &cfg->recording_format) != 0) {
            snprintf(err, errlen,
                     "line %d: recording format must be wav, wav_ulaw or "
                     "opus, got \"%s\"",
                     sp_yaml_line(sp_yaml_get(recn, "format")), fmt);
            st = SP_ERR_CONFIG;
            goto fail;
        }
    }

    sp_yaml_node *trn = sp_yaml_get(root, "sip_trace");
    if (trn) {
        if ((st = reject_unknown(trn, SIP_TRACE_KEYS, "sip_trace",
                                 err, errlen)) != SP_OK)
            goto fail;
        if ((st = absolute_dir(trn, "export_dir", "sip_trace export_dir",
                               &cfg->sip_trace_export_dir,
                               err, errlen)) != SP_OK)
            goto fail;
    }

    sp_yaml_node *tsn = sp_yaml_get(root, "transcript");
    if (tsn) {
        if ((st = reject_unknown(tsn, TRANSCRIPT_KEYS, "transcript",
                                 err, errlen)) != SP_OK)
            goto fail;
        if ((st = absolute_dir(tsn, "model", "transcript model",
                               &cfg->transcript_model, err, errlen)) != SP_OK)
            goto fail;
    }

    sp_yaml_node *accs = sp_yaml_get(root, "accounts");
    size_t n_accs = sp_yaml_len(accs);
    if (n_accs > 0) {
        cfg->accounts = calloc(n_accs, sizeof *cfg->accounts);
        if (!cfg->accounts) {
            st = SP_ERR_INTERNAL;
            snprintf(err, errlen, "out of memory");
            goto fail;
        }
        for (size_t i = 0; i < n_accs; i++) {
            st = parse_account(sp_yaml_at(accs, i), &cfg->accounts[i],
                               err, errlen);
            if (st != SP_OK)
                goto fail;
            cfg->n_accounts = i + 1;
            for (size_t j = 0; j < i; j++) {
                if (strcmp(cfg->accounts[j].id, cfg->accounts[i].id) == 0) {
                    snprintf(err, errlen,
                             "duplicate account id \"%s\"", cfg->accounts[i].id);
                    st = SP_ERR_CONFIG;
                    goto fail;
                }
            }
        }
    }

    st = sp_rules_parse(sp_yaml_get(root, "header_rules"), &cfg->rules,
                        &cfg->n_rules, err, errlen);
    if (st != SP_OK)
        goto fail;

    st = parse_presets(sp_yaml_get(root, "header_presets"), &cfg->presets,
                       &cfg->n_presets, err, errlen);
    if (st != SP_OK)
        goto fail;

    if ((st = parse_str_list(sp_yaml_get(root, "header_names"),
                             &cfg->header_names,
                             &cfg->n_header_names)) != SP_OK) {
        snprintf(err, errlen, "out of memory reading header_names");
        goto fail;
    }

    sp_yaml_free(root);
    *out = cfg;
    return SP_OK;

fail:
    sp_yaml_free(root);
    sp_config_free(cfg);
    return st;
}

/* --- writing ---------------------------------------------------------- */

/* Always double-quote and escape. Cheaper than deciding when a value is
   safe bare, and a value like "12:34" or "45.4,-71.9" must not be
   allowed to change meaning on reload. */
static void emit_quoted(FILE *f, const char *s) {
    fputc('"', f);
    for (const char *p = s; p && *p; p++) {
        if (*p == '"' || *p == '\\')
            fputc('\\', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

static void emit_key_str(FILE *f, const char *indent, const char *key,
                         const char *value) {
    if (!value)
        return;
    fprintf(f, "%s%s: ", indent, key);
    emit_quoted(f, value);
    fputc('\n', f);
}

static const char *transport_str(sp_transport t) {
    switch (t) {
    case SP_TP_TCP: return "tcp";
    case SP_TP_TLS: return "tls";
    case SP_TP_UDP: break;
    }
    return "udp";
}

static const char *srtp_str(sp_srtp_mode m) {
    switch (m) {
    case SP_SRTP_OPTIONAL:  return "optional";
    case SP_SRTP_MANDATORY: return "mandatory";
    case SP_SRTP_DISABLED:  break;
    }
    return "disabled";
}

static const char *direction_str(sp_direction d) {
    return d == SP_DIR_RX ? "rx" : "tx";
}

static const char *message_str(sp_msg_kind m) {
    switch (m) {
    case SP_MSG_REQUEST:  return "request";
    case SP_MSG_RESPONSE: return "response";
    case SP_MSG_ANY:      break;
    }
    return "any";
}

static void emit_rules(FILE *f, const sp_config *cfg) {
    if (cfg->n_rules == 0)
        return;
    fputs("\nheader_rules:\n", f);
    for (size_t i = 0; i < cfg->n_rules; i++) {
        const sp_hdr_rule *r = &cfg->rules[i];
        fputs("  - name: ", f);
        emit_quoted(f, r->name);
        fputc('\n', f);
        fprintf(f, "    direction: %s\n", direction_str(r->direction));
        fprintf(f, "    message: %s\n", message_str(r->message));
        if (r->n_methods > 0) {
            fputs("    method: [", f);
            for (size_t j = 0; j < r->n_methods; j++) {
                if (j)
                    fputs(", ", f);
                emit_quoted(f, r->methods[j]);
            }
            fputs("]\n", f);
        }
        emit_key_str(f, "    ", "account", r->account);
        fputs("    actions:\n", f);
        for (size_t j = 0; j < r->n_actions; j++) {
            const sp_hdr_action *a = &r->actions[j];
            if (a->kind == SP_ACT_REMOVE) {
                fputs("      - remove: ", f);
                emit_quoted(f, a->name);
                fputc('\n', f);
            } else {
                fprintf(f, "      - %s: {name: ",
                        a->kind == SP_ACT_SET ? "set" : "add");
                emit_quoted(f, a->name);
                fputs(", value: ", f);
                emit_quoted(f, a->value ? a->value : "");
                fputs("}\n", f);
            }
        }
    }
}

sp_status sp_config_save(const sp_config *cfg, const char *path,
                         char *err, size_t errlen) {
    if (!cfg || !path) {
        snprintf(err, errlen, "invalid argument");
        return SP_ERR_INVALID_ARG;
    }

    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        snprintf(err, errlen, "path too long: %s", path);
        return SP_ERR_CONFIG;
    }

    FILE *f = fopen(tmp, "we");
    if (!f) {
        snprintf(err, errlen, "cannot write %s: %s", tmp, strerror(errno));
        return SP_ERR_CONFIG;
    }

    fputs("# Written by comlink. Hand edits are preserved on reload,\n"
          "# but comments and key order are not.\n\n", f);
    fprintf(f, "log_level: %d\n", cfg->log_level);
    fprintf(f, "sip_port: %d\n", cfg->sip_port);
    emit_key_str(f, "", "audio_device", cfg->audio_device);
    emit_key_str(f, "", "ringtone", cfg->ringtone);
    emit_key_str(f, "", "ringback", cfg->ringback);
    emit_key_str(f, "", "notification_sound", cfg->notification_sound);
    emit_key_str(f, "", "audio_capture_device", cfg->audio_capture_device);
    emit_key_str(f, "", "audio_playback_device", cfg->audio_playback_device);
    emit_key_str(f, "", "tls_cert_file", cfg->tls_cert_file);
    emit_key_str(f, "", "tls_key_file", cfg->tls_key_file);
    emit_key_str(f, "", "tls_ca_file", cfg->tls_ca_file);
    emit_key_str(f, "", "db_file", cfg->db_file);
    /* sip_log is not written: it is modules.sip_trace now, and a file
       carrying both would be refused on the way back in. */
    fprintf(f, "sip_log_redact: %s\n", cfg->sip_log_redact ? "true" : "false");
    fprintf(f, "sip_log_max: %d\n", cfg->sip_log_max);
    fprintf(f, "metrics_interval_ms: %d\n", cfg->metrics_interval_ms);
    fprintf(f, "silence_suppression: %s\n",
            cfg->silence_suppression ? "true" : "false");
    fprintf(f, "tx_volume: %d\n", cfg->tx_volume_pct);
    fprintf(f, "rx_volume: %d\n", cfg->rx_volume_pct);
    fprintf(f, "echo_cancel: %s\n", cfg->echo_cancel ? "true" : "false");
    fprintf(f, "echo_tail_ms: %d\n", cfg->echo_tail_ms);
    fprintf(f, "noise_suppression: %s\n",
            cfg->noise_suppression ? "true" : "false");
    fprintf(f, "auto_gain: %s\n", cfg->auto_gain ? "true" : "false");
    fprintf(f, "comfort_noise: %s\n",
            cfg->comfort_noise ? "true" : "false");
    fprintf(f, "packet_loss_concealment: %s\n",
            cfg->packet_loss_concealment ? "true" : "false");
    if (cfg->window_width > 0 && cfg->window_height > 0) {
        fprintf(f, "window_width: %d\n", cfg->window_width);
        fprintf(f, "window_height: %d\n", cfg->window_height);
    }
    fprintf(f, "show_activity_log: %s\n",
            cfg->show_activity_log ? "true" : "false");
    fprintf(f, "show_call_log: %s\n",
            cfg->show_call_log ? "true" : "false");
    /* Only when on: a key an older Comlink has never seen is a warning
       at every start, and off is what it would do anyway. */
    if (cfg->auto_record)
        fputs("auto_record: true\n", f);

    fputs("\ntheme:\n", f);
    fprintf(f, "  preset: %s\n",
            cfg->theme_preset_unknown ? cfg->theme_preset_unknown
                                      : sp_theme_preset_name(cfg->theme_preset));
    fprintf(f, "  text_scale: %s\n", sp_text_scale_name(cfg->theme_scale));
    emit_key_str(f, "  ", "accent", cfg->theme_accent);
    emit_key_str(f, "  ", "font", cfg->theme_font);

    fputs("\nrecording:\n", f);
    emit_key_str(f, "  ", "dir", cfg->recording_dir);
    fprintf(f, "  format: %s\n",
            recording_format_name(cfg->recording_format));

    /* Only written when set. Not for safety -- a bare "sip_trace:"
       with nothing under it parses back fine either way, since
       sp_yaml_len() returns 0 for a null scalar and reject_unknown
       walks nothing (yaml_tree.c:263-271, :279-286). It is skipped
       because it is noise: a section with nothing under it reads
       like one an operator left half-finished by hand. */
    if (cfg->sip_trace_export_dir) {
        fputs("\nsip_trace:\n", f);
        emit_key_str(f, "  ", "export_dir", cfg->sip_trace_export_dir);
    }

    if (cfg->transcript_model) {
        fputs("\ntranscript:\n", f);
        emit_key_str(f, "  ", "model", cfg->transcript_model);
    }

    /* Only what differs from its default: the list stays short, and
       an operator scanning the file sees what they changed rather
       than a wall of true. Absent means default, which is also what
       an old file means. */
    {
        bool any = false;
        for (int mi = 0; mi < sp_module_count(); mi++)
            if (cfg->modules[mi] != sp_module_default_at(mi))
                any = true;
        if (any) {
            fputs("\nmodules:\n", f);
            for (int mi = 0; mi < sp_module_count(); mi++)
                if (cfg->modules[mi] != sp_module_default_at(mi))
                    fprintf(f, "  %s: %s\n", sp_module_id_at(mi),
                            cfg->modules[mi] ? "true" : "false");
        }
    }

    if (cfg->n_codecs > 0) {
        fputs("\ncodecs:\n", f);
        for (size_t i = 0; i < cfg->n_codecs; i++) {
            const sp_codec_cfg *c = &cfg->codecs[i];
            fputs("  - id: ", f);
            emit_quoted(f, c->id);
            fputc('\n', f);
            if (!c->enabled)
                fputs("    enabled: false\n", f);
            /* Only what was actually chosen: an unset parameter written
               back as a number would turn the codec's own default into
               a decision the operator never made. */
            if (c->bitrate >= 0)
                fprintf(f, "    bitrate: %d\n", c->bitrate);
            if (c->complexity >= 0)
                fprintf(f, "    complexity: %d\n", c->complexity);
            if (c->expected_loss_pct >= 0)
                fprintf(f, "    expected_loss_pct: %d\n",
                        c->expected_loss_pct);
            if (c->ptime_ms >= 0)
                fprintf(f, "    ptime_ms: %d\n", c->ptime_ms);
            if (c->cbr >= 0)
                fprintf(f, "    cbr: %s\n", c->cbr ? "true" : "false");
        }
    }

    fputs("\naccounts:", f);
    if (cfg->n_accounts == 0) {
        fputs(" []\n", f);
    } else {
        fputc('\n', f);
        for (size_t i = 0; i < cfg->n_accounts; i++) {
            const sp_account_cfg *a = &cfg->accounts[i];
            fputs("  - id: ", f);
            emit_quoted(f, a->id);
            fputc('\n', f);
            emit_key_str(f, "    ", "user", a->user);
            emit_key_str(f, "    ", "domain", a->domain);
            emit_key_str(f, "    ", "auth_user", a->auth_user);
            emit_key_str(f, "    ", "password", a->password);
            emit_key_str(f, "    ", "registrar", a->registrar);
            emit_key_str(f, "    ", "outbound_proxy", a->outbound_proxy);
            emit_key_str(f, "    ", "stun_server", a->stun_server);
            fprintf(f, "    transport: %s\n", transport_str(a->transport));
            fprintf(f, "    reg_expires_s: %d\n", a->reg_expires_s);
            fprintf(f, "    register: %s\n", a->do_register ? "true" : "false");
            fprintf(f, "    srtp: %s\n", srtp_str(a->srtp));
        }
    }

    if (cfg->n_presets > 0) {
        fputs("\nheader_presets:\n", f);
        for (size_t i = 0; i < cfg->n_presets; i++) {
            fputs("  - name: ", f);
            emit_quoted(f, cfg->presets[i].name);
            fputs("\n    headers:\n", f);
            for (size_t j = 0; j < cfg->presets[i].n_headers; j++) {
                fputs("      - {name: ", f);
                emit_quoted(f, cfg->presets[i].headers[j].name);
                fputs(", value: ", f);
                emit_quoted(f, cfg->presets[i].headers[j].value);
                fputs("}\n", f);
            }
        }
    }

    if (cfg->n_header_names > 0) {
        fputs("\nheader_names: [", f);
        for (size_t i = 0; i < cfg->n_header_names; i++) {
            if (i)
                fputs(", ", f);
            emit_quoted(f, cfg->header_names[i]);
        }
        fputs("]\n", f);
    }

    emit_rules(f, cfg);

    /* Flush and sync before rename, or a crash can rename a file whose
       contents are not on disk yet. */
    int bad = ferror(f);
    if (!bad && fflush(f) != 0)
        bad = 1;
    if (!bad && fsync(fileno(f)) != 0)
        bad = 1;
    if (fclose(f) != 0)
        bad = 1;
    if (bad) {
        snprintf(err, errlen, "error writing %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return SP_ERR_CONFIG;
    }

    if (rename(tmp, path) != 0) {
        snprintf(err, errlen, "cannot replace %s: %s", path, strerror(errno));
        unlink(tmp);
        return SP_ERR_CONFIG;
    }
    return SP_OK;
}
