#include "sp_test.h"
#include "config.h"
#include "modules.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *write_tmp(const char *body) {
    static char path[512];
    snprintf(path, sizeof path, "%s/module_test.yaml", getenv("SP_TEST_TMP"));
    FILE *f = fopen(path, "w");
    if (!f) return NULL;
    fputs(body, f);
    fclose(f);
    return path;
}

static sp_config *load_ok(const char *body) {
    sp_config *c = NULL; char err[256] = {0};
    if (sp_config_load(write_tmp(body), &c, err, sizeof err) != SP_OK) {
        printf("  unexpected load failure: %s\n", err);
        return NULL;
    }
    return c;
}

static sp_status load_err(const char *body, char *err, size_t errlen) {
    sp_config *c = NULL;
    sp_status st = sp_config_load(write_tmp(body), &c, err, errlen);
    if (c) sp_config_free(c);
    return st;
}

SP_TEST(absent_modules_means_each_module_at_its_default) {
    /* A config written before modules existed describes a phone with
       everything it had then, and has to keep meaning that; a module
       added since comes up the way it was introduced. */
    sp_config *c = load_ok("accounts: []\n");
    if (!c) { SP_ASSERT(0); return; }
    for (int i = 0; i < sp_module_count(); i++)
        SP_ASSERT(sp_config_module_enabled(c, sp_module_id_at(i))
                  == sp_module_default_at(i));
    SP_ASSERT(sp_config_module_enabled(c, "recording") == true);
    SP_ASSERT(sp_config_module_enabled(c, "transcript") == false);
    sp_config_free(c);
}

SP_TEST(a_default_off_module_can_be_switched_on) {
    sp_config *c = load_ok("accounts: []\nmodules:\n  transcript: true\n");
    if (!c) { SP_ASSERT(0); return; }
    SP_ASSERT(sp_config_module_enabled(c, "transcript") == true);
    sp_config_free(c);
}

SP_TEST(a_module_switched_on_survives_a_save_and_reload) {
    sp_config *c = load_ok("accounts: []\nmodules:\n  transcript: true\n");
    if (!c) { SP_ASSERT(0); return; }
    char out[512], serr[256] = {0};
    snprintf(out, sizeof out, "%s/module_on.yaml", getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_config_save(c, out, serr, sizeof serr) == SP_OK);
    sp_config_free(c);

    sp_config *b = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(out, &b, err, sizeof err) == SP_OK);
    if (!b) { printf("  %s\n", err); SP_ASSERT(0); return; }
    SP_ASSERT(sp_config_module_enabled(b, "transcript") == true);
    SP_ASSERT(sp_config_module_enabled(b, "recording") == true);
    sp_config_free(b);
}

SP_TEST(the_base_is_listed_and_cannot_be_removed) {
    bool saw_core = false;
    for (int i = 0; i < sp_module_count(); i++) {
        const char *id = sp_module_id_at(i);
        SP_ASSERT(id && *id);
        SP_ASSERT(sp_module_label_at(i) && *sp_module_label_at(i));
        if (strcmp(id, "sip_core") == 0) {
            saw_core = true;
            SP_ASSERT(sp_module_removable_at(i) == false);
        } else {
            SP_ASSERT(sp_module_removable_at(i) == true);
        }
    }
    SP_ASSERT(saw_core);
}

SP_TEST(a_module_can_be_switched_off_in_config) {
    sp_config *c = load_ok("accounts: []\nmodules:\n  recording: false\n");
    if (!c) { SP_ASSERT(0); return; }
    SP_ASSERT(sp_config_module_enabled(c, "recording") == false);
    SP_ASSERT(sp_config_module_enabled(c, "audio_quality") == true);
    SP_ASSERT(sp_config_module_enabled(c, "sip_trace") == true);
    sp_config_free(c);
}

SP_TEST(an_unknown_module_is_refused_naming_itself) {
    char err[256] = {0};
    SP_ASSERT(load_err("accounts: []\nmodules:\n  teleporter: false\n",
                       err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "teleporter") != NULL);
}

