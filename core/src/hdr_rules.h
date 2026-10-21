/*
 * Header rule evaluation. Deliberately free of PJSIP types so rule
 * semantics are provable without a SIP stack or a registrar; the
 * pjsip_module that applies these actions lives in hdr_module.c.
 */
#ifndef SP_HDR_RULES_H
#define SP_HDR_RULES_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "comlink/core.h"

typedef enum { SP_DIR_TX, SP_DIR_RX } sp_direction;
typedef enum { SP_MSG_REQUEST, SP_MSG_RESPONSE, SP_MSG_ANY } sp_msg_kind;
typedef enum { SP_ACT_SET, SP_ACT_REMOVE, SP_ACT_ADD } sp_action_kind;

typedef struct {
    sp_action_kind kind;
    char          *name;
    char          *value;   /* NULL for SP_ACT_REMOVE */
} sp_hdr_action;

typedef struct {
    char          *name;
    sp_direction   direction;
    sp_msg_kind    message;
    char         **methods;
    size_t         n_methods;   /* 0 => any method */
    char          *account;     /* NULL => any account */
    sp_hdr_action *actions;
    size_t         n_actions;
    /* Incremented from pjsip worker threads while being read by a
       frontend thread through sp_rules_stats, so it is atomic. */
    _Atomic uint64_t hits;
} sp_hdr_rule;

typedef struct {
    sp_direction  direction;
    sp_msg_kind   message;      /* never SP_MSG_ANY in a context */
    const char   *method;
    int           status_code;  /* 0 for requests */
    const char   *account;      /* NULL when not account-scoped */
} sp_rule_ctx;

struct sp_yaml_node;

sp_status sp_rules_parse(struct sp_yaml_node *seq, sp_hdr_rule **out,
                         size_t *n_out, char *err, size_t errlen);
void      sp_rules_free(sp_hdr_rule *rules, size_t n);

/* Appends the actions of every matching rule, in file order, and
   increments each matching rule's hit counter. The actions point into
   the ruleset and are not owned by the caller. Returns
   SP_ERR_INTERNAL rather than overrunning cap. */
sp_status sp_rules_eval(sp_hdr_rule *rules, size_t n_rules,
                        const sp_rule_ctx *ctx,
                        const sp_hdr_action **out, size_t cap, size_t *n_out);

#endif /* SP_HDR_RULES_H */
