#include "sp_test.h"
#include "config.h"
#include "yaml_tree.h"
#include "modules.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *write_tmp(const char *body) {
    static char path[512];
    snprintf(path, sizeof path, "%s/sp_cfg_test.yaml", getenv("SP_TEST_TMP"));
    FILE *f = fopen(path, "w");
    if (!f) return NULL;
    fputs(body, f);
    fclose(f);
    return path;
}

SP_TEST(loads_a_single_account) {
    const char *p = write_tmp(
        "accounts:\n"
        "  - id: work\n"
        "    user: 1001\n"
        "    domain: pbx.example.lan\n"
        "    password: secret\n"
        "    transport: tls\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_OK);
    SP_ASSERT(c != NULL);
    if (!c) return;
    SP_ASSERT(c->n_accounts == 1);
    SP_ASSERT_STR(c->accounts[0].id, "work");
    SP_ASSERT_STR(c->accounts[0].user, "1001");
    SP_ASSERT_STR(c->accounts[0].domain, "pbx.example.lan");
    SP_ASSERT(c->accounts[0].transport == SP_TP_TLS);
    sp_config_free(c);
}

SP_TEST(transport_names_map_to_all_three_enums) {
    const struct { const char *name; sp_transport want; } cases[] = {
        {"udp", SP_TP_UDP}, {"tcp", SP_TP_TCP}, {"tls", SP_TP_TLS}
    };
    for (size_t i = 0; i < 3; i++) {
        char body[512];
        snprintf(body, sizeof body,
                 "accounts:\n  - id: a\n    user: u\n    domain: d\n"
                 "    transport: %s\n", cases[i].name);
        sp_config *c = NULL; char err[256] = {0};
        SP_ASSERT(sp_config_load(write_tmp(body), &c, err, sizeof err) == SP_OK);
        if (!c) continue;
        SP_ASSERT(c->accounts[0].transport == cases[i].want);
        sp_config_free(c);
    }
}

SP_TEST(unknown_transport_is_a_config_error_naming_the_value) {
    const char *p = write_tmp(
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"
        "    transport: sctp\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(c == NULL);
    SP_ASSERT(strstr(err, "sctp") != NULL);
    SP_ASSERT(strstr(err, "transport") != NULL);
}

/* Spec: unknown keys are a hard error, never a warning. A typo in a
   header rule that gets silently ignored is the failure mode this
   project cannot tolerate. */
SP_TEST(unknown_account_key_is_a_hard_error) {
    const char *p = write_tmp(
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"
        "    passwrod: oops\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "passwrod") != NULL);
}

/* This once asserted a hard error, until refusing unknown
   top-level keys was found to break rollback: a 0.6.0 would not start
   on a config a newer build had written "recording:" into. The typo
   case the old rule existed for is still caught -- loudly, by name,
   on every start -- but it no longer stops the phone. What it costs
   is asserted here rather than left implicit: a misspelt key means
   that setting is silently absent, so log_level falls back to its
   default instead of taking the 3 the operator meant. */
SP_TEST(a_misspelt_top_level_key_warns_and_leaves_its_setting_unset) {
    const char *p = write_tmp("logg_level: 5\naccounts: []\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_OK);
    SP_ASSERT(c != NULL);
    if (!c) return;
    SP_ASSERT(c->log_level == 3);   /* the default, not the 5 meant */
    sp_config_free(c);
}

SP_TEST(missing_required_account_key_is_an_error) {
    const char *p = write_tmp("accounts:\n  - id: a\n    domain: d\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "user") != NULL);
}

SP_TEST(duplicate_account_id_is_an_error) {
    const char *p = write_tmp(
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"
        "  - id: a\n    user: v\n    domain: e\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "a") != NULL);
}

SP_TEST(all_three_srtp_modes_parse) {
    const struct { const char *name; sp_srtp_mode want; } cases[] = {
        {"disabled", SP_SRTP_DISABLED},
        {"optional", SP_SRTP_OPTIONAL},
        {"mandatory", SP_SRTP_MANDATORY}
    };
    for (size_t i = 0; i < 3; i++) {
        char body[512];
        snprintf(body, sizeof body,
                 "accounts:\n  - id: a\n    user: u\n    domain: d\n"
                 "    srtp: %s\n", cases[i].name);
        sp_config *c = NULL; char err[256] = {0};
        SP_ASSERT(sp_config_load(write_tmp(body), &c, err, sizeof err) == SP_OK);
        if (!c) { printf("  %s\n", err); continue; }
        SP_ASSERT(c->accounts[0].srtp == cases[i].want);
        sp_config_free(c);
    }
}

/* Default is disabled, not optional: switching it on for every account
   would silently change what every existing config offers. */
SP_TEST(srtp_defaults_to_disabled) {
    const char *p = write_tmp("accounts:\n  - id: a\n    user: u\n    domain: d\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT(c->accounts[0].srtp == SP_SRTP_DISABLED);
    sp_config_free(c);
}

SP_TEST(unknown_srtp_mode_is_rejected_naming_the_value) {
    const char *p = write_tmp(
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"
        "    srtp: paranoid\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "paranoid") != NULL);
    SP_ASSERT(strstr(err, "srtp") != NULL);
}

SP_TEST(srtp_mode_survives_a_save) {
    const char *p = write_tmp(
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"
        "    srtp: mandatory\n");
    sp_config *a = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &a, err, sizeof err) == SP_OK);
    if (!a) return;
    char out[512];
    snprintf(out, sizeof out, "%s/srtp_rt.yaml", getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_config_save(a, out, err, sizeof err) == SP_OK);
    sp_config *b = NULL;
    SP_ASSERT(sp_config_load(out, &b, err, sizeof err) == SP_OK);
    if (b) {
        SP_ASSERT(b->accounts[0].srtp == SP_SRTP_MANDATORY);
        sp_config_free(b);
    }
    sp_config_free(a);
}

SP_TEST(defaults_apply_when_optional_keys_absent) {
    const char *p = write_tmp("accounts:\n  - id: a\n    user: u\n    domain: d\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT(c->accounts[0].transport == SP_TP_UDP);
    SP_ASSERT(c->accounts[0].reg_expires_s == 300);
    SP_ASSERT(c->accounts[0].do_register == true);
    SP_ASSERT(c->accounts[0].stun_server == NULL);
    /* auth_user defaults to user rather than staying empty. */
    SP_ASSERT_STR(c->accounts[0].auth_user, "u");
    SP_ASSERT(c->log_level == 3);
    SP_ASSERT(c->sip_port == 0);
    sp_config_free(c);
}

SP_TEST(register_false_is_honored) {
    const char *p = write_tmp(
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"
        "    register: false\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT(c->accounts[0].do_register == false);
    sp_config_free(c);
}

SP_TEST(sip_port_is_read) {
    const char *p = write_tmp("sip_port: 5080\naccounts: []\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT(c->sip_port == 5080);
    sp_config_free(c);
}

SP_TEST(codec_priority_preserves_order) {
    const char *p = write_tmp(
        "codec_priority: [opus, G722, PCMU]\n"
        "accounts:\n  - id: a\n    user: u\n    domain: d\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_OK);
    if (!c) return;
    /* codec_priority is the old spelling and still loads; it arrives
       migrated into the codecs list. */
    SP_ASSERT(c->n_codecs == 3);
    SP_ASSERT_STR(c->codecs[0].id, "opus");
    SP_ASSERT_STR(c->codecs[2].id, "PCMU");
    sp_config_free(c);
}

SP_TEST(out_of_range_log_level_is_rejected) {
    const char *p = write_tmp("log_level: 99\naccounts: []\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "log_level") != NULL);
}

SP_TEST(non_numeric_reg_expires_is_rejected) {
    const char *p = write_tmp(
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"
        "    reg_expires_s: soon\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "reg_expires_s") != NULL);
}

SP_TEST(missing_file_is_a_config_error) {
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load("/nonexistent/x.yaml", &c, err, sizeof err)
              == SP_ERR_CONFIG);
    SP_ASSERT(err[0] != '\0');
}

SP_TEST(empty_accounts_list_is_valid) {
    const char *p = write_tmp("accounts: []\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT(c->n_accounts == 0);
    sp_config_free(c);
}

SP_TEST(tls_paths_are_read) {
    const char *p = write_tmp(
        "tls_cert_file: /etc/comlink/cert.pem\n"
        "tls_key_file: /etc/comlink/key.pem\n"
        "tls_ca_file: /etc/comlink/ca.pem\n"
        "accounts: []\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_OK);
    if (!c) { printf("  %s\n", err); return; }
    SP_ASSERT_STR(c->tls_cert_file, "/etc/comlink/cert.pem");
    SP_ASSERT_STR(c->tls_key_file, "/etc/comlink/key.pem");
    SP_ASSERT_STR(c->tls_ca_file, "/etc/comlink/ca.pem");
    sp_config_free(c);
}

SP_TEST(tls_paths_survive_a_save) {
    const char *p = write_tmp(
        "tls_cert_file: /var/x/cert.pem\ntls_key_file: /var/x/key.pem\n"
        "accounts: []\n");
    sp_config *a = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &a, err, sizeof err) == SP_OK);
    if (!a) return;
    char out[512];
    snprintf(out, sizeof out, "%s/tls_rt.yaml", getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_config_save(a, out, err, sizeof err) == SP_OK);
    sp_config *b = NULL;
    SP_ASSERT(sp_config_load(out, &b, err, sizeof err) == SP_OK);
    if (b) {
        SP_ASSERT_STR(b->tls_cert_file, "/var/x/cert.pem");
        SP_ASSERT_STR(b->tls_key_file, "/var/x/key.pem");
        SP_ASSERT(b->tls_ca_file == NULL);
        sp_config_free(b);
    }
    sp_config_free(a);
}

SP_TEST(sip_log_keys_have_documented_defaults) {
    const char *p = write_tmp("accounts: []\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_OK);
    if (!c) { printf("  %s\n", err); return; }
    SP_ASSERT(c->sip_log == true);
    SP_ASSERT(c->sip_log_redact == true);
    SP_ASSERT(c->sip_log_max == 5000);
    SP_ASSERT(c->db_file == NULL);
    sp_config_free(c);
}

SP_TEST(sip_log_keys_can_be_overridden_and_survive_a_save) {
    const char *p = write_tmp(
        "sip_log: false\nsip_log_redact: false\nsip_log_max: 100\n"
        "db_file: /var/lib/comlink/x.db\naccounts: []\n");
    sp_config *a = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &a, err, sizeof err) == SP_OK);
    if (!a) { printf("  %s\n", err); return; }
    SP_ASSERT(a->sip_log == false);
    SP_ASSERT(a->sip_log_redact == false);
    SP_ASSERT(a->sip_log_max == 100);
    SP_ASSERT_STR(a->db_file, "/var/lib/comlink/x.db");

    char out[512];
    snprintf(out, sizeof out, "%s/siplog_rt.yaml", getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_config_save(a, out, err, sizeof err) == SP_OK);
    sp_config *b = NULL;
    SP_ASSERT(sp_config_load(out, &b, err, sizeof err) == SP_OK);
    if (b) {
        SP_ASSERT(b->sip_log == false);
        SP_ASSERT(b->sip_log_redact == false);
        SP_ASSERT(b->sip_log_max == 100);
        SP_ASSERT_STR(b->db_file, "/var/lib/comlink/x.db");
        sp_config_free(b);
    }
    sp_config_free(a);
}

SP_TEST(metrics_defaults_and_bounds) {
    /* Defaults: sampling on at two seconds, silence suppression off,
       because this phone is a measuring instrument. */
    const char *base = "accounts:\n  - id: a\n    user: u\n    domain: d\n";
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(write_tmp(base), &c, err, sizeof err) == SP_OK);
    if (c) {
        SP_ASSERT(c->metrics_interval_ms == 2000);
        SP_ASSERT(c->silence_suppression == false);
        sp_config_free(c);
    }

    /* Explicit values are taken as given. */
    c = NULL;
    SP_ASSERT(sp_config_load(write_tmp(
        "metrics_interval_ms: 500\nsilence_suppression: true\n"
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"),
        &c, err, sizeof err) == SP_OK);
    if (c) {
        SP_ASSERT(c->metrics_interval_ms == 500);
        SP_ASSERT(c->silence_suppression == true);
        sp_config_free(c);
    }

    /* Zero used to mean "sampling off" and is now the old spelling of
       modules.audio_quality: false. It still loads, and migrates: the
       module goes off and the interval keeps only its real meaning,
       the rate, so it falls back to the default rather than staying
       at a zero nothing reads any more. */
    c = NULL;
    SP_ASSERT(sp_config_load(write_tmp(
        "metrics_interval_ms: 0\n"
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"),
        &c, err, sizeof err) == SP_OK);
    if (c) {
        SP_ASSERT(sp_config_module_enabled(c, "audio_quality") == false);
        SP_ASSERT(c->metrics_interval_ms > 0);
        sp_config_free(c);
    }

    /* An interval below the floor is rejected loudly, not silently
       clamped: a typo that quietly sampled every 20 ms would flood the
       event queue. */
    c = NULL; err[0] = '\0';
    SP_ASSERT(sp_config_load(write_tmp(
        "metrics_interval_ms: 20\n"
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"),
        &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "metrics_interval_ms") != NULL);

    c = NULL; err[0] = '\0';
    SP_ASSERT(sp_config_load(write_tmp(
        "metrics_interval_ms: 600000\n"
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"),
        &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "metrics_interval_ms") != NULL);
}

SP_TEST(metrics_keys_survive_a_save) {
    /* Anything that does not survive save-then-load is destroyed the
       first time the operator edits an account in the GUI. */
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(write_tmp(
        "metrics_interval_ms: 750\nsilence_suppression: true\n"
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"),
        &c, err, sizeof err) == SP_OK);
    if (!c) return;
    char out[512];
    snprintf(out, sizeof out, "%s/sp_cfg_metrics_saved.yaml",
             getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_config_save(c, out, err, sizeof err) == SP_OK);
    sp_config_free(c);

    c = NULL;
    SP_ASSERT(sp_config_load(out, &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT(c->metrics_interval_ms == 750);
    SP_ASSERT(c->silence_suppression == true);
    sp_config_free(c);
}

SP_TEST(audio_device_keys_and_the_no_audio_predicate) {
    sp_config *c = NULL; char err[256] = {0};
    const char *tail = "accounts:\n  - id: a\n    user: u\n    domain: d\n";

    /* Nothing set: system default, and audio is wanted. */
    SP_ASSERT(sp_config_load(write_tmp(tail), &c, err, sizeof err) == SP_OK);
    if (c) {
        SP_ASSERT(c->audio_capture_device == NULL);
        SP_ASSERT(c->audio_playback_device == NULL);
        SP_ASSERT(sp_config_wants_no_audio(c) == false);
        sp_config_free(c);
    }

    /* Per-direction names are read and kept apart. */
    c = NULL;
    SP_ASSERT(sp_config_load(write_tmp(
        "audio_capture_device: headset-mic\n"
        "audio_playback_device: desk-speakers\n"
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"),
        &c, err, sizeof err) == SP_OK);
    if (c) {
        SP_ASSERT_STR(c->audio_capture_device, "headset-mic");
        SP_ASSERT_STR(c->audio_playback_device, "desk-speakers");
        SP_ASSERT(sp_config_wants_no_audio(c) == false);
        sp_config_free(c);
    }

    /* Any of the three keys saying "none" is a request for no sound
       card, because that is what the startup guard has to act on. */
    const char *nones[] = {
        "audio_device: none\n",
        "audio_capture_device: none\n",
        "audio_playback_device: none\n",
    };
    for (size_t i = 0; i < 3; i++) {
        char body[256];
        snprintf(body, sizeof body, "%s%s", nones[i], tail);
        c = NULL;
        SP_ASSERT(sp_config_load(write_tmp(body), &c, err, sizeof err) == SP_OK);
        if (c) {
            SP_ASSERT(sp_config_wants_no_audio(c) == true);
            sp_config_free(c);
        }
    }

    /* A NULL config is not a request for anything. */
    SP_ASSERT(sp_config_wants_no_audio(NULL) == false);
}

SP_TEST(audio_device_keys_survive_a_save) {
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(write_tmp(
        "audio_capture_device: mic-a\n"
        "audio_playback_device: spk-b\n"
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"),
        &c, err, sizeof err) == SP_OK);
    if (!c) return;
    char out[512];
    snprintf(out, sizeof out, "%s/sp_cfg_audio_saved.yaml",
             getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_config_save(c, out, err, sizeof err) == SP_OK);
    sp_config_free(c);

    c = NULL;
    SP_ASSERT(sp_config_load(out, &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT_STR(c->audio_capture_device, "mic-a");
    SP_ASSERT_STR(c->audio_playback_device, "spk-b");
    sp_config_free(c);
}

SP_TEST(window_size_defaults_to_unset_and_survives_a_save) {
    sp_config *c = NULL; char err[256] = {0};
    const char *tail = "accounts:\n  - id: a\n    user: u\n    domain: d\n";

    /* Unset means zero, which is how a frontend knows to use its own
       default rather than a remembered size. */
    SP_ASSERT(sp_config_load(write_tmp(tail), &c, err, sizeof err) == SP_OK);
    if (c) {
        SP_ASSERT(c->window_width == 0);
        SP_ASSERT(c->window_height == 0);
        sp_config_free(c);
    }

    c = NULL;
    SP_ASSERT(sp_config_load(write_tmp(
        "window_width: 512\nwindow_height: 720\n"
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"),
        &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT(c->window_width == 512);
    SP_ASSERT(c->window_height == 720);

    char out[512];
    snprintf(out, sizeof out, "%s/sp_cfg_window_saved.yaml",
             getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_config_save(c, out, err, sizeof err) == SP_OK);
    sp_config_free(c);

    c = NULL;
    SP_ASSERT(sp_config_load(out, &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT(c->window_width == 512);
    SP_ASSERT(c->window_height == 720);
    sp_config_free(c);
}

SP_TEST(an_unset_window_size_is_not_written_back_as_zero) {
    /* A zero pair in the file would be indistinguishable from a real
       setting of zero, so save omits the keys entirely when unset. */
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(write_tmp(
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"),
        &c, err, sizeof err) == SP_OK);
    if (!c) return;
    char out[512];
    snprintf(out, sizeof out, "%s/sp_cfg_window_unset.yaml",
             getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_config_save(c, out, err, sizeof err) == SP_OK);
    sp_config_free(c);

    FILE *f = fopen(out, "r");
    SP_ASSERT(f != NULL);
    if (!f) return;
    char buf[8192] = {0};
    size_t got = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[got] = '\0';
    SP_ASSERT(strstr(buf, "window_width") == NULL);
    SP_ASSERT(strstr(buf, "window_height") == NULL);
}

SP_TEST(the_activity_pane_setting_defaults_on_and_round_trips) {
    sp_config *c = NULL; char err[256] = {0};
    const char *tail = "accounts:\n  - id: a\n    user: u\n    domain: d\n";

    /* Shown by default: hiding the activity pane has to be a choice,
       since it is where the phone says what it is doing. */
    SP_ASSERT(sp_config_load(write_tmp(tail), &c, err, sizeof err) == SP_OK);
    if (c) {
        SP_ASSERT(c->show_activity_log == true);
        sp_config_free(c);
    }

    c = NULL;
    SP_ASSERT(sp_config_load(write_tmp(
        "show_activity_log: false\n"
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"),
        &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT(c->show_activity_log == false);

    char out[512];
    snprintf(out, sizeof out, "%s/sp_cfg_activity_saved.yaml",
             getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_config_save(c, out, err, sizeof err) == SP_OK);
    sp_config_free(c);

    c = NULL;
    SP_ASSERT(sp_config_load(out, &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT(c->show_activity_log == false);
    sp_config_free(c);
}

SP_TEST(an_absurd_window_size_is_rejected_naming_the_key) {
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(write_tmp(
        "window_width: 99999\n"
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"),
        &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "window_width") != NULL);
}

SP_TEST(dsp_keys_have_instrument_defaults_and_survive_a_save) {
    sp_config *c = NULL; char err[256] = {0};
    const char *tail = "accounts:\n  - id: a\n    user: u\n    domain: d\n";

    /* The defaults are a decision, not an accident: echo cancellation
       on because a speakerphone without it howls, anything that alters
       what the operator hears off, loss concealment on so that hearing
       every lost packet is a deliberate choice. */
    SP_ASSERT(sp_config_load(write_tmp(tail), &c, err, sizeof err) == SP_OK);
    if (c) {
        SP_ASSERT(c->echo_cancel == true);
        SP_ASSERT(c->echo_tail_ms == 200);
        SP_ASSERT(c->noise_suppression == false);
        SP_ASSERT(c->auto_gain == false);
        SP_ASSERT(c->comfort_noise == true);
        SP_ASSERT(c->packet_loss_concealment == true);
        SP_ASSERT(c->silence_suppression == false);
        sp_config_free(c);
    }

    /* Every one of them is readable, and every one survives a save:
       a setting that did not round-trip would be destroyed the first
       time the operator edited an account. */
    c = NULL;
    SP_ASSERT(sp_config_load(write_tmp(
        "echo_cancel: false\necho_tail_ms: 320\n"
        "noise_suppression: true\nauto_gain: true\n"
        "comfort_noise: false\npacket_loss_concealment: false\n"
        "silence_suppression: true\n"
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"),
        &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT(c->echo_cancel == false);
    SP_ASSERT(c->echo_tail_ms == 320);
    SP_ASSERT(c->noise_suppression == true);
    SP_ASSERT(c->auto_gain == true);
    SP_ASSERT(c->comfort_noise == false);
    SP_ASSERT(c->packet_loss_concealment == false);
    SP_ASSERT(c->silence_suppression == true);

    char out[512];
    snprintf(out, sizeof out, "%s/sp_cfg_dsp_saved.yaml", getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_config_save(c, out, err, sizeof err) == SP_OK);
    sp_config_free(c);

    c = NULL;
    SP_ASSERT(sp_config_load(out, &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT(c->echo_cancel == false);
    SP_ASSERT(c->echo_tail_ms == 320);
    SP_ASSERT(c->noise_suppression == true);
    SP_ASSERT(c->auto_gain == true);
    SP_ASSERT(c->comfort_noise == false);
    SP_ASSERT(c->packet_loss_concealment == false);
    SP_ASSERT(c->silence_suppression == true);
    sp_config_free(c);
}

SP_TEST(an_out_of_range_echo_tail_is_rejected_naming_the_key) {
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(write_tmp(
        "echo_tail_ms: 5000\n"
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"),
        &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "echo_tail_ms") != NULL);
}

SP_TEST(volume_defaults_to_unity_and_round_trips) {
    sp_config *c = NULL; char err[256] = {0};
    const char *tail = "accounts:\n  - id: a\n    user: u\n    domain: d\n";

    /* Unity by default: a phone should not start quieter or louder
       than the system it runs on. */
    SP_ASSERT(sp_config_load(write_tmp(tail), &c, err, sizeof err) == SP_OK);
    if (c) {
        SP_ASSERT(c->tx_volume_pct == 100);
        SP_ASSERT(c->rx_volume_pct == 100);
        sp_config_free(c);
    }

    c = NULL;
    SP_ASSERT(sp_config_load(write_tmp(
        "tx_volume: 0\nrx_volume: 200\n"
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"),
        &c, err, sizeof err) == SP_OK);
    if (!c) return;
    /* Both ends of the range are valid settings: 0 is silence and 200
       is twice as loud. */
    SP_ASSERT(c->tx_volume_pct == 0);
    SP_ASSERT(c->rx_volume_pct == 200);

    char out[512];
    snprintf(out, sizeof out, "%s/sp_cfg_volume_saved.yaml",
             getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_config_save(c, out, err, sizeof err) == SP_OK);
    sp_config_free(c);

    c = NULL;
    SP_ASSERT(sp_config_load(out, &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT(c->tx_volume_pct == 0);
    SP_ASSERT(c->rx_volume_pct == 200);
    sp_config_free(c);
}

SP_TEST(a_volume_beyond_the_range_is_rejected_naming_the_key) {
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(write_tmp(
        "rx_volume: 400\n"
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"),
        &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "rx_volume") != NULL);

    c = NULL; err[0] = '\0';
    SP_ASSERT(sp_config_load(write_tmp(
        "tx_volume: -10\n"
        "accounts:\n  - id: a\n    user: u\n    domain: d\n"),
        &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "tx_volume") != NULL);
}

SP_TEST(recording_keys_load) {
    const char *p = write_tmp(
        "recording:\n"
        "  dir: /srv/recordings\n"
        "  format: wav_ulaw\n"
        "sip_trace:\n"
        "  export_dir: /srv/traces\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT_STR(c->recording_dir, "/srv/recordings");
    SP_ASSERT(c->recording_format == SP_REC_WAV_ULAW);
    SP_ASSERT_STR(c->sip_trace_export_dir, "/srv/traces");
    sp_config_free(c);
}

/* Absent means the core's default, not an empty string: the core
   resolves NULL to a directory beside the configuration file. */
SP_TEST(recording_keys_default_to_nothing_stored) {
    const char *p = write_tmp("log_level: 3\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT(c->recording_dir == NULL);
    SP_ASSERT(c->sip_trace_export_dir == NULL);
    SP_ASSERT(c->recording_format == SP_REC_WAV);
    sp_config_free(c);
}

/* Relative to what? There is no good answer once a phone has a
   working directory nobody chose, so it is refused at load. */
SP_TEST(a_relative_recording_dir_is_refused) {
    const char *p = write_tmp("recording:\n  dir: recordings\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "absolute") != NULL);
    SP_ASSERT(strstr(err, "recordings") != NULL);
}

SP_TEST(a_relative_trace_export_dir_is_refused) {
    const char *p = write_tmp("sip_trace:\n  export_dir: traces\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "absolute") != NULL);
    SP_ASSERT(strstr(err, "traces") != NULL);
}

/* Opus recording (task 3) no longer needs playback (task 4) to be
   legal to configure: the writer stands on its own. The refusal this
   test used to assert is gone; what stays true is that the format is
   now accepted and round-trips like the other two. */
SP_TEST(opus_is_accepted_as_a_recording_format) {
    const char *p = write_tmp("recording:\n  format: opus\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_OK);
    if (!c) return;
    SP_ASSERT(c->recording_format == SP_REC_OPUS);
    sp_config_free(c);
}

SP_TEST(an_unknown_recording_key_is_refused) {
    const char *p = write_tmp("recording:\n  folder: /srv\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "folder") != NULL);
}

SP_TEST(an_unknown_sip_trace_key_is_refused) {
    const char *p = write_tmp("sip_trace:\n  dir: /srv\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "dir") != NULL);
}


/* Rollback safety. A newer Comlink writes top-level keys this build
   has never heard of, and refusing them means a downgraded package
   will not start -- which is how a 0.6.0 refused to boot on a config
   a 0.7.0 build had written "recording:" into. They are warned about
   and ignored instead. */
SP_TEST(an_unknown_top_level_key_is_ignored_not_refused) {
    const char *p = write_tmp(
        "log_level: 4\n"
        "recording:\n"
        "  format: wav_ulaw\n"
        "from_a_future_comlink: yes\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_OK);
    SP_ASSERT(c != NULL);
    if (!c) return;
    /* The keys it DOES know still take effect. */
    SP_ASSERT(c->log_level == 4);
    sp_config_free(c);
}

/* Inside a known mapping it stays fatal: that is the failure this
   strictness was written for, and a header rule is a mapping. */
SP_TEST(an_unknown_key_inside_a_mapping_is_still_refused) {
    const char *p = write_tmp("recording:\n  folder: /srv\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "folder") != NULL);
}

/* The operator is told, by name, once per unknown key. Counted rather
   than scraped off stderr. */
SP_TEST(every_unknown_top_level_key_is_named) {
    const char *p = write_tmp(
        "log_level: 3\nnot_a_key: 1\nnor_this_one: 2\n");
    char err[256] = {0};
    sp_yaml_node *root = sp_yaml_parse_file(p, err, sizeof err);
    SP_ASSERT(root != NULL);
    if (!root) return;
    SP_ASSERT(sp_config_warn_unknown_top(root) == 2);
    sp_yaml_free(root);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(loads_a_single_account);
    SP_RUN(transport_names_map_to_all_three_enums);
    SP_RUN(unknown_transport_is_a_config_error_naming_the_value);
    SP_RUN(unknown_account_key_is_a_hard_error);
    SP_RUN(a_misspelt_top_level_key_warns_and_leaves_its_setting_unset);
    SP_RUN(missing_required_account_key_is_an_error);
    SP_RUN(duplicate_account_id_is_an_error);
    SP_RUN(all_three_srtp_modes_parse);
    SP_RUN(srtp_defaults_to_disabled);
    SP_RUN(unknown_srtp_mode_is_rejected_naming_the_value);
    SP_RUN(srtp_mode_survives_a_save);
    SP_RUN(defaults_apply_when_optional_keys_absent);
    SP_RUN(register_false_is_honored);
    SP_RUN(sip_port_is_read);
    SP_RUN(codec_priority_preserves_order);
    SP_RUN(out_of_range_log_level_is_rejected);
    SP_RUN(non_numeric_reg_expires_is_rejected);
    SP_RUN(missing_file_is_a_config_error);
    SP_RUN(empty_accounts_list_is_valid);
    SP_RUN(tls_paths_are_read);
    SP_RUN(tls_paths_survive_a_save);
    SP_RUN(sip_log_keys_have_documented_defaults);
    SP_RUN(sip_log_keys_can_be_overridden_and_survive_a_save);
    SP_RUN(metrics_defaults_and_bounds);
    SP_RUN(metrics_keys_survive_a_save);
    SP_RUN(audio_device_keys_and_the_no_audio_predicate);
    SP_RUN(audio_device_keys_survive_a_save);
    SP_RUN(window_size_defaults_to_unset_and_survives_a_save);
    SP_RUN(an_unset_window_size_is_not_written_back_as_zero);
    SP_RUN(the_activity_pane_setting_defaults_on_and_round_trips);
    SP_RUN(an_absurd_window_size_is_rejected_naming_the_key);
    SP_RUN(dsp_keys_have_instrument_defaults_and_survive_a_save);
    SP_RUN(an_out_of_range_echo_tail_is_rejected_naming_the_key);
    SP_RUN(volume_defaults_to_unity_and_round_trips);
    SP_RUN(a_volume_beyond_the_range_is_rejected_naming_the_key);
    SP_RUN(an_unknown_top_level_key_is_ignored_not_refused);
    SP_RUN(an_unknown_key_inside_a_mapping_is_still_refused);
    SP_RUN(every_unknown_top_level_key_is_named);
    SP_RUN(recording_keys_load);
    SP_RUN(recording_keys_default_to_nothing_stored);
    SP_RUN(a_relative_recording_dir_is_refused);
    SP_RUN(a_relative_trace_export_dir_is_refused);
    SP_RUN(opus_is_accepted_as_a_recording_format);
    SP_RUN(an_unknown_recording_key_is_refused);
    SP_RUN(an_unknown_sip_trace_key_is_refused);
    return SP_TEST_RESULT();
}
