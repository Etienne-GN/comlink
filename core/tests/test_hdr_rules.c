#include "sp_test.h"
#include "hdr_rules.h"
#include "yaml_tree.h"
#include <stdlib.h>

static sp_hdr_rule *parse_ok(const char *doc, size_t *n) {
    char err[256] = {0};
    sp_yaml_node *root = sp_yaml_parse_string(doc, err, sizeof err);
    SP_ASSERT(root != NULL);
    sp_hdr_rule *r = NULL;
    sp_status st = sp_rules_parse(sp_yaml_get(root, "header_rules"),
                                  &r, n, err, sizeof err);
    if (st != SP_OK)
        printf("  parse failed: %s\n", err);
    SP_ASSERT(st == SP_OK);
    sp_yaml_free(root);
    return r;
}

static sp_status parse_err(const char *doc, char *err, size_t errlen) {
    sp_yaml_node *root = sp_yaml_parse_string(doc, err, errlen);
    SP_ASSERT(root != NULL);
    sp_hdr_rule *r = NULL; size_t n = 0;
    sp_status st = sp_rules_parse(sp_yaml_get(root, "header_rules"), &r, &n,
                                  err, errlen);
    sp_rules_free(r, n);
    sp_yaml_free(root);
    return st;
}

SP_TEST(matches_method_and_direction) {
    size_t n = 0;
    sp_hdr_rule *r = parse_ok(
        "header_rules:\n"
        "  - name: strip-ua\n"
        "    direction: tx\n"
        "    message: request\n"
        "    method: [INVITE]\n"
        "    actions:\n"
        "      - remove: User-Agent\n", &n);
    SP_ASSERT(n == 1);

    const sp_hdr_action *acts[8]; size_t na = 0;
    sp_rule_ctx hit = {SP_DIR_TX, SP_MSG_REQUEST, "INVITE", 0, NULL};
    SP_ASSERT(sp_rules_eval(r, n, &hit, acts, 8, &na) == SP_OK);
    SP_ASSERT(na == 1);
    SP_ASSERT(acts[0]->kind == SP_ACT_REMOVE);
    SP_ASSERT_STR(acts[0]->name, "User-Agent");

    na = 0;
    sp_rule_ctx wrong_method = {SP_DIR_TX, SP_MSG_REQUEST, "REGISTER", 0, NULL};
    sp_rules_eval(r, n, &wrong_method, acts, 8, &na);
    SP_ASSERT(na == 0);

    na = 0;
    sp_rule_ctx wrong_dir = {SP_DIR_RX, SP_MSG_REQUEST, "INVITE", 0, NULL};
    sp_rules_eval(r, n, &wrong_dir, acts, 8, &na);
    SP_ASSERT(na == 0);
    sp_rules_free(r, n);
}

SP_TEST(method_match_is_case_insensitive) {
    size_t n = 0;
    sp_hdr_rule *r = parse_ok(
        "header_rules:\n  - name: c\n    direction: tx\n    message: request\n"
        "    method: [invite]\n    actions:\n      - remove: X\n", &n);
    const sp_hdr_action *acts[8]; size_t na = 0;
    sp_rule_ctx ctx = {SP_DIR_TX, SP_MSG_REQUEST, "INVITE", 0, NULL};
    sp_rules_eval(r, n, &ctx, acts, 8, &na);
    SP_ASSERT(na == 1);
    sp_rules_free(r, n);
}

SP_TEST(empty_method_list_matches_any_method) {
    size_t n = 0;
    sp_hdr_rule *r = parse_ok(
        "header_rules:\n  - name: all\n    direction: tx\n"
        "    message: any\n    actions:\n      - remove: X\n", &n);
    const sp_hdr_action *acts[8]; size_t na = 0;
    sp_rule_ctx a = {SP_DIR_TX, SP_MSG_REQUEST, "OPTIONS", 0, NULL};
    sp_rules_eval(r, n, &a, acts, 8, &na);
    SP_ASSERT(na == 1);
    na = 0;
    sp_rule_ctx b = {SP_DIR_TX, SP_MSG_RESPONSE, "INVITE", 200, NULL};
    sp_rules_eval(r, n, &b, acts, 8, &na);
    SP_ASSERT(na == 1);
    sp_rules_free(r, n);
}

