#include "sp_test.h"
#include <string.h>
#include "comlink/core.h"
#include <stdio.h>
#include <stdlib.h>

static sp_core *booted(void) {
    static char p[512];
    snprintf(p, sizeof p, "%s/acct.yaml", getenv("SP_TEST_TMP"));
    FILE *f = fopen(p, "w");
    fputs("log_level: 0\naccounts: []\n", f);
    fclose(f);
    sp_core *c = NULL;
    if (sp_core_create(p, &c) != SP_OK) return NULL;
    if (sp_core_start(c) != SP_OK) { sp_core_destroy(c); return NULL; }
    return c;
}

static void shutdown_core(sp_core *c) {
    sp_core_stop(c);
    sp_core_destroy(c);
}

static sp_account_cfg base_cfg(const char *id, const char *user) {
    sp_account_cfg a = {0};
    a.id = id;
    a.user = user;
    a.domain = "127.0.0.1";
    a.auth_user = user;
    a.transport = SP_TP_UDP;
    a.reg_expires_s = 300;
    a.do_register = false;   /* no registrar available in unit tests */
    return a;
}

SP_TEST(adding_an_account_yields_a_nonzero_handle) {
    sp_core *c = booted(); SP_ASSERT(c != NULL); if (!c) return;
    sp_account_cfg cfg = base_cfg("a", "1000");
    sp_account_id id = 0;
    SP_ASSERT(sp_account_add(c, &cfg, &id) == SP_OK);
    SP_ASSERT(id != 0);
    shutdown_core(c);
}

SP_TEST(two_accounts_get_distinct_handles) {
    sp_core *c = booted(); SP_ASSERT(c != NULL); if (!c) return;
    sp_account_cfg a = base_cfg("a", "1000");
    sp_account_cfg b = base_cfg("b", "1001");
    sp_account_id ia = 0, ib = 0;
    SP_ASSERT(sp_account_add(c, &a, &ia) == SP_OK);
    SP_ASSERT(sp_account_add(c, &b, &ib) == SP_OK);
    SP_ASSERT(ia != ib);
    shutdown_core(c);
}

SP_TEST(duplicate_account_id_is_rejected) {
    sp_core *c = booted(); SP_ASSERT(c != NULL); if (!c) return;
    sp_account_cfg a = base_cfg("dup", "1000");
    sp_account_id i1 = 0, i2 = 0;
    SP_ASSERT(sp_account_add(c, &a, &i1) == SP_OK);
    SP_ASSERT(sp_account_add(c, &a, &i2) == SP_ERR_CONFIG);
    SP_ASSERT(i2 == 0);
    shutdown_core(c);
}

SP_TEST(mandatory_srtp_account_is_accepted_and_persisted) {
    sp_core *c = booted(); SP_ASSERT(c != NULL); if (!c) return;
    sp_account_cfg a = base_cfg("s", "1000");
    a.srtp = SP_SRTP_MANDATORY;
    sp_account_id id = 0;
    SP_ASSERT(sp_account_add(c, &a, &id) == SP_OK);
    SP_ASSERT(id != 0);
    sp_account_cfg back = {0};
    SP_ASSERT(sp_account_get_cfg(c, id, &back) == SP_OK);
    SP_ASSERT(back.srtp == SP_SRTP_MANDATORY);
    shutdown_core(c);
}

SP_TEST(missing_required_fields_are_rejected) {
    sp_core *c = booted(); SP_ASSERT(c != NULL); if (!c) return;
    sp_account_cfg a = {0};
    sp_account_id id = 0;
    SP_ASSERT(sp_account_add(c, &a, &id) == SP_ERR_INVALID_ARG);
    a.id = "x";
    SP_ASSERT(sp_account_add(c, &a, &id) == SP_ERR_INVALID_ARG);
    a.user = "u";
    SP_ASSERT(sp_account_add(c, &a, &id) == SP_ERR_INVALID_ARG);
    shutdown_core(c);
}

