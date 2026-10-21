#include "hdr_rules.h"
#include "yaml_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *const RULE_KEYS[] = {
    "name", "direction", "message", "method", "account", "actions", NULL
};

static const char *const ACTION_KEYS[] = { "set", "remove", "add", NULL };

static int known(const char *const *set, const char *key) {
    for (size_t i = 0; set[i]; i++)
        if (strcmp(set[i], key) == 0)
            return 1;
    return 0;
}

static void action_free(sp_hdr_action *a) {
    free(a->name);
    free(a->value);
}

void sp_rules_free(sp_hdr_rule *rules, size_t n) {
    if (!rules)
        return;
    for (size_t i = 0; i < n; i++) {
        sp_hdr_rule *r = &rules[i];
        free(r->name);
        free(r->account);
        for (size_t j = 0; j < r->n_methods; j++)
            free(r->methods[j]);
        free(r->methods);
        for (size_t j = 0; j < r->n_actions; j++)
            action_free(&r->actions[j]);
        free(r->actions);
    }
    free(rules);
}

static sp_status parse_action(sp_yaml_node *node, sp_hdr_action *out,
                              const char *rule_name, char *err, size_t errlen) {
    if (sp_yaml_kind_of(node) != SP_YAML_MAP || sp_yaml_len(node) != 1) {
        snprintf(err, errlen,
                 "line %d: rule \"%s\": each action must be a single-key "
                 "mapping of set, remove or add",
                 sp_yaml_line(node), rule_name);
        return SP_ERR_CONFIG;
    }
    const char *key = sp_yaml_key_at(node, 0);
    if (!known(ACTION_KEYS, key)) {
        snprintf(err, errlen,
                 "line %d: rule \"%s\": unknown action \"%s\"",
                 sp_yaml_line(node), rule_name, key);
        return SP_ERR_CONFIG;
    }
    sp_yaml_node *val = sp_yaml_get(node, key);

    if (strcmp(key, "remove") == 0) {
        const char *name = sp_yaml_scalar(val);
        if (!name || !*name) {
            snprintf(err, errlen,
                     "line %d: rule \"%s\": remove needs a header name",
                     sp_yaml_line(node), rule_name);
            return SP_ERR_CONFIG;
        }
        out->kind = SP_ACT_REMOVE;
        out->name = strdup(name);
        out->value = NULL;
        return out->name ? SP_OK : SP_ERR_INTERNAL;
    }

    const char *name = sp_yaml_scalar(sp_yaml_get(val, "name"));
    const char *value = sp_yaml_scalar(sp_yaml_get(val, "value"));
    if (!name || !*name || !value) {
        snprintf(err, errlen,
                 "line %d: rule \"%s\": %s needs name and value",
                 sp_yaml_line(node), rule_name, key);
        return SP_ERR_CONFIG;
    }
    out->kind = (strcmp(key, "set") == 0) ? SP_ACT_SET : SP_ACT_ADD;
    out->name = strdup(name);
    out->value = strdup(value);
    return (out->name && out->value) ? SP_OK : SP_ERR_INTERNAL;
}

