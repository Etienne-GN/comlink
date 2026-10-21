#include "sp_test.h"
#include "comlink/core.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>

static const char *write_cfg(const char *name, const char *body) {
    static char p[512];
    snprintf(p, sizeof p, "%s/%s", getenv("SP_TEST_TMP"), name);
    FILE *f = fopen(p, "w");
    if (!f) return NULL;
    fputs(body, f);
    fclose(f);
    return p;
}

static const char *cfg_path(void) {
    return write_cfg("lifecycle.yaml",
        "log_level: 0\n"
        "sip_port: 0\n"
        "accounts:\n"
        "  - id: t\n    user: 1000\n    domain: 127.0.0.1\n"
        "    transport: udp\n    register: false\n");
}

SP_TEST(create_start_stop_destroy_round_trips) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_path(), &c) == SP_OK);
    SP_ASSERT(c != NULL);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    SP_ASSERT(sp_core_stop(c) == SP_OK);
    sp_core_destroy(c);
}

SP_TEST(create_rejects_a_missing_config) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create("/nonexistent/x.yaml", &c) == SP_ERR_CONFIG);
    SP_ASSERT(c == NULL);
}

SP_TEST(create_rejects_null_output) {
    SP_ASSERT(sp_core_create(cfg_path(), NULL) == SP_ERR_INVALID_ARG);
}

SP_TEST(double_start_is_a_state_error) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_path(), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    SP_ASSERT(sp_core_start(c) == SP_ERR_STATE);
    sp_core_stop(c); sp_core_destroy(c);
}

SP_TEST(stop_before_start_is_a_state_error) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_path(), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_stop(c) == SP_ERR_STATE);
    sp_core_destroy(c);
}

/* Spec 7.1: an unregistered foreign thread calling into pjsua trips an
   assertion, so every entry point must self-register. */
static void *foreign(void *arg) {
    sp_core *c = arg;
    sp_account_state st;
    sp_status s = sp_account_get_state(c, 999999, &st);
    return (void *)(intptr_t)(s == SP_ERR_NOT_FOUND);
}

SP_TEST(foreign_thread_may_call_in_without_aborting) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_path(), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    pthread_t th; void *ret = NULL;
    pthread_create(&th, NULL, foreign, c);
    pthread_join(th, &ret);
    SP_ASSERT((intptr_t)ret == 1);
    sp_core_stop(c); sp_core_destroy(c);
}

SP_TEST(rules_stats_lists_every_configured_rule) {
    const char *p = write_cfg("stats.yaml",
        "log_level: 0\n"
        "accounts:\n  - id: t\n    user: 1\n    domain: 127.0.0.1\n"
        "    register: false\n"
        "header_rules:\n"
        "  - name: r1\n    direction: tx\n    message: any\n"
        "    actions:\n      - remove: X\n");
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    if (!c) return;
    const sp_rule_stat *st = NULL; size_t n = 0;
    SP_ASSERT(sp_rules_stats(c, &st, &n) == SP_OK);
    SP_ASSERT(n == 1);
    if (n == 1) {
        SP_ASSERT_STR(st[0].name, "r1");
        SP_ASSERT(st[0].hits == 0);
    }
    sp_core_destroy(c);
}

SP_TEST(error_detail_is_never_null) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_path(), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_last_error_detail(c) != NULL);
    sp_account_state st;
    SP_ASSERT(sp_account_get_state(c, 424242, &st) == SP_ERR_NOT_FOUND);
    SP_ASSERT(sp_last_error_detail(c) != NULL);
    sp_core_destroy(c);
}

/* The pjsip_module holds one core per process, so a second concurrent
   start must be refused rather than silently rewiring the first. */
SP_TEST(a_second_concurrent_core_is_refused) {
    sp_core *a = NULL, *b = NULL;
    SP_ASSERT(sp_core_create(cfg_path(), &a) == SP_OK);
    SP_ASSERT(sp_core_create(cfg_path(), &b) == SP_OK);
    if (!a || !b) return;
    SP_ASSERT(sp_core_start(a) == SP_OK);
    SP_ASSERT(sp_core_start(b) == SP_ERR_STATE);
    sp_core_stop(a);
    sp_core_destroy(a);
    sp_core_destroy(b);
}

SP_TEST(config_with_bad_yaml_fails_before_touching_pjsua) {
    const char *p = write_cfg("bad.yaml", "accounts: [oops\n");
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_ERR_CONFIG);
    SP_ASSERT(c == NULL);
}

/* Wifi to LTE, or a cable pulled. It was a stub that returned
   SP_ERR_STATE on a running core, which is indistinguishable from
   "the core is not running" -- the one answer a caller must not get
   wrong when deciding whether to retry. */
SP_TEST(a_network_change_is_handled_on_a_running_core) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_path(), &c) == SP_OK);
    if (!c) return;
    /* Before start: still a state error, and that one is real. */
    SP_ASSERT(sp_core_network_changed(c) == SP_ERR_STATE);
    SP_ASSERT(sp_core_start(c) == SP_OK);

    sp_account_id acc = 0;
    {
        sp_account_id first[4];
        size_t fn = 0;
        SP_ASSERT(sp_account_list(c, first, 4, &fn) == SP_OK);
        SP_ASSERT(fn == 1);
        acc = fn ? first[0] : 0;
    }

    SP_ASSERT(sp_core_network_changed(c) == SP_OK);
    /* A second one straight away, because a handset crossing a
       boundary reports the change more than once. Either it is
       accepted or it is refused as already in progress; what it must
       never be is a second restart on top of the first, which leaves
       every listener marked restarting and every call refused with
       PJSIP_ETPNOTAVAIL. */
    sp_status again = sp_core_network_changed(c);
    SP_ASSERT(again == SP_OK || again == SP_ERR_STATE);

    /* And the phone still works afterwards, which is the assertion
       that matters: a network change that leaves the transport
       unusable passes every check about accounts and fails the only
       one anybody notices. */
    for (int i = 0; i < 100; i++) {
        sp_call_id call = 0;
        sp_status st = sp_call_make(c, acc, "sip:x@127.0.0.1:5999", NULL, 0,
                                    &call);
        if (st == SP_OK) {
            sp_call_hangup(c, call, 486);
            break;
        }
        SP_ASSERT(i < 99);
        if (i == 99)
            SP_ASSERT(st == SP_OK);
        usleep(100 * 1000);
    }

    sp_account_id ids[4];
    size_t n = 0;
    SP_ASSERT(sp_account_list(c, ids, 4, &n) == SP_OK);
    SP_ASSERT(n == 1);
    SP_ASSERT(sp_core_stop(c) == SP_OK);
    sp_core_destroy(c);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(create_rejects_a_missing_config);
    SP_RUN(create_rejects_null_output);
    SP_RUN(config_with_bad_yaml_fails_before_touching_pjsua);
    SP_RUN(rules_stats_lists_every_configured_rule);
    SP_RUN(error_detail_is_never_null);
    SP_RUN(stop_before_start_is_a_state_error);
    SP_RUN(create_start_stop_destroy_round_trips);
    SP_RUN(double_start_is_a_state_error);
    SP_RUN(foreign_thread_may_call_in_without_aborting);
    SP_RUN(a_second_concurrent_core_is_refused);
    SP_RUN(a_network_change_is_handled_on_a_running_core);
    return SP_TEST_RESULT();
}