SP_TEST(removed_account_handle_is_retired) {
    sp_core *c = booted(); SP_ASSERT(c != NULL); if (!c) return;
    sp_account_cfg a = base_cfg("r", "1000");
    sp_account_id id = 0;
    SP_ASSERT(sp_account_add(c, &a, &id) == SP_OK);
    SP_ASSERT(sp_account_remove(c, id) == SP_OK);
    sp_account_state st;
    SP_ASSERT(sp_account_get_state(c, id, &st) == SP_ERR_NOT_FOUND);
    SP_ASSERT(sp_account_set_registration(c, id, true) == SP_ERR_NOT_FOUND);
    SP_ASSERT(sp_account_remove(c, id) == SP_ERR_NOT_FOUND);
    shutdown_core(c);
}

/* Reusing an id is fine once the previous holder is gone. */
SP_TEST(account_id_may_be_reused_after_removal) {
    sp_core *c = booted(); SP_ASSERT(c != NULL); if (!c) return;
    sp_account_cfg a = base_cfg("recycle", "1000");
    sp_account_id first = 0, second = 0;
    SP_ASSERT(sp_account_add(c, &a, &first) == SP_OK);
    SP_ASSERT(sp_account_remove(c, first) == SP_OK);
    SP_ASSERT(sp_account_add(c, &a, &second) == SP_OK);
    SP_ASSERT(second != first);
    shutdown_core(c);
}

SP_TEST(state_of_an_unregistered_account_reads_as_not_registered) {
    sp_core *c = booted(); SP_ASSERT(c != NULL); if (!c) return;
    sp_account_cfg a = base_cfg("u", "1000");
    sp_account_id id = 0;
    SP_ASSERT(sp_account_add(c, &a, &id) == SP_OK);
    sp_account_state st = {0};
    SP_ASSERT(sp_account_get_state(c, id, &st) == SP_OK);
    SP_ASSERT(st.registered == false);
    shutdown_core(c);
}

SP_TEST(accounts_from_config_are_added_at_start) {
    static char p[512];
    snprintf(p, sizeof p, "%s/acct_cfg.yaml", getenv("SP_TEST_TMP"));
    FILE *f = fopen(p, "w");
    fputs("log_level: 0\n"
          "accounts:\n"
          "  - id: one\n    user: 1000\n    domain: 127.0.0.1\n"
          "    register: false\n"
          "  - id: two\n    user: 1001\n    domain: 127.0.0.1\n"
          "    register: false\n", f);
    fclose(f);
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    /* Handles are issued in config order starting at 1. */
    sp_account_state st;
    SP_ASSERT(sp_account_get_state(c, 1, &st) == SP_OK);
    SP_ASSERT(sp_account_get_state(c, 2, &st) == SP_OK);
    SP_ASSERT(sp_account_get_state(c, 3, &st) == SP_ERR_NOT_FOUND);
    shutdown_core(c);
}

/* The GUI's whole account-management story rests on this: an account
   added at runtime must be in the file, not just in memory. */
SP_TEST(added_account_is_written_to_the_config_file) {
    static char p[512];
    snprintf(p, sizeof p, "%s/persist.yaml", getenv("SP_TEST_TMP"));
    FILE *f = fopen(p, "w");
    fputs("log_level: 0\naccounts: []\n"
          "header_rules:\n"
          "  - name: keepme\n    direction: tx\n    message: any\n"
          "    actions:\n      - remove: X-Gone\n", f);
    fclose(f);

    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);

    sp_account_cfg a = base_cfg("saved", "5551234");
    a.password = "hunter2";
    a.transport = SP_TP_TCP;
    sp_account_id id = 0;
    SP_ASSERT(sp_account_add(c, &a, &id) == SP_OK);
    shutdown_core(c);

    /* Reload from disk in a fresh core: the account must be there. */
    sp_core *c2 = NULL;
    SP_ASSERT(sp_core_create(p, &c2) == SP_OK);
    if (!c2) return;
    SP_ASSERT(sp_core_start(c2) == SP_OK);
    sp_account_state st;
    SP_ASSERT(sp_account_get_state(c2, 1, &st) == SP_OK);
    SP_ASSERT_STR(sp_account_id_of(c2, 1), "saved");
    SP_ASSERT_STR(sp_account_domain_of(c2, 1), "127.0.0.1");

    sp_account_cfg back = {0};
    SP_ASSERT(sp_account_get_cfg(c2, 1, &back) == SP_OK);
    SP_ASSERT_STR(back.user, "5551234");
    SP_ASSERT_STR(back.password, "hunter2");
    SP_ASSERT(back.transport == SP_TP_TCP);

    /* And the pre-existing header rule must have survived the save. */
    const sp_rule_stat *stats = NULL; size_t n = 0;
    SP_ASSERT(sp_rules_stats(c2, &stats, &n) == SP_OK);
    SP_ASSERT(n == 1);
    if (n == 1)
        SP_ASSERT_STR(stats[0].name, "keepme");
    shutdown_core(c2);
}

