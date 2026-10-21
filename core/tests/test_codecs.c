#include "sp_test.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *write_tmp(const char *body) {
    static char path[512];
    snprintf(path, sizeof path, "%s/codec_test.yaml", getenv("SP_TEST_TMP"));
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

SP_TEST(a_codecs_list_is_read_in_order) {
    sp_config *c = load_ok("accounts: []\n"
                           "codecs:\n"
                           "  - id: opus\n"
                           "  - id: G722\n"
                           "  - id: PCMU\n");
    if (!c) { SP_ASSERT(0); return; }
    SP_ASSERT(c->n_codecs == 3);
    SP_ASSERT(strcmp(c->codecs[0].id, "opus") == 0);
    SP_ASSERT(strcmp(c->codecs[1].id, "G722") == 0);
    SP_ASSERT(strcmp(c->codecs[2].id, "PCMU") == 0);
    /* Order is priority: first in the list is offered first. */
    SP_ASSERT(c->codecs[0].priority > c->codecs[1].priority);
    SP_ASSERT(c->codecs[1].priority > c->codecs[2].priority);
    sp_config_free(c);
}

SP_TEST(an_absent_parameter_means_the_codecs_own_default) {
    sp_config *c = load_ok("accounts: []\ncodecs:\n  - id: G722\n");
    if (!c) { SP_ASSERT(0); return; }
    SP_ASSERT(c->codecs[0].enabled == true);
    SP_ASSERT(c->codecs[0].bitrate == -1);
    SP_ASSERT(c->codecs[0].complexity == -1);
    SP_ASSERT(c->codecs[0].expected_loss_pct == -1);
    SP_ASSERT(c->codecs[0].ptime_ms == -1);
    SP_ASSERT(c->codecs[0].cbr == -1);
    sp_config_free(c);
}

SP_TEST(parameters_are_read_when_given) {
    sp_config *c = load_ok("accounts: []\n"
                           "codecs:\n"
                           "  - id: opus\n"
                           "    bitrate: 24000\n"
                           "    complexity: 8\n"
                           "    expected_loss_pct: 5\n"
                           "    ptime_ms: 40\n"
                           "    cbr: true\n"
                           "  - id: PCMU\n"
                           "    enabled: false\n");
    if (!c) { SP_ASSERT(0); return; }
    SP_ASSERT(c->codecs[0].bitrate == 24000);
    SP_ASSERT(c->codecs[0].complexity == 8);
    SP_ASSERT(c->codecs[0].expected_loss_pct == 5);
    SP_ASSERT(c->codecs[0].ptime_ms == 40);
    SP_ASSERT(c->codecs[0].cbr == 1);
    /* Disabled is priority zero, which is how pjsua says "never". */
    SP_ASSERT(c->codecs[1].enabled == false);
    SP_ASSERT(c->codecs[1].priority == 0);
    sp_config_free(c);
}

SP_TEST(codec_priority_migrates_to_the_codecs_list) {
    sp_config *c = load_ok("accounts: []\n"
                           "codec_priority: [opus, G722, PCMU]\n");
    if (!c) { SP_ASSERT(0); return; }
    SP_ASSERT(c->n_codecs == 3);
    SP_ASSERT(strcmp(c->codecs[0].id, "opus") == 0);
    SP_ASSERT(strcmp(c->codecs[2].id, "PCMU") == 0);
    SP_ASSERT(c->codecs[0].enabled == true);
    SP_ASSERT(c->codecs[0].bitrate == -1);
    SP_ASSERT(c->codecs[0].priority > c->codecs[2].priority);
    sp_config_free(c);
}

SP_TEST(both_keys_at_once_is_refused_rather_than_guessed) {
    char err[256] = {0};
    SP_ASSERT(load_err("accounts: []\n"
                       "codec_priority: [opus]\n"
                       "codecs:\n  - id: G722\n", err, sizeof err)
              == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "codec_priority") != NULL);
    SP_ASSERT(strstr(err, "codecs") != NULL);
}

SP_TEST(a_codec_entry_without_an_id_is_refused) {
    char err[256] = {0};
    SP_ASSERT(load_err("accounts: []\ncodecs:\n  - bitrate: 24000\n",
                       err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "id") != NULL);
}

SP_TEST(an_unknown_codec_key_is_refused_naming_itself) {
    char err[256] = {0};
    SP_ASSERT(load_err("accounts: []\ncodecs:\n  - id: opus\n    bitrote: 1\n",
                       err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "bitrote") != NULL);
}