SP_TEST(switching_off_the_base_is_refused) {
    char err[256] = {0};
    SP_ASSERT(load_err("accounts: []\nmodules:\n  sip_core: false\n",
                       err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "sip_core") != NULL);
}

SP_TEST(sip_log_migrates_to_the_sip_trace_module) {
    sp_config *c = load_ok("accounts: []\nsip_log: false\n");
    if (!c) { SP_ASSERT(0); return; }
    SP_ASSERT(sp_config_module_enabled(c, "sip_trace") == false);
    sp_config_free(c);

    sp_config *on = load_ok("accounts: []\nsip_log: true\n");
    if (!on) { SP_ASSERT(0); return; }
    SP_ASSERT(sp_config_module_enabled(on, "sip_trace") == true);
    sp_config_free(on);
}

SP_TEST(a_zero_metrics_interval_migrates_to_audio_quality_off) {
    sp_config *c = load_ok("accounts: []\nmetrics_interval_ms: 0\n");
    if (!c) { SP_ASSERT(0); return; }
    SP_ASSERT(sp_config_module_enabled(c, "audio_quality") == false);
    /* The interval keeps its real job: the sampling rate, which now
       falls back to the default rather than meaning "off". */
    SP_ASSERT(c->metrics_interval_ms > 0);
    sp_config_free(c);
}

SP_TEST(both_spellings_at_once_are_refused) {
    char err[256] = {0};
    SP_ASSERT(load_err("accounts: []\nsip_log: false\n"
                       "modules:\n  sip_trace: true\n", err, sizeof err)
              == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "sip_log") != NULL);
    SP_ASSERT(strstr(err, "sip_trace") != NULL);
}

SP_TEST(modules_survive_a_save_and_reload) {
    sp_config *c = load_ok("accounts: []\n"
                           "modules:\n  recording: false\n"
                           "  audio_quality: false\n");
    if (!c) { SP_ASSERT(0); return; }
    char out[512], serr[256] = {0};
    snprintf(out, sizeof out, "%s/module_rt.yaml", getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_config_save(c, out, serr, sizeof serr) == SP_OK);
    sp_config_free(c);

    sp_config *b = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(out, &b, err, sizeof err) == SP_OK);
    if (!b) { printf("  %s\n", err); SP_ASSERT(0); return; }
    SP_ASSERT(sp_config_module_enabled(b, "recording") == false);
    SP_ASSERT(sp_config_module_enabled(b, "audio_quality") == false);
    SP_ASSERT(sp_config_module_enabled(b, "sip_trace") == true);
    sp_config_free(b);
}

SP_TEST(a_migrated_config_is_written_back_as_modules) {
    sp_config *c = load_ok("accounts: []\nsip_log: false\n");
    if (!c) { SP_ASSERT(0); return; }
    char out[512], serr[256] = {0};
    snprintf(out, sizeof out, "%s/module_mig.yaml", getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_config_save(c, out, serr, sizeof serr) == SP_OK);
    sp_config_free(c);

    FILE *f = fopen(out, "r");
    SP_ASSERT(f != NULL);
    if (!f) return;
    char buf[8192] = {0};
    const size_t got = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    SP_ASSERT(got > 0);
    SP_ASSERT(strstr(buf, "modules:") != NULL);
    SP_ASSERT(strstr(buf, "sip_trace: false") != NULL);
    SP_ASSERT(strstr(buf, "sip_log:") == NULL);
}

/* --- against a running core ------------------------------------------ */

static sp_core *start_core(const char *name, const char *body) {
    static char path[512];
    char db[512];
    snprintf(path, sizeof path, "%s/%s.yaml", getenv("SP_TEST_TMP"), name);
    snprintf(db, sizeof db, "%s/%s.db", getenv("SP_TEST_TMP"), name);
    unlink(db);
    FILE *f = fopen(path, "w");
    if (!f) return NULL;
    fprintf(f, "log_level: 0\nsip_port: 0\naudio_device: none\n"
               "db_file: %s\n%s", db, body);
    fclose(f);
    sp_core *c = NULL;
    if (sp_core_create(path, &c) != SP_OK)
        return NULL;
    if (sp_core_start(c) != SP_OK) {
        printf("  core would not start: %s\n", sp_last_error_detail(c));
        sp_core_destroy(c);
        return NULL;
    }
    return c;
}