SP_TEST(removed_account_is_erased_from_the_config_file) {
    static char p[512];
    snprintf(p, sizeof p, "%s/persist_rm.yaml", getenv("SP_TEST_TMP"));
    FILE *f = fopen(p, "w");
    fputs("log_level: 0\n"
          "accounts:\n"
          "  - id: gone\n    user: 1\n    domain: 127.0.0.1\n"
          "    register: false\n"
          "  - id: stays\n    user: 2\n    domain: 127.0.0.1\n"
          "    register: false\n", f);
    fclose(f);

    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    SP_ASSERT(sp_account_remove(c, 1) == SP_OK);
    shutdown_core(c);

    sp_core *c2 = NULL;
    SP_ASSERT(sp_core_create(p, &c2) == SP_OK);
    if (!c2) return;
    SP_ASSERT(sp_core_start(c2) == SP_OK);
    sp_account_state st;
    SP_ASSERT(sp_account_get_state(c2, 1, &st) == SP_OK);
    SP_ASSERT_STR(sp_account_id_of(c2, 1), "stays");
    SP_ASSERT(sp_account_get_state(c2, 2, &st) == SP_ERR_NOT_FOUND);
    shutdown_core(c2);
}

/* Switching an account on or off must survive a restart, or the phone
   comes back online on its own after you deliberately took it offline.

   The account points at 127.0.0.1 where no registrar answers, so the
   first REGISTER stays in flight and pjsua reports PJSIP_EBUSY for
   further changes. That is real behaviour, so the test covers it
   rather than avoiding it. */
SP_TEST(registration_toggle_is_persisted) {
    static char p[512];
    snprintf(p, sizeof p, "%s/regtoggle.yaml", getenv("SP_TEST_TMP"));
    FILE *f = fopen(p, "w");
    fputs("log_level: 0\n"
          "accounts:\n"
          "  - id: t\n    user: 1\n    domain: 127.0.0.1\n"
          "    password: x\n    register: false\n", f);
    fclose(f);

    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);

    sp_account_cfg cfg = {0};
    SP_ASSERT(sp_account_get_cfg(c, 1, &cfg) == SP_OK);
    SP_ASSERT(cfg.do_register == false);

    /* Switching on must be accepted, which only works because the
       account is given a registrar even when register: false. */
    SP_ASSERT(sp_account_set_registration(c, 1, true) == SP_OK);
    SP_ASSERT(sp_account_get_cfg(c, 1, &cfg) == SP_OK);
    SP_ASSERT(cfg.do_register == true);
    shutdown_core(c);

    /* Reload: still on. */
    sp_core *c2 = NULL;
    SP_ASSERT(sp_core_create(p, &c2) == SP_OK);
    if (!c2) return;
    SP_ASSERT(sp_core_start(c2) == SP_OK);
    SP_ASSERT(sp_account_get_cfg(c2, 1, &cfg) == SP_OK);
    SP_ASSERT(cfg.do_register == true);
    shutdown_core(c2);
}

/* A refused change still records the preference, and reports a state
   error rather than a transport failure so the caller retries instead
   of investigating the network. */