/* Spec 8.1: all matching rules apply; matching does not stop at first hit. */
SP_TEST(all_matching_rules_apply_in_file_order) {
    size_t n = 0;
    sp_hdr_rule *r = parse_ok(
        "header_rules:\n"
        "  - name: first\n    direction: tx\n    message: request\n"
        "    actions:\n      - set: {name: X-A, value: one}\n"
        "  - name: second\n    direction: tx\n    message: request\n"
        "    actions:\n      - set: {name: X-B, value: two}\n", &n);
    SP_ASSERT(n == 2);
    const sp_hdr_action *acts[8]; size_t na = 0;
    sp_rule_ctx ctx = {SP_DIR_TX, SP_MSG_REQUEST, "INVITE", 0, NULL};
    sp_rules_eval(r, n, &ctx, acts, 8, &na);
    SP_ASSERT(na == 2);
    SP_ASSERT_STR(acts[0]->name, "X-A");
    SP_ASSERT_STR(acts[1]->name, "X-B");
    sp_rules_free(r, n);
}

SP_TEST(account_scoped_rule_skips_other_accounts) {
    size_t n = 0;
    sp_hdr_rule *r = parse_ok(
        "header_rules:\n  - name: work-only\n    direction: tx\n"
        "    message: request\n    account: work\n"
        "    actions:\n      - set: {name: X-Geo, value: qc}\n", &n);
    const sp_hdr_action *acts[8]; size_t na = 0;
    sp_rule_ctx work = {SP_DIR_TX, SP_MSG_REQUEST, "INVITE", 0, "work"};
    sp_rules_eval(r, n, &work, acts, 8, &na);
    SP_ASSERT(na == 1);
    na = 0;
    sp_rule_ctx home = {SP_DIR_TX, SP_MSG_REQUEST, "INVITE", 0, "home"};
    sp_rules_eval(r, n, &home, acts, 8, &na);
    SP_ASSERT(na == 0);
    na = 0;
    sp_rule_ctx none = {SP_DIR_TX, SP_MSG_REQUEST, "INVITE", 0, NULL};
    sp_rules_eval(r, n, &none, acts, 8, &na);
    SP_ASSERT(na == 0);
    sp_rules_free(r, n);
}

SP_TEST(hit_counters_increment_only_on_match) {
    size_t n = 0;
    sp_hdr_rule *r = parse_ok(
        "header_rules:\n  - name: only-invite\n    direction: tx\n"
        "    message: request\n    method: [INVITE]\n"
        "    actions:\n      - remove: X\n", &n);
    const sp_hdr_action *acts[8]; size_t na = 0;
    sp_rule_ctx inv = {SP_DIR_TX, SP_MSG_REQUEST, "INVITE", 0, NULL};
    sp_rule_ctx opt = {SP_DIR_TX, SP_MSG_REQUEST, "OPTIONS", 0, NULL};
    SP_ASSERT(r[0].hits == 0);
    sp_rules_eval(r, n, &inv, acts, 8, &na);
    sp_rules_eval(r, n, &opt, acts, 8, &na);
    sp_rules_eval(r, n, &inv, acts, 8, &na);
    SP_ASSERT(r[0].hits == 2);
    sp_rules_free(r, n);
}

SP_TEST(all_three_action_kinds_parse) {
    size_t n = 0;
    sp_hdr_rule *r = parse_ok(
        "header_rules:\n  - name: mix\n    direction: tx\n    message: any\n"
        "    actions:\n"
        "      - remove: A\n"
        "      - set: {name: B, value: b}\n"
        "      - add: {name: C, value: c}\n", &n);
    SP_ASSERT(r[0].n_actions == 3);
    SP_ASSERT(r[0].actions[0].kind == SP_ACT_REMOVE);
    SP_ASSERT(r[0].actions[0].value == NULL);
    SP_ASSERT(r[0].actions[1].kind == SP_ACT_SET);
    SP_ASSERT_STR(r[0].actions[1].value, "b");
    SP_ASSERT(r[0].actions[2].kind == SP_ACT_ADD);
    sp_rules_free(r, n);
}

