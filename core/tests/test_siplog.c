#include "sp_test.h"
#include "comlink/core.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static const char *cfg_with(const char *name, const char *extra) {
    static char p[512];
    snprintf(p, sizeof p, "%s/%s.yaml", getenv("SP_TEST_TMP"), name);
    char db[512];
    snprintf(db, sizeof db, "%s/%s.db", getenv("SP_TEST_TMP"), name);
    unlink(db);
    FILE *f = fopen(p, "w");
    fprintf(f, "log_level: 0\nsip_port: 0\naudio_device: none\n"
               "db_file: %s\n%s"
               "accounts:\n  - id: t\n    user: 1\n    domain: 127.0.0.1\n"
               "    register: false\n", db, extra);
    fclose(f);
    return p;
}

/* A call attempt to a dead port still produces a real INVITE, so the
   log fills without needing a peer. */
static void make_traffic(sp_core *c) {
    sp_call_id call = 0;
    sp_call_make(c, 1, "sip:x@127.0.0.1:65530", NULL, 0, &call);
    usleep(600000);
}

/* Wait for a condition rather than for a duration. SIP capture is
   synchronous on the sending thread, but action-log entries come from
   events, which arrive whenever the stack decides; a fixed sleep races
   them and fails intermittently. */
static size_t wait_for_actions(sp_core *c, size_t want, int max_ms) {
    const sp_action_entry *e = NULL;
    size_t n = 0;
    for (int i = 0; i < max_ms; i += 20) {
        if (sp_actionlog_read(c, 500, &e, &n) == SP_OK && n >= want)
            return n;
        usleep(20000);
    }
    return n;
}

SP_TEST(messages_are_captured_and_read_back_newest_first) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_with("sl1", ""), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    make_traffic(c);

    const sp_sip_message *msgs = NULL; size_t n = 0;
    SP_ASSERT(sp_siplog_read(c, NULL, 50, &msgs, &n) == SP_OK);
    SP_ASSERT(n >= 1);
    if (n >= 1) {
        SP_ASSERT(msgs[0].raw != NULL);
        SP_ASSERT(msgs[0].direction != NULL);
        SP_ASSERT(msgs[0].ts_us > 0);
    }
    if (n >= 2)
        SP_ASSERT(msgs[0].ts_us >= msgs[1].ts_us);
    sp_core_stop(c); sp_core_destroy(c);
}

SP_TEST(the_invite_is_in_the_log_with_its_call_id) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_with("sl2", ""), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    make_traffic(c);
    const sp_sip_message *msgs = NULL; size_t n = 0;
    SP_ASSERT(sp_siplog_read(c, NULL, 50, &msgs, &n) == SP_OK);
    int invite = 0;
    const char *cid = NULL;
    for (size_t i = 0; i < n; i++) {
        if (msgs[i].method && strcmp(msgs[i].method, "INVITE") == 0) {
            invite = 1;
            cid = msgs[i].call_id;
            SP_ASSERT(strstr(msgs[i].raw, "INVITE sip:") != NULL);
        }
    }
    SP_ASSERT(invite == 1);
    SP_ASSERT(cid != NULL);
    /* Filtering by that Call-ID must return only its own messages. */
    if (cid) {
        char keep[256];
        snprintf(keep, sizeof keep, "%s", cid);
        const sp_sip_message *f = NULL; size_t fn = 0;
        SP_ASSERT(sp_siplog_read(c, keep, 50, &f, &fn) == SP_OK);
        SP_ASSERT(fn >= 1);
        for (size_t i = 0; i < fn; i++)
            SP_ASSERT_STR(f[i].call_id, keep);
    }
    sp_core_stop(c); sp_core_destroy(c);
}

SP_TEST(credentials_are_redacted_by_default) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_with("sl3", ""), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    make_traffic(c);
    const sp_sip_message *msgs = NULL; size_t n = 0;
    SP_ASSERT(sp_siplog_read(c, NULL, 50, &msgs, &n) == SP_OK);
    SP_ASSERT(n >= 1);
    for (size_t i = 0; i < n; i++)
        SP_ASSERT(msgs[i].redacted == true);
    sp_core_stop(c); sp_core_destroy(c);
}

SP_TEST(sip_log_false_captures_nothing) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_with("sl4", "sip_log: false\n"), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    make_traffic(c);
    const sp_sip_message *msgs = NULL; size_t n = 99;
    SP_ASSERT(sp_siplog_read(c, NULL, 50, &msgs, &n) == SP_OK);
    SP_ASSERT(n == 0);
    sp_core_stop(c); sp_core_destroy(c);
}

SP_TEST(clear_empties_the_log) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_with("sl5", ""), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    make_traffic(c);
    SP_ASSERT(sp_siplog_clear(c) == SP_OK);
    const sp_sip_message *msgs = NULL; size_t n = 99;
    SP_ASSERT(sp_siplog_read(c, NULL, 50, &msgs, &n) == SP_OK);
    SP_ASSERT(n == 0);
    sp_core_stop(c); sp_core_destroy(c);
}