SP_TEST(out_of_range_parameters_are_refused_naming_the_value) {
    char err[256] = {0};
    SP_ASSERT(load_err("accounts: []\ncodecs:\n  - id: opus\n"
                       "    complexity: 11\n", err, sizeof err)
              == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "complexity") != NULL);

    err[0] = 0;
    SP_ASSERT(load_err("accounts: []\ncodecs:\n  - id: opus\n"
                       "    expected_loss_pct: 101\n", err, sizeof err)
              == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "expected_loss_pct") != NULL);

    err[0] = 0;
    SP_ASSERT(load_err("accounts: []\ncodecs:\n  - id: opus\n"
                       "    ptime_ms: 0\n", err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "ptime_ms") != NULL);
}

SP_TEST(the_same_codec_twice_is_refused) {
    char err[256] = {0};
    SP_ASSERT(load_err("accounts: []\ncodecs:\n  - id: opus\n  - id: opus\n",
                       err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "opus") != NULL);
}

SP_TEST(a_codecs_list_survives_a_save_and_reload) {
    sp_config *c = load_ok("accounts: []\n"
                           "codecs:\n"
                           "  - id: opus\n    bitrate: 16000\n"
                           "    expected_loss_pct: 10\n"
                           "  - id: PCMA\n    enabled: false\n");
    if (!c) { SP_ASSERT(0); return; }
    char out[512];
    snprintf(out, sizeof out, "%s/codec_rt.yaml", getenv("SP_TEST_TMP"));
    char serr[256] = {0};
    SP_ASSERT(sp_config_save(c, out, serr, sizeof serr) == SP_OK);
    sp_config_free(c);

    sp_config *b = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(out, &b, err, sizeof err) == SP_OK);
    if (!b) { printf("  %s\n", err); SP_ASSERT(0); return; }
    SP_ASSERT(b->n_codecs == 2);
    SP_ASSERT(strcmp(b->codecs[0].id, "opus") == 0);
    SP_ASSERT(b->codecs[0].bitrate == 16000);
    SP_ASSERT(b->codecs[0].expected_loss_pct == 10);
    SP_ASSERT(b->codecs[1].enabled == false);
    /* What was never set stays unset rather than being written out as
       a value the operator did not choose. */
    SP_ASSERT(b->codecs[0].complexity == -1);
    sp_config_free(b);
}

SP_TEST(a_migrated_config_is_written_back_as_codecs) {
    sp_config *c = load_ok("accounts: []\ncodec_priority: [opus, PCMU]\n");
    if (!c) { SP_ASSERT(0); return; }
    char out[512];
    snprintf(out, sizeof out, "%s/codec_mig.yaml", getenv("SP_TEST_TMP"));
    char serr[256] = {0};
    SP_ASSERT(sp_config_save(c, out, serr, sizeof serr) == SP_OK);
    sp_config_free(c);

    FILE *f = fopen(out, "r");
    SP_ASSERT(f != NULL);
    if (!f) return;
    char buf[8192] = {0};
    const size_t got = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    SP_ASSERT(got > 0);
    SP_ASSERT(strstr(buf, "codecs:") != NULL);
    SP_ASSERT(strstr(buf, "codec_priority") == NULL);
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

SP_TEST(the_stack_reports_what_each_codec_supports) {
    sp_core *c = start_core("codec_caps", "accounts: []\n");
    if (!c) { SP_ASSERT(0); return; }
    const sp_codec_params *v = NULL;
    size_t n = 0;
    SP_ASSERT(sp_media_get_codecs(c, &v, &n) == SP_OK);
    SP_ASSERT(n > 0);

    bool saw_opus = false;
    for (size_t i = 0; i < n; i++) {
        SP_ASSERT(v[i].id && *v[i].id);
        SP_ASSERT(v[i].clock_rate > 0);
        /* Whatever the stack said, ptime has to have a frame to be a
           multiple of, or the control cannot be offered honestly. */
        if (v[i].supports_ptime)
            SP_ASSERT(v[i].frame_ptime_ms > 0);
        if (strncasecmp(v[i].id, "opus", 4) == 0) {
            saw_opus = true;
            SP_ASSERT(v[i].supports_bitrate == true);
        } else {
            SP_ASSERT(v[i].supports_bitrate == false);
        }
    }
    /* pjproject is built with opus here; if that ever stops being
       true this says so rather than silently testing nothing. */
    SP_ASSERT(saw_opus);
    sp_core_destroy(c);
}

SP_TEST(setting_codecs_persists_across_a_restart) {
    sp_core *c = start_core("codec_set", "accounts: []\n");
    if (!c) { SP_ASSERT(0); return; }

    sp_codec_params want[2] = {0};
    want[0].id = "PCMA";
    want[0].enabled = true;
    want[0].bitrate = -1; want[0].complexity = -1;
    want[0].expected_loss_pct = -1; want[0].ptime_ms = -1; want[0].cbr = -1;
    want[1].id = "PCMU";
    want[1].enabled = false;
    want[1].bitrate = -1; want[1].complexity = -1;
    want[1].expected_loss_pct = -1; want[1].ptime_ms = -1; want[1].cbr = -1;

    SP_ASSERT(sp_media_set_codecs(c, want, 2) == SP_OK);

    const sp_codec_params *v = NULL;
    size_t n = 0;
    SP_ASSERT(sp_media_get_codecs(c, &v, &n) == SP_OK);
    for (size_t i = 0; i < n; i++) {
        if (strncasecmp(v[i].id, "PCMA", 4) == 0)
            SP_ASSERT(v[i].enabled == true);
        if (strncasecmp(v[i].id, "PCMU", 4) == 0)
            SP_ASSERT(v[i].enabled == false);
    }
    sp_core_destroy(c);

    /* And it is on disk, not just in memory. */
    static char path[512];
    snprintf(path, sizeof path, "%s/codec_set.yaml", getenv("SP_TEST_TMP"));
    sp_config *cfg = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(path, &cfg, err, sizeof err) == SP_OK);
    if (!cfg) { printf("  %s\n", err); SP_ASSERT(0); return; }
    SP_ASSERT(cfg->n_codecs == 2);
    SP_ASSERT_STR(cfg->codecs[0].id, "PCMA");
    SP_ASSERT(cfg->codecs[1].enabled == false);
    sp_config_free(cfg);
}