/* Spec 8.1: statically unsatisfiable rules are rejected at load. A
   method list can never match a response, which is matched by code. */
SP_TEST(response_with_method_list_is_rejected_as_unsatisfiable) {
    char err[256] = {0};
    SP_ASSERT(parse_err(
        "header_rules:\n  - name: bad\n    direction: tx\n"
        "    message: response\n    method: [INVITE]\n"
        "    actions:\n      - remove: X\n", err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "bad") != NULL);
}

SP_TEST(unknown_rule_key_is_rejected) {
    char err[256] = {0};
    SP_ASSERT(parse_err(
        "header_rules:\n  - name: typo\n    directon: tx\n"
        "    message: any\n    actions:\n      - remove: X\n",
        err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "directon") != NULL);
}

SP_TEST(unknown_action_is_rejected) {
    char err[256] = {0};
    SP_ASSERT(parse_err(
        "header_rules:\n  - name: a\n    direction: tx\n    message: any\n"
        "    actions:\n      - mangle: X\n", err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "mangle") != NULL);
}

SP_TEST(rule_with_no_actions_is_rejected) {
    char err[256] = {0};
    SP_ASSERT(parse_err(
        "header_rules:\n  - name: empty\n    direction: tx\n"
        "    message: any\n    actions: []\n", err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "empty") != NULL);
}

SP_TEST(bad_direction_is_rejected) {
    char err[256] = {0};
    SP_ASSERT(parse_err(
        "header_rules:\n  - name: d\n    direction: sideways\n"
        "    message: any\n    actions:\n      - remove: X\n",
        err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "sideways") != NULL);
}

SP_TEST(set_without_value_is_rejected) {
    char err[256] = {0};
    SP_ASSERT(parse_err(
        "header_rules:\n  - name: s\n    direction: tx\n    message: any\n"
        "    actions:\n      - set: {name: X-Only}\n",
        err, sizeof err) == SP_ERR_CONFIG);
}

SP_TEST(null_ruleset_evaluates_to_no_actions) {
    const sp_hdr_action *acts[8]; size_t na = 99;
    sp_rule_ctx ctx = {SP_DIR_TX, SP_MSG_REQUEST, "INVITE", 0, NULL};
    SP_ASSERT(sp_rules_eval(NULL, 0, &ctx, acts, 8, &na) == SP_OK);
    SP_ASSERT(na == 0);
}

SP_TEST(output_capacity_overflow_is_reported_not_overrun) {
    size_t n = 0;
    sp_hdr_rule *r = parse_ok(
        "header_rules:\n"
        "  - name: a\n    direction: tx\n    message: any\n"
        "    actions:\n      - remove: A\n"
        "  - name: b\n    direction: tx\n    message: any\n"
        "    actions:\n      - remove: B\n", &n);
    const sp_hdr_action *acts[1]; size_t na = 0;
    sp_rule_ctx ctx = {SP_DIR_TX, SP_MSG_REQUEST, "INVITE", 0, NULL};
    SP_ASSERT(sp_rules_eval(r, n, &ctx, acts, 1, &na) == SP_ERR_INTERNAL);
    sp_rules_free(r, n);
}

int main(void) {
    SP_RUN(matches_method_and_direction);
    SP_RUN(method_match_is_case_insensitive);
    SP_RUN(empty_method_list_matches_any_method);
    SP_RUN(all_matching_rules_apply_in_file_order);
    SP_RUN(account_scoped_rule_skips_other_accounts);
    SP_RUN(hit_counters_increment_only_on_match);
    SP_RUN(all_three_action_kinds_parse);
    SP_RUN(response_with_method_list_is_rejected_as_unsatisfiable);
    SP_RUN(unknown_rule_key_is_rejected);
    SP_RUN(unknown_action_is_rejected);
    SP_RUN(rule_with_no_actions_is_rejected);
    SP_RUN(bad_direction_is_rejected);
    SP_RUN(set_without_value_is_rejected);
    SP_RUN(null_ruleset_evaluates_to_no_actions);
    SP_RUN(output_capacity_overflow_is_reported_not_overrun);
    return SP_TEST_RESULT();
}