static sp_status parse_rule(sp_yaml_node *node, sp_hdr_rule *out,
                            char *err, size_t errlen) {
    if (sp_yaml_kind_of(node) != SP_YAML_MAP) {
        snprintf(err, errlen, "line %d: each header rule must be a mapping",
                 sp_yaml_line(node));
        return SP_ERR_CONFIG;
    }
    for (size_t i = 0; i < sp_yaml_len(node); i++) {
        const char *key = sp_yaml_key_at(node, i);
        if (!known(RULE_KEYS, key)) {
            snprintf(err, errlen, "line %d: unknown header rule key \"%s\"",
                     sp_yaml_line(node), key);
            return SP_ERR_CONFIG;
        }
    }

    const char *name = sp_yaml_scalar(sp_yaml_get(node, "name"));
    if (!name || !*name) {
        snprintf(err, errlen, "line %d: header rule is missing \"name\"",
                 sp_yaml_line(node));
        return SP_ERR_CONFIG;
    }
    out->name = strdup(name);
    if (!out->name)
        return SP_ERR_INTERNAL;

    const char *dir = sp_yaml_scalar(sp_yaml_get(node, "direction"));
    if (!dir || (strcmp(dir, "tx") && strcmp(dir, "rx"))) {
        snprintf(err, errlen,
                 "line %d: rule \"%s\": direction must be tx or rx, got \"%s\"",
                 sp_yaml_line(node), name, dir ? dir : "(missing)");
        return SP_ERR_CONFIG;
    }
    out->direction = (strcmp(dir, "tx") == 0) ? SP_DIR_TX : SP_DIR_RX;

    const char *msg = sp_yaml_scalar(sp_yaml_get(node, "message"));
    if (!msg)
        msg = "any";
    if (strcmp(msg, "request") == 0)
        out->message = SP_MSG_REQUEST;
    else if (strcmp(msg, "response") == 0)
        out->message = SP_MSG_RESPONSE;
    else if (strcmp(msg, "any") == 0)
        out->message = SP_MSG_ANY;
    else {
        snprintf(err, errlen,
                 "line %d: rule \"%s\": message must be request, response or "
                 "any, got \"%s\"", sp_yaml_line(node), name, msg);
        return SP_ERR_CONFIG;
    }

    sp_yaml_node *methods = sp_yaml_get(node, "method");
    size_t n_methods = sp_yaml_len(methods);
    if (n_methods > 0) {
        out->methods = calloc(n_methods, sizeof *out->methods);
        if (!out->methods)
            return SP_ERR_INTERNAL;
        for (size_t i = 0; i < n_methods; i++) {
            const char *m = sp_yaml_scalar(sp_yaml_at(methods, i));
            if (!m || !*m) {
                snprintf(err, errlen,
                         "line %d: rule \"%s\": method list entry %zu is empty",
                         sp_yaml_line(node), name, i);
                return SP_ERR_CONFIG;
            }
            out->methods[i] = strdup(m);
            if (!out->methods[i])
                return SP_ERR_INTERNAL;
            out->n_methods = i + 1;
        }
    }

    /* A response is matched by status code, never by method, so this
       combination can never fire. Reject it at load rather than let it
       sit in the config looking effective. */
    if (out->message == SP_MSG_RESPONSE && out->n_methods > 0) {
        snprintf(err, errlen,
                 "line %d: rule \"%s\": message: response cannot be combined "
                 "with a method list, since responses match on status code; "
                 "this rule could never fire",
                 sp_yaml_line(node), name);
        return SP_ERR_CONFIG;
    }

    const char *acct = sp_yaml_scalar(sp_yaml_get(node, "account"));
    if (acct) {
        out->account = strdup(acct);
        if (!out->account)
            return SP_ERR_INTERNAL;
    }

    sp_yaml_node *actions = sp_yaml_get(node, "actions");
    size_t n_actions = sp_yaml_len(actions);
    if (n_actions == 0) {
        snprintf(err, errlen,
                 "line %d: rule \"%s\": needs at least one action",
                 sp_yaml_line(node), name);
        return SP_ERR_CONFIG;
    }
    out->actions = calloc(n_actions, sizeof *out->actions);
    if (!out->actions)
        return SP_ERR_INTERNAL;
    for (size_t i = 0; i < n_actions; i++) {
        sp_status st = parse_action(sp_yaml_at(actions, i), &out->actions[i],
                                    name, err, errlen);
        out->n_actions = i + 1;
        if (st != SP_OK)
            return st;
    }
    return SP_OK;
}

sp_status sp_rules_parse(sp_yaml_node *seq, sp_hdr_rule **out, size_t *n_out,
                         char *err, size_t errlen) {
    if (!out || !n_out)
        return SP_ERR_INVALID_ARG;
    *out = NULL;
    *n_out = 0;

    size_t n = sp_yaml_len(seq);
    if (n == 0)
        return SP_OK;
    if (sp_yaml_kind_of(seq) != SP_YAML_SEQ) {
        snprintf(err, errlen, "line %d: header_rules must be a sequence",
                 sp_yaml_line(seq));
        return SP_ERR_CONFIG;
    }

    sp_hdr_rule *rules = calloc(n, sizeof *rules);
    if (!rules) {
        snprintf(err, errlen, "out of memory");
        return SP_ERR_INTERNAL;
    }
    for (size_t i = 0; i < n; i++) {
        sp_status st = parse_rule(sp_yaml_at(seq, i), &rules[i], err, errlen);
        if (st != SP_OK) {
            sp_rules_free(rules, i + 1);
            return st;
        }
    }
    *out = rules;
    *n_out = n;
    return SP_OK;
}

static int method_matches(const sp_hdr_rule *r, const char *method) {
    if (r->n_methods == 0)
        return 1;
    if (!method)
        return 0;
    for (size_t i = 0; i < r->n_methods; i++)
        if (strcasecmp(r->methods[i], method) == 0)
            return 1;
    return 0;
}

static int rule_matches(const sp_hdr_rule *r, const sp_rule_ctx *ctx) {
    if (r->direction != ctx->direction)
        return 0;
    if (r->message != SP_MSG_ANY && r->message != ctx->message)
        return 0;
    if (!method_matches(r, ctx->method))
        return 0;
    if (r->account) {
        if (!ctx->account || strcmp(r->account, ctx->account) != 0)
            return 0;
    }
    return 1;
}

sp_status sp_rules_eval(sp_hdr_rule *rules, size_t n_rules,
                        const sp_rule_ctx *ctx,
                        const sp_hdr_action **out, size_t cap, size_t *n_out) {
    if (!ctx || !out || !n_out)
        return SP_ERR_INVALID_ARG;
    *n_out = 0;
    if (!rules || n_rules == 0)
        return SP_OK;

    size_t n = 0;
    for (size_t i = 0; i < n_rules; i++) {
        if (!rule_matches(&rules[i], ctx))
            continue;
        if (n + rules[i].n_actions > cap)
            return SP_ERR_INTERNAL;
        atomic_fetch_add_explicit(&rules[i].hits, 1,
                                  memory_order_relaxed);
        for (size_t j = 0; j < rules[i].n_actions; j++)
            out[n++] = &rules[i].actions[j];
    }
    *n_out = n;
    return SP_OK;
}