SP_TEST(busy_registration_change_is_a_state_error_and_still_persists) {
    static char p[512];
    snprintf(p, sizeof p, "%s/regbusy.yaml", getenv("SP_TEST_TMP"));
    FILE *f = fopen(p, "w");
    fputs("log_level: 0\n"
          "accounts:\n"
          "  - id: t\n    user: 1\n    domain: 127.0.0.1\n"
          "    password: x\n    register: true\n", f);
    fclose(f);

    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);

    /* The startup REGISTER is unanswered and therefore still in
       flight, so pjsua refuses the change. */
    sp_status st = sp_account_set_registration(c, 1, false);
    SP_ASSERT(st == SP_ERR_STATE);
    SP_ASSERT(strstr(sp_last_error_detail(c), "in flight") != NULL);

    /* Refused on the wire, but the preference was still recorded. */
    sp_account_cfg cfg = {0};
    SP_ASSERT(sp_account_get_cfg(c, 1, &cfg) == SP_OK);
    SP_ASSERT(cfg.do_register == false);
    shutdown_core(c);

    sp_core *c2 = NULL;
    SP_ASSERT(sp_core_create(p, &c2) == SP_OK);
    if (!c2) return;
    SP_ASSERT(sp_core_start(c2) == SP_OK);
    SP_ASSERT(sp_account_get_cfg(c2, 1, &cfg) == SP_OK);
    SP_ASSERT(cfg.do_register == false);
    shutdown_core(c2);
}

SP_TEST(renaming_an_account_carries_the_rules_scoped_to_it) {
    /* The point of this test: a rule scoped to an account by name must
       follow the rename. Without that it keeps matching a name no
       account has, stops firing, and says nothing about it. */
    static char p[512];
    snprintf(p, sizeof p, "%s/rename.yaml", getenv("SP_TEST_TMP"));
    FILE *f = fopen(p, "w");
    SP_ASSERT(f != NULL);
    if (!f) return;
    fputs("log_level: 0\n"
          "accounts:\n"
          "  - id: before\n    user: 1000\n    domain: 127.0.0.1\n"
          "    register: false\n"
          "  - id: other\n    user: 1001\n    domain: 127.0.0.1\n"
          "    register: false\n"
          "header_rules:\n"
          "  - name: scoped\n    direction: tx\n    message: request\n"
          "    account: before\n"
          "    actions:\n      - set: {name: X-A, value: 1}\n"
          "  - name: also-scoped\n    direction: tx\n    message: request\n"
          "    account: before\n"
          "    actions:\n      - set: {name: X-B, value: 2}\n"
          "  - name: unscoped\n    direction: tx\n    message: request\n"
          "    actions:\n      - set: {name: X-C, value: 3}\n"
          "  - name: elsewhere\n    direction: tx\n    message: request\n"
          "    account: other\n"
          "    actions:\n      - set: {name: X-D, value: 4}\n", f);
    fclose(f);

    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);

    sp_account_id ids[8];
    size_t n = 0;
    SP_ASSERT(sp_account_list(c, ids, 8, &n) == SP_OK);
    SP_ASSERT(n == 2);
    if (n != 2) { shutdown_core(c); return; }

    /* Whichever handle is called "before". */
    sp_account_id target = 0;
    for (size_t i = 0; i < n; i++) {
        const char *id = sp_account_id_of(c, ids[i]);
        if (id && strcmp(id, "before") == 0)
            target = ids[i];
    }
    SP_ASSERT(target != 0);
    if (!target) { shutdown_core(c); return; }

    /* An empty name is refused. */
    SP_ASSERT(sp_account_rename(c, target, "", NULL) == SP_ERR_INVALID_ARG);
    /* A name already in use is refused, naming it. */
    SP_ASSERT(sp_account_rename(c, target, "other", NULL)
              == SP_ERR_INVALID_ARG);
    SP_ASSERT(strstr(sp_last_error_detail(c), "other") != NULL);
    /* An unknown handle is not found. */
    SP_ASSERT(sp_account_rename(c, 987654321, "x", NULL) == SP_ERR_NOT_FOUND);
    /* Renaming to the same name is a no-op, not an error. */
    SP_ASSERT(sp_account_rename(c, target, "before", NULL) == SP_OK);

    size_t moved = 999;
    SP_ASSERT(sp_account_rename(c, target, "after", &moved) == SP_OK);
    /* Two rules were scoped to it; the unscoped one and the one scoped
       to another account must not move. */
    SP_ASSERT(moved == 2);
    SP_ASSERT_STR(sp_account_id_of(c, target), "after");

    shutdown_core(c);

    /* And it survived the write: reopening finds the new name, the
       rules pointing at it, and the other account untouched. */
    sp_core *c2 = NULL;
    SP_ASSERT(sp_core_create(p, &c2) == SP_OK);
    if (!c2) return;
    SP_ASSERT(sp_core_start(c2) == SP_OK);
    n = 0;
    SP_ASSERT(sp_account_list(c2, ids, 8, &n) == SP_OK);
    SP_ASSERT(n == 2);
    bool saw_after = false, saw_other = false, saw_before = false;
    for (size_t i = 0; i < n; i++) {
        const char *id = sp_account_id_of(c2, ids[i]);
        if (!id) continue;
        if (!strcmp(id, "after")) saw_after = true;
        if (!strcmp(id, "other")) saw_other = true;
        if (!strcmp(id, "before")) saw_before = true;
    }
    SP_ASSERT(saw_after);
    SP_ASSERT(saw_other);
    SP_ASSERT(!saw_before);

    /* The rules are still four, and two of them now name "after". */
    const sp_rule_stat *stats = NULL;
    size_t ns = 0;
    SP_ASSERT(sp_rules_stats(c2, &stats, &ns) == SP_OK);
    SP_ASSERT(ns == 4);
    shutdown_core(c2);
}