SP_TEST(a_disabled_module_refuses_rather_than_doing_nothing) {
    sp_core *c = start_core("mod_off",
                            "accounts: []\nmodules:\n"
                            "  recording: false\n  audio_quality: false\n");
    if (!c) { SP_ASSERT(0); return; }

    /* No call exists, so the handle check would fail anyway -- the
       point is which error comes back. Disabled has to win, because
       the module is the reason it will never work, and a caller told
       "handle not found" would go looking for the wrong problem. */
    char out[512];
    snprintf(out, sizeof out, "%s/should_not_appear.wav",
             getenv("SP_TEST_TMP"));
    unlink(out);
    SP_ASSERT(sp_call_record_start(c, 1, out) == SP_ERR_DISABLED);
    SP_ASSERT(strstr(sp_last_error_detail(c), "recording") != NULL);
    SP_ASSERT(access(out, F_OK) != 0);

    SP_ASSERT(sp_call_record_stop(c, 1) == SP_ERR_DISABLED);

    sp_call_quality q;
    SP_ASSERT(sp_call_get_quality(c, 1, &q) == SP_ERR_DISABLED);
    SP_ASSERT(strstr(sp_last_error_detail(c), "quality") != NULL);
    sp_core_destroy(c);
}

SP_TEST(an_enabled_module_does_not_refuse) {
    sp_core *c = start_core("mod_on", "accounts: []\n");
    if (!c) { SP_ASSERT(0); return; }
    /* Not SP_OK -- there is no such call -- but not DISABLED either,
       which is what proves the gate is the module and not the
       default. */
    SP_ASSERT(sp_call_record_stop(c, 1) != SP_ERR_DISABLED);
    sp_call_quality q;
    SP_ASSERT(sp_call_get_quality(c, 1, &q) != SP_ERR_DISABLED);
    sp_core_destroy(c);
}

SP_TEST(switching_a_module_off_deletes_nothing) {
    /* The rule the registry keeps for every module, present and
       future: a toggle that destroys data is a trap, not a toggle. */
    static char path[512];
    char db[512];
    snprintf(path, sizeof path, "%s/mod_keep.yaml", getenv("SP_TEST_TMP"));
    snprintf(db, sizeof db, "%s/mod_keep.db", getenv("SP_TEST_TMP"));
    unlink(db);
    FILE *f = fopen(path, "w");
    if (!f) { SP_ASSERT(0); return; }
    fprintf(f, "log_level: 0\nsip_port: 0\naudio_device: none\n"
               "db_file: %s\naccounts: []\n", db);
    fclose(f);

    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(path, &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    sp_core_destroy(c);
    SP_ASSERT(access(db, F_OK) == 0);   /* the history exists */

    /* Now start again with everything switched off. */
    f = fopen(path, "w");
    fprintf(f, "log_level: 0\nsip_port: 0\naudio_device: none\n"
               "db_file: %s\naccounts: []\nmodules:\n"
               "  recording: false\n  audio_quality: false\n"
               "  sip_trace: false\n", db);
    fclose(f);
    sp_core *c2 = NULL;
    SP_ASSERT(sp_core_create(path, &c2) == SP_OK);
    if (!c2) return;
    SP_ASSERT(sp_core_start(c2) == SP_OK);
    sp_core_destroy(c2);

    SP_ASSERT(access(db, F_OK) == 0);   /* and still exists */
}

SP_TEST(the_abi_reports_and_changes_the_registry) {
    sp_core *c = start_core("mod_abi", "accounts: []\n");
    if (!c) { SP_ASSERT(0); return; }

    const sp_module *v = NULL;
    size_t n = 0;
    SP_ASSERT(sp_modules_get(c, &v, &n) == SP_OK);
    SP_ASSERT(n == (size_t)sp_module_count());
    for (size_t i = 0; i < n; i++) {
        SP_ASSERT(v[i].id && *v[i].id);
        SP_ASSERT(v[i].label && *v[i].label);
        SP_ASSERT(v[i].enabled == sp_module_default_at((int)i));
    }

    SP_ASSERT(sp_module_set(c, "recording", false) == SP_OK);
    SP_ASSERT(sp_module_is_enabled(c, "recording") == false);
    SP_ASSERT(sp_module_is_enabled(c, "sip_trace") == true);

    /* And it took effect, not just recorded. */
    SP_ASSERT(sp_call_record_stop(c, 1) == SP_ERR_DISABLED);

    SP_ASSERT(sp_module_set(c, "nosuchmodule", false) == SP_ERR_NOT_FOUND);
    SP_ASSERT(sp_module_set(c, "sip_core", false) == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_module_is_enabled(c, "sip_core") == true);
    sp_core_destroy(c);

    /* Persisted, not only held in memory. */
    static char path[512];
    snprintf(path, sizeof path, "%s/mod_abi.yaml", getenv("SP_TEST_TMP"));
    sp_config *cfg = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(path, &cfg, err, sizeof err) == SP_OK);
    if (!cfg) { printf("  %s\n", err); SP_ASSERT(0); return; }
    SP_ASSERT(sp_config_module_enabled(cfg, "recording") == false);
    sp_config_free(cfg);
}