/* The trace must outlive the process, or it is not a log. */
SP_TEST(the_log_survives_a_restart) {
    const char *p = cfg_with("sl6", "");
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    make_traffic(c);
    const sp_sip_message *m1 = NULL; size_t n1 = 0;
    SP_ASSERT(sp_siplog_read(c, NULL, 50, &m1, &n1) == SP_OK);
    SP_ASSERT(n1 >= 1);
    sp_core_stop(c); sp_core_destroy(c);

    sp_core *c2 = NULL;
    SP_ASSERT(sp_core_create(p, &c2) == SP_OK);
    if (!c2) return;
    const sp_sip_message *m2 = NULL; size_t n2 = 0;
    SP_ASSERT(sp_siplog_read(c2, NULL, 50, &m2, &n2) == SP_OK);
    SP_ASSERT(n2 == n1);
    sp_core_destroy(c2);
}

SP_TEST(reading_with_bad_arguments_is_rejected) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_with("sl7", ""), &c) == SP_OK);
    if (!c) return;
    size_t n = 0;
    SP_ASSERT(sp_siplog_read(c, NULL, 10, NULL, &n) == SP_ERR_INVALID_ARG);
    const sp_sip_message *m = NULL;
    SP_ASSERT(sp_siplog_read(c, NULL, 10, &m, NULL) == SP_ERR_INVALID_ARG);
    sp_core_destroy(c);
}

SP_TEST(events_become_action_log_entries) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_with("al1", ""), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    make_traffic(c);

    SP_ASSERT(wait_for_actions(c, 1, 5000) >= 1);
    const sp_action_entry *e = NULL; size_t n = 0;
    SP_ASSERT(sp_actionlog_read(c, 100, &e, &n) == SP_OK);
    SP_ASSERT(n >= 1);
    if (n >= 1) {
        SP_ASSERT(e[0].text != NULL);
        SP_ASSERT(e[0].ts_us > 0);
    }
    /* A call was placed, so the history must mention a call. */
    int mentions_call = 0;
    for (size_t i = 0; i < n; i++)
        if (strstr(e[i].text, "call ") != NULL)
            mentions_call = 1;
    SP_ASSERT(mentions_call == 1);
    sp_core_stop(c); sp_core_destroy(c);
}

/* One row per SIP message would duplicate the SIP log and bury the
   history, so those events are deliberately not recorded. */
SP_TEST(sip_message_events_are_not_in_the_action_log) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_with("al2", ""), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    make_traffic(c);
    wait_for_actions(c, 1, 5000);
    const sp_action_entry *e = NULL; size_t n = 0;
    SP_ASSERT(sp_actionlog_read(c, 500, &e, &n) == SP_OK);
    for (size_t i = 0; i < n; i++)
        SP_ASSERT(strstr(e[i].text, "call-id=") == NULL);
    sp_core_stop(c); sp_core_destroy(c);
}

SP_TEST(an_action_can_be_added_by_hand_and_read_back) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_with("al3", ""), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    SP_ASSERT(sp_actionlog_add(c, 3, "operator pressed hold") == SP_OK);
    SP_ASSERT(sp_actionlog_add(c, 3, NULL) == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_actionlog_add(c, 3, "") == SP_ERR_INVALID_ARG);
    const sp_action_entry *e = NULL; size_t n = 0;
    SP_ASSERT(sp_actionlog_read(c, 1, &e, &n) == SP_OK);
    SP_ASSERT(n == 1);
    if (n == 1)
        SP_ASSERT_STR(e[0].text, "operator pressed hold");
    sp_core_stop(c); sp_core_destroy(c);
}

/* The history is the point: it must outlive the process. */
SP_TEST(the_action_log_survives_a_restart) {
    const char *p = cfg_with("al4", "");
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    SP_ASSERT(sp_actionlog_add(c, 3, "before the restart") == SP_OK);
    sp_core_stop(c); sp_core_destroy(c);

    sp_core *c2 = NULL;
    SP_ASSERT(sp_core_create(p, &c2) == SP_OK);
    if (!c2) return;
    const sp_action_entry *e = NULL; size_t n = 0;
    SP_ASSERT(sp_actionlog_read(c2, 200, &e, &n) == SP_OK);
    int found = 0;
    for (size_t i = 0; i < n; i++)
        if (strcmp(e[i].text, "before the restart") == 0)
            found = 1;
    SP_ASSERT(found == 1);
    sp_core_destroy(c2);
}

SP_TEST(clearing_the_action_log_works) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_with("al5", ""), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    SP_ASSERT(sp_actionlog_add(c, 3, "something") == SP_OK);
    SP_ASSERT(sp_actionlog_clear(c) == SP_OK);
    const sp_action_entry *e = NULL; size_t n = 99;
    SP_ASSERT(sp_actionlog_read(c, 50, &e, &n) == SP_OK);
    SP_ASSERT(n == 0);
    sp_core_stop(c); sp_core_destroy(c);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(messages_are_captured_and_read_back_newest_first);
    SP_RUN(the_invite_is_in_the_log_with_its_call_id);
    SP_RUN(credentials_are_redacted_by_default);
    SP_RUN(sip_log_false_captures_nothing);
    SP_RUN(clear_empties_the_log);
    SP_RUN(the_log_survives_a_restart);
    SP_RUN(reading_with_bad_arguments_is_rejected);
    SP_RUN(events_become_action_log_entries);
    SP_RUN(sip_message_events_are_not_in_the_action_log);
    SP_RUN(an_action_can_be_added_by_hand_and_read_back);
    SP_RUN(the_action_log_survives_a_restart);
    SP_RUN(clearing_the_action_log_works);
    return SP_TEST_RESULT();
}