/* The config the core writes has to be one the core can read. An
   account added with reg_expires_s left at zero -- what any C caller
   that memsets the struct passes, and what the Android JNI bridge
   did -- used to be written out verbatim as "reg_expires_s: 0", which
   the loader then rejected as outside 30..86400. The core produced a
   file it refused to open, and the only symptom was a configuration
   error on the next start.

   Asserting the reload is the point. A test that only checked the
   stored value would pass against the broken version too, because the
   broken version stored exactly what it was given. */
SP_TEST(an_account_added_with_no_expiry_still_reloads) {
    static char p[512];
    snprintf(p, sizeof p, "%s/acct-exp.yaml", getenv("SP_TEST_TMP"));
    FILE *f = fopen(p, "w");
    fputs("log_level: 0\naccounts: []\n", f);
    fclose(f);

    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    SP_ASSERT(sp_core_start(c) == SP_OK);

    sp_account_cfg cfg = {0};
    cfg.id = "zero";
    cfg.user = "1000";
    cfg.domain = "127.0.0.1";
    cfg.transport = SP_TP_UDP;
    cfg.do_register = false;
    /* reg_expires_s deliberately left at 0. */
    sp_account_id id = 0;
    SP_ASSERT(sp_account_add(c, &cfg, &id) == SP_OK);
    SP_ASSERT(id != 0);

    sp_account_cfg got = {0};
    SP_ASSERT(sp_account_get_cfg(c, id, &got) == SP_OK);
    SP_ASSERT(got.reg_expires_s == 300);
    shutdown_core(c);

    /* The file the core just wrote, opened by the core. */
    sp_core *again = NULL;
    SP_ASSERT(sp_core_create(p, &again) == SP_OK);
    sp_core_destroy(again);
}

SP_TEST(an_out_of_range_expiry_is_refused_rather_than_written) {
    sp_core *c = booted(); SP_ASSERT(c != NULL); if (!c) return;
    sp_account_cfg cfg = base_cfg("tooshort", "1000");
    cfg.reg_expires_s = 5;
    sp_account_id id = 0;
    SP_ASSERT(sp_account_add(c, &cfg, &id) == SP_ERR_INVALID_ARG);
    SP_ASSERT(id == 0);

    cfg = base_cfg("toolong", "1001");
    cfg.reg_expires_s = 999999;
    SP_ASSERT(sp_account_add(c, &cfg, &id) == SP_ERR_INVALID_ARG);
    shutdown_core(c);
}

/* The password is the reason this entry point exists, so the test
   looks for the password. An account added transiently must leave no
   trace in the file the core writes -- not the id, and above all not
   the credential. */