SP_TEST(a_refused_list_changes_nothing) {
    sp_core *c = start_core("codec_bad", "accounts: []\ncodecs:\n  - id: PCMU\n");
    if (!c) { SP_ASSERT(0); return; }

    /* Bitrate on a codec that has no such setting. */
    sp_codec_params bad[1] = {0};
    bad[0].id = "PCMA";
    bad[0].enabled = true;
    bad[0].bitrate = 24000;
    bad[0].complexity = -1; bad[0].expected_loss_pct = -1;
    bad[0].ptime_ms = -1; bad[0].cbr = -1;
    SP_ASSERT(sp_media_set_codecs(c, bad, 1) == SP_ERR_INVALID_ARG);

    /* The same codec twice. */
    sp_codec_params dup[2] = {0};
    for (int i = 0; i < 2; i++) {
        dup[i].id = "PCMU";
        dup[i].enabled = true;
        dup[i].bitrate = -1; dup[i].complexity = -1;
        dup[i].expected_loss_pct = -1; dup[i].ptime_ms = -1; dup[i].cbr = -1;
    }
    SP_ASSERT(sp_media_set_codecs(c, dup, 2) == SP_ERR_INVALID_ARG);
    sp_core_destroy(c);

    /* The config still says what it said before either attempt. */
    static char path[512];
    snprintf(path, sizeof path, "%s/codec_bad.yaml", getenv("SP_TEST_TMP"));
    sp_config *cfg = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(path, &cfg, err, sizeof err) == SP_OK);
    if (!cfg) return;
    SP_ASSERT(cfg->n_codecs == 1);
    SP_ASSERT_STR(cfg->codecs[0].id, "PCMU");
    sp_config_free(cfg);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(a_codecs_list_is_read_in_order);
    SP_RUN(an_absent_parameter_means_the_codecs_own_default);
    SP_RUN(parameters_are_read_when_given);
    SP_RUN(codec_priority_migrates_to_the_codecs_list);
    SP_RUN(both_keys_at_once_is_refused_rather_than_guessed);
    SP_RUN(a_codec_entry_without_an_id_is_refused);
    SP_RUN(an_unknown_codec_key_is_refused_naming_itself);
    SP_RUN(out_of_range_parameters_are_refused_naming_the_value);
    SP_RUN(the_same_codec_twice_is_refused);
    SP_RUN(a_codecs_list_survives_a_save_and_reload);
    SP_RUN(a_migrated_config_is_written_back_as_codecs);
    SP_RUN(the_stack_reports_what_each_codec_supports);
    SP_RUN(setting_codecs_persists_across_a_restart);
    SP_RUN(a_refused_list_changes_nothing);
    return SP_TEST_RESULT();
}