/* Registering at a port with nothing behind it still puts a REGISTER
   on the wire, which is enough to exercise the capture path without
   a PBX or a network. */
#define SP_DEAD_REGISTRAR \
    "accounts:\n  - id: t\n    user: 1000\n    domain: 127.0.0.1\n" \
    "    registrar: sip:127.0.0.1:9\n    password: x\n    register: true\n"

static size_t captured_messages(sp_core *c) {
    const sp_sip_message *m = NULL;
    size_t n = 0;
    if (sp_siplog_read(c, NULL, 100, &m, &n) != SP_OK)
        return 0;
    return n;
}

SP_TEST(sip_trace_on_captures_and_off_does_not) {
    /* The bug this guards: the log object was created at startup
       according to the module, so switching the module off left the
       capture running and only a restart stopped it. */
    sp_core *on = start_core("mod_trace_on", SP_DEAD_REGISTRAR);
    if (!on) { SP_ASSERT(0); return; }
    for (int i = 0; i < 40 && captured_messages(on) == 0; i++)
        usleep(50000);
    const size_t with_module_on = captured_messages(on);
    SP_ASSERT(with_module_on > 0);

    /* Now switch it off on a running core and let it try again. */
    SP_ASSERT(sp_module_set(on, "sip_trace", false) == SP_OK);
    SP_ASSERT(sp_siplog_clear(on) == SP_OK);
    for (int i = 0; i < 20; i++)
        usleep(50000);
    SP_ASSERT(captured_messages(on) == 0);

    /* And back on again, so the gate is the module either way. */
    SP_ASSERT(sp_module_set(on, "sip_trace", true) == SP_OK);
    for (int i = 0; i < 40 && captured_messages(on) == 0; i++)
        usleep(50000);
    SP_ASSERT(captured_messages(on) > 0);
    sp_core_destroy(on);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(absent_modules_means_each_module_at_its_default);
    SP_RUN(a_default_off_module_can_be_switched_on);
    SP_RUN(a_module_switched_on_survives_a_save_and_reload);
    SP_RUN(the_base_is_listed_and_cannot_be_removed);
    SP_RUN(a_module_can_be_switched_off_in_config);
    SP_RUN(an_unknown_module_is_refused_naming_itself);
    SP_RUN(switching_off_the_base_is_refused);
    SP_RUN(sip_log_migrates_to_the_sip_trace_module);
    SP_RUN(a_zero_metrics_interval_migrates_to_audio_quality_off);
    SP_RUN(both_spellings_at_once_are_refused);
    SP_RUN(modules_survive_a_save_and_reload);
    SP_RUN(a_migrated_config_is_written_back_as_modules);
    SP_RUN(a_disabled_module_refuses_rather_than_doing_nothing);
    SP_RUN(an_enabled_module_does_not_refuse);
    SP_RUN(switching_a_module_off_deletes_nothing);
    SP_RUN(the_abi_reports_and_changes_the_registry);
    SP_RUN(sip_trace_on_captures_and_off_does_not);
    return SP_TEST_RESULT();
}