SP_TEST(a_transient_account_is_not_written_to_the_config) {
    static char p[512];
    snprintf(p, sizeof p, "%s/acct-transient.yaml", getenv("SP_TEST_TMP"));
    FILE *f = fopen(p, "w");
    fputs("log_level: 0\naccounts: []\n", f);
    fclose(f);

    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    SP_ASSERT(sp_core_start(c) == SP_OK);

    sp_account_cfg cfg = base_cfg("transient", "1000");
    cfg.password = "correct-horse-battery-staple";
    sp_account_id id = 0;
    SP_ASSERT(sp_account_add_transient(c, &cfg, &id) == SP_OK);
    SP_ASSERT(id != 0);
    /* Usable, not merely accepted. */
    sp_account_state st;
    SP_ASSERT(sp_account_get_state(c, id, &st) == SP_OK);
    shutdown_core(c);

    char buf[8192];
    f = fopen(p, "r");
    SP_ASSERT(f != NULL);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    SP_ASSERT(strstr(buf, "correct-horse-battery-staple") == NULL);
    SP_ASSERT(strstr(buf, "transient") == NULL);

    /* And a non-transient add still does persist, so the assertion
       above is about the new entry point rather than about the writer
       having quietly stopped working. */
    sp_core *c2 = NULL;
    SP_ASSERT(sp_core_create(p, &c2) == SP_OK);
    SP_ASSERT(sp_core_start(c2) == SP_OK);
    sp_account_cfg kept = base_cfg("kept", "1001");
    sp_account_id id2 = 0;
    SP_ASSERT(sp_account_add(c2, &kept, &id2) == SP_OK);
    shutdown_core(c2);

    f = fopen(p, "r");
    SP_ASSERT(f != NULL);
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    SP_ASSERT(strstr(buf, "kept") != NULL);
}

/* The rules the accounts dialog used to check by hand, moved here so
   both frontends refuse the same account for the same reason. The
   message is the core's too: a frontend that wrote its own would
   disagree with the other platform about what is wrong. */
SP_TEST(validation_names_the_field_that_is_wrong) {
    char why[160];
    sp_account_cfg cfg = base_cfg("work", "1000");
    SP_ASSERT(sp_account_validate(&cfg, why, sizeof why) == SP_OK);
    SP_ASSERT(why[0] == '\0');

    cfg = base_cfg("", "1000");
    SP_ASSERT(sp_account_validate(&cfg, why, sizeof why) == SP_ERR_INVALID_ARG);
    SP_ASSERT(strstr(why, "Name") != NULL);

    cfg = base_cfg("two words", "1000");
    SP_ASSERT(sp_account_validate(&cfg, why, sizeof why) == SP_ERR_INVALID_ARG);
    SP_ASSERT(strstr(why, "spaces") != NULL);

    cfg = base_cfg("work", "");
    SP_ASSERT(sp_account_validate(&cfg, why, sizeof why) == SP_ERR_INVALID_ARG);
    SP_ASSERT(strstr(why, "User") != NULL);

    cfg = base_cfg("work", "1000");
    cfg.domain = "";
    SP_ASSERT(sp_account_validate(&cfg, why, sizeof why) == SP_ERR_INVALID_ARG);
    SP_ASSERT(strstr(why, "Domain") != NULL);

    /* Registering without a password is the one that is easy to get
       wrong, and the wording says what to do about it. */
    cfg = base_cfg("work", "1000");
    cfg.do_register = true;
    cfg.password = NULL;
    SP_ASSERT(sp_account_validate(&cfg, why, sizeof why) == SP_ERR_INVALID_ARG);
    SP_ASSERT(strstr(why, "password") != NULL);

    cfg.password = "s3cret";
    SP_ASSERT(sp_account_validate(&cfg, why, sizeof why) == SP_OK);

    /* The expiry range sp_account_add already enforces, so an
       operator sees it before the add rather than after. */
    cfg = base_cfg("work", "1000");
    cfg.reg_expires_s = 5;
    SP_ASSERT(sp_account_validate(&cfg, why, sizeof why) == SP_ERR_INVALID_ARG);
}

SP_TEST(updating_an_account_keeps_its_handle_live_and_carries_rules) {
    static char p[512];
    snprintf(p, sizeof p, "%s/acct-update.yaml", getenv("SP_TEST_TMP"));
    FILE *f = fopen(p, "w");
    fputs("log_level: 0\naccounts: []\n", f);
    fclose(f);

    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    SP_ASSERT(sp_core_start(c) == SP_OK);

    sp_account_cfg cfg = base_cfg("work", "1000");
    sp_account_id id = 0;
    SP_ASSERT(sp_account_add(c, &cfg, &id) == SP_OK);

    sp_account_cfg edited = base_cfg("desk", "1001");
    edited.reg_expires_s = 600;
    sp_account_id after = 0;
    size_t rules = 0;
    SP_ASSERT(sp_account_update(c, id, &edited, &after, &rules) == SP_OK);
    SP_ASSERT(after != 0);

    /* One account, not two: an update is not an add. */
    sp_account_id ids[8];
    size_t n = 0;
    SP_ASSERT(sp_account_list(c, ids, 8, &n) == SP_OK);
    SP_ASSERT(n == 1);

    sp_account_cfg back = {0};
    SP_ASSERT(sp_account_get_cfg(c, after, &back) == SP_OK);
    SP_ASSERT_STR(back.id, "desk");
    SP_ASSERT_STR(back.user, "1001");
    SP_ASSERT(back.reg_expires_s == 600);
    shutdown_core(c);
}

/* The dangerous half: a failed update must not destroy the account it
   was editing. */
SP_TEST(a_rejected_update_leaves_the_original_account_in_place) {
    sp_core *c = booted(); SP_ASSERT(c != NULL); if (!c) return;
    sp_account_cfg cfg = base_cfg("work", "1000");
    sp_account_id id = 0;
    SP_ASSERT(sp_account_add(c, &cfg, &id) == SP_OK);

    sp_account_cfg bad = base_cfg("work", "1001");
    bad.reg_expires_s = 5;          /* refused by sp_account_add */
    sp_account_id after = 0;
    size_t rules = 0;
    SP_ASSERT(sp_account_update(c, id, &bad, &after, &rules)
              == SP_ERR_INVALID_ARG);

    sp_account_id ids[8];
    size_t n = 0;
    SP_ASSERT(sp_account_list(c, ids, 8, &n) == SP_OK);
    SP_ASSERT(n == 1);
    sp_account_cfg back = {0};
    SP_ASSERT(sp_account_get_cfg(c, ids[0], &back) == SP_OK);
    SP_ASSERT_STR(back.user, "1000");
    shutdown_core(c);
}

/* The rule the dialler needs and both frontends would otherwise own:
   three keystrokes rather than a whole URI. */
SP_TEST(a_bare_extension_is_completed_with_the_account_domain) {
    sp_core *c = booted(); SP_ASSERT(c != NULL); if (!c) return;
    sp_account_cfg cfg = base_cfg("work", "1000");
    cfg.domain = "pbx.example.lan";
    sp_account_id id = 0;
    SP_ASSERT(sp_account_add(c, &cfg, &id) == SP_OK);

    char out[256];
    SP_ASSERT(sp_call_uri_complete(c, id, "123", out, sizeof out) == SP_OK);
    SP_ASSERT_STR(out, "sip:123@pbx.example.lan");

    /* Already a URI: left alone, both schemes. */
    SP_ASSERT(sp_call_uri_complete(c, id, "sip:9@h", out, sizeof out)
              == SP_OK);
    SP_ASSERT_STR(out, "sip:9@h");
    SP_ASSERT(sp_call_uri_complete(c, id, "sips:9@h", out, sizeof out)
              == SP_OK);
    SP_ASSERT_STR(out, "sips:9@h");

    /* user@host with no scheme gets one, and nothing else. */
    SP_ASSERT(sp_call_uri_complete(c, id, "9@other.lan", out, sizeof out)
              == SP_OK);
    SP_ASSERT_STR(out, "sip:9@other.lan");

    /* Surrounding space is not part of anybody's extension. */
    SP_ASSERT(sp_call_uri_complete(c, id, "  123  ", out, sizeof out)
              == SP_OK);
    SP_ASSERT_STR(out, "sip:123@pbx.example.lan");

    SP_ASSERT(sp_call_uri_complete(c, id, "", out, sizeof out)
              == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_call_uri_complete(c, 999999, "123", out, sizeof out)
              == SP_ERR_NOT_FOUND);
    /* Too small to hold the answer is a refusal, not a truncation:
       half a URI dials somebody else. */
    char tiny[8];
    SP_ASSERT(sp_call_uri_complete(c, id, "123", tiny, sizeof tiny)
              == SP_ERR_INVALID_ARG);
    shutdown_core(c);
}

/* The point of this entry point is that the password is not in the
   file, so the test looks in the file. */
SP_TEST(a_password_set_afterwards_is_not_written_to_the_config) {
    static char p[512];
    snprintf(p, sizeof p, "%s/acct-setpw.yaml", getenv("SP_TEST_TMP"));
    FILE *f = fopen(p, "w");
    fputs("log_level: 0\naccounts: []\n", f);
    fclose(f);

    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    SP_ASSERT(sp_core_start(c) == SP_OK);

    /* Added with no password at all, which is what a frontend holding
       the credential elsewhere does. */
    sp_account_cfg cfg = base_cfg("keyring", "1000");
    cfg.auth_user = "authy";
    sp_account_id id = 0;
    SP_ASSERT(sp_account_add(c, &cfg, &id) == SP_OK);
    SP_ASSERT(sp_account_set_password(c, id, "hunter2-not-in-the-file")
              == SP_OK);

    /* The credential has to go in under the account's auth user.
       Taken from the AOR instead it reads "1000@127.0.0.1", which
       every registrar answers 401 to for ever -- and nothing else in
       this test would have noticed, because the add and the set both
       still return SP_OK. */
    char who[128] = {0};
    SP_ASSERT(sp_account_auth_user(c, id, who, sizeof who) == SP_OK);
    SP_ASSERT_STR(who, "authy");

    /* And with no auth_user, the plain user -- never user@domain. */
    sp_account_cfg plain = base_cfg("plainuser", "2000");
    sp_account_id pid = 0;
    SP_ASSERT(sp_account_add(c, &plain, &pid) == SP_OK);
    SP_ASSERT(sp_account_set_password(c, pid, "another") == SP_OK);
    SP_ASSERT(sp_account_auth_user(c, pid, who, sizeof who) == SP_OK);
    SP_ASSERT_STR(who, "2000");
    SP_ASSERT(sp_account_remove(c, pid) == SP_OK);
    /* Still usable afterwards. */
    sp_account_state st;
    SP_ASSERT(sp_account_get_state(c, id, &st) == SP_OK);
    /* Clearing it is allowed, and so is setting it again. */
    SP_ASSERT(sp_account_set_password(c, id, NULL) == SP_OK);
    SP_ASSERT(sp_account_set_password(c, id, "hunter2-not-in-the-file")
              == SP_OK);
    SP_ASSERT(sp_account_set_password(c, 999999, "x") == SP_ERR_NOT_FOUND);
    shutdown_core(c);

    char buf[8192];
    f = fopen(p, "r");
    SP_ASSERT(f != NULL);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    SP_ASSERT(strstr(buf, "hunter2-not-in-the-file") == NULL);
    /* The account itself did persist -- which is the difference from
       sp_account_add_transient, and the whole reason this exists. */
    SP_ASSERT(strstr(buf, "keyring") != NULL);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(adding_an_account_yields_a_nonzero_handle);
    SP_RUN(two_accounts_get_distinct_handles);
    SP_RUN(duplicate_account_id_is_rejected);
    SP_RUN(mandatory_srtp_account_is_accepted_and_persisted);
    SP_RUN(missing_required_fields_are_rejected);
    SP_RUN(removed_account_handle_is_retired);
    SP_RUN(account_id_may_be_reused_after_removal);
    SP_RUN(state_of_an_unregistered_account_reads_as_not_registered);
    SP_RUN(accounts_from_config_are_added_at_start);
    SP_RUN(added_account_is_written_to_the_config_file);
    SP_RUN(removed_account_is_erased_from_the_config_file);
    SP_RUN(registration_toggle_is_persisted);
    SP_RUN(busy_registration_change_is_a_state_error_and_still_persists);
    SP_RUN(renaming_an_account_carries_the_rules_scoped_to_it);
    SP_RUN(an_account_added_with_no_expiry_still_reloads);
    SP_RUN(an_out_of_range_expiry_is_refused_rather_than_written);
    SP_RUN(a_transient_account_is_not_written_to_the_config);
    SP_RUN(validation_names_the_field_that_is_wrong);
    SP_RUN(updating_an_account_keeps_its_handle_live_and_carries_rules);
    SP_RUN(a_rejected_update_leaves_the_original_account_in_place);
    SP_RUN(a_bare_extension_is_completed_with_the_account_domain);
    SP_RUN(a_password_set_afterwards_is_not_written_to_the_config);
    return SP_TEST_RESULT();
}
