#include "sp_test.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *path_for(const char *name) {
    static char p[512];
    snprintf(p, sizeof p, "%s/%s", getenv("SP_TEST_TMP"), name);
    return p;
}

static const char *write_file(const char *name, const char *body) {
    const char *p = path_for(name);
    FILE *f = fopen(p, "w");
    if (!f) return NULL;
    fputs(body, f);
    fclose(f);
    return p;
}

/* Reads a saved config back as plain text, for tests that need to
   assert on what actually landed in the file rather than on what a
   reload parses out of it. */
static bool file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

/* Saving then loading must yield the same configuration. Anything that
   does not survive the round trip is silently destroyed the first time
   a user edits an account in the GUI. */
SP_TEST(round_trip_preserves_everything) {
    const char *src = write_file("rt_in.yaml",
        "log_level: 4\n"
        "sip_port: 5070\n"
        "audio_device: none\n"
        "codec_priority: [opus, G722, PCMU]\n"
        "accounts:\n"
        "  - id: work\n"
        "    user: 1001\n"
        "    domain: pbx.example.lan\n"
        "    auth_user: authy\n"
        "    password: s3cret\n"
        "    registrar: sip:reg.example.lan\n"
        "    outbound_proxy: sip:proxy.example.lan\n"
        "    transport: tls\n"
        "    reg_expires_s: 600\n"
        "    stun_server: stun.example.lan\n"
        "    register: true\n"
        "  - id: home\n"
        "    user: 2002\n"
        "    domain: home.lan\n"
        "    transport: tcp\n"
        "    register: false\n"
        "header_rules:\n"
        "  - name: strip-ua\n"
        "    direction: tx\n"
        "    message: request\n"
        "    method: [INVITE, REGISTER]\n"
        "    actions:\n"
        "      - remove: User-Agent\n"
        "  - name: tag\n"
        "    direction: tx\n"
        "    message: request\n"
        "    account: work\n"
        "    actions:\n"
        "      - set: {name: X-Geo, value: qc}\n"
        "      - add: {name: X-Extra, value: two}\n"
        "recording:\n"
        "  dir: /srv/rec\n"
        "  format: wav_ulaw\n"
        "sip_trace:\n"
        "  export_dir: /srv/tr\n");

    sp_config *a = NULL; char err[512] = {0};
    SP_ASSERT(sp_config_load(src, &a, err, sizeof err) == SP_OK);
    if (!a) { printf("  load failed: %s\n", err); return; }

    const char *dst = path_for("rt_out.yaml");
    SP_ASSERT(sp_config_save(a, dst, err, sizeof err) == SP_OK);

    /* Mirror of an_unset_trace_export_dir_survives_a_save: the export
       dir is set here, so the guard in sp_config_save must actually
       write the sip_trace: section rather than skip it. */
    SP_ASSERT(file_contains(dst, "sip_trace:"));

    sp_config *b = NULL;
    SP_ASSERT(sp_config_load(dst, &b, err, sizeof err) == SP_OK);
    if (!b) { printf("  reload failed: %s\n", err); sp_config_free(a); return; }

    SP_ASSERT(b->log_level == 4);
    SP_ASSERT(b->sip_port == 5070);
    SP_ASSERT_STR(b->audio_device, "none");
    SP_ASSERT(b->n_codecs == 3);
    SP_ASSERT_STR(b->codecs[0].id, "opus");
    SP_ASSERT_STR(b->codecs[2].id, "PCMU");

    SP_ASSERT(b->n_accounts == 2);
    SP_ASSERT_STR(b->accounts[0].id, "work");
    SP_ASSERT_STR(b->accounts[0].user, "1001");
    SP_ASSERT_STR(b->accounts[0].domain, "pbx.example.lan");
    SP_ASSERT_STR(b->accounts[0].auth_user, "authy");
    SP_ASSERT_STR(b->accounts[0].password, "s3cret");
    SP_ASSERT_STR(b->accounts[0].registrar, "sip:reg.example.lan");
    SP_ASSERT_STR(b->accounts[0].outbound_proxy, "sip:proxy.example.lan");
    SP_ASSERT(b->accounts[0].transport == SP_TP_TLS);
    SP_ASSERT(b->accounts[0].reg_expires_s == 600);
    SP_ASSERT_STR(b->accounts[0].stun_server, "stun.example.lan");
    SP_ASSERT(b->accounts[0].do_register == true);
    SP_ASSERT_STR(b->accounts[1].id, "home");
    SP_ASSERT(b->accounts[1].transport == SP_TP_TCP);
    SP_ASSERT(b->accounts[1].do_register == false);

    /* The rules are the part most likely to be quietly lost. */
    SP_ASSERT(b->n_rules == 2);
    SP_ASSERT_STR(b->rules[0].name, "strip-ua");
    SP_ASSERT(b->rules[0].direction == SP_DIR_TX);
    SP_ASSERT(b->rules[0].message == SP_MSG_REQUEST);
    SP_ASSERT(b->rules[0].n_methods == 2);
    SP_ASSERT_STR(b->rules[0].methods[0], "INVITE");
    SP_ASSERT(b->rules[0].n_actions == 1);
    SP_ASSERT(b->rules[0].actions[0].kind == SP_ACT_REMOVE);
    SP_ASSERT_STR(b->rules[0].actions[0].name, "User-Agent");

    SP_ASSERT_STR(b->rules[1].name, "tag");
    SP_ASSERT_STR(b->rules[1].account, "work");
    SP_ASSERT(b->rules[1].n_actions == 2);
    SP_ASSERT(b->rules[1].actions[0].kind == SP_ACT_SET);
    SP_ASSERT_STR(b->rules[1].actions[0].value, "qc");
    SP_ASSERT(b->rules[1].actions[1].kind == SP_ACT_ADD);
    SP_ASSERT_STR(b->rules[1].actions[1].name, "X-Extra");

    SP_ASSERT_STR(b->recording_dir, "/srv/rec");
    SP_ASSERT(b->recording_format == SP_REC_WAV_ULAW);
    SP_ASSERT_STR(b->sip_trace_export_dir, "/srv/tr");

    sp_config_free(a);
    sp_config_free(b);
}

SP_TEST(values_needing_quotes_survive) {
    const char *src = write_file("q_in.yaml",
        "accounts:\n"
        "  - id: odd\n"
        "    user: \"12:34\"\n"
        "    domain: example.lan\n"
        "    password: \"pa ss #with: junk\"\n"
        "header_rules:\n"
        "  - name: geo\n"
        "    direction: tx\n"
        "    message: request\n"
        "    actions:\n"
        "      - set: {name: X-Geo, value: \"45.4,-71.9\"}\n");
    sp_config *a = NULL; char err[512] = {0};
    SP_ASSERT(sp_config_load(src, &a, err, sizeof err) == SP_OK);
    if (!a) { printf("  load failed: %s\n", err); return; }

    const char *dst = path_for("q_out.yaml");
    SP_ASSERT(sp_config_save(a, dst, err, sizeof err) == SP_OK);

    sp_config *b = NULL;
    SP_ASSERT(sp_config_load(dst, &b, err, sizeof err) == SP_OK);
    if (!b) { printf("  reload failed: %s\n", err); sp_config_free(a); return; }
    SP_ASSERT_STR(b->accounts[0].user, "12:34");
    SP_ASSERT_STR(b->accounts[0].password, "pa ss #with: junk");
    SP_ASSERT_STR(b->rules[0].actions[0].value, "45.4,-71.9");
    sp_config_free(a);
    sp_config_free(b);
}

SP_TEST(save_to_an_unwritable_path_is_an_error) {
    const char *src = write_file("u_in.yaml", "accounts: []\n");
    sp_config *a = NULL; char err[512] = {0};
    SP_ASSERT(sp_config_load(src, &a, err, sizeof err) == SP_OK);
    if (!a) return;
    SP_ASSERT(sp_config_save(a, "/proc/nope/x.yaml", err, sizeof err)
              == SP_ERR_CONFIG);
    SP_ASSERT(err[0] != '\0');
    sp_config_free(a);
}

/* A failed save must not leave a truncated config behind. */
SP_TEST(save_is_atomic) {
    const char *src = write_file("at_in.yaml",
        "accounts:\n  - id: a\n    user: u\n    domain: d\n");
    sp_config *a = NULL; char err[512] = {0};
    SP_ASSERT(sp_config_load(src, &a, err, sizeof err) == SP_OK);
    if (!a) return;
    const char *dst = path_for("at_out.yaml");
    SP_ASSERT(sp_config_save(a, dst, err, sizeof err) == SP_OK);
    /* No stray temp file left next to it. */
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    SP_ASSERT(fopen(tmp, "r") == NULL);
    sp_config_free(a);
}

/* sp_config_save only writes a sip_trace: mapping when
   sip_trace_export_dir is set, so a config that never had a sip_trace
   section keeps not having one across a save/reload. Coverage gap
   this closes: round_trip_preserves_everything only ever exercises
   recording/sip_trace with the export dir SET, so nothing proved the
   unset path round-trips at all.

   This is not proof the save guard is load-bearing -- it is not: the
   parser tolerates a bare "sip_trace:" line just as well (see the
   guard's comment in sp_config_save for why it is kept anyway). This
   test only guarantees the unset case keeps reloading, guard or no
   guard. See task-2-report.md, "Fix round 1" and "Fix round 2".

   What the guard actually does is decide whether the file gets a
   sip_trace: line at all, so that is what gets checked directly here
   -- the saved file itself, not just what a reload parses back out
   of it. */
SP_TEST(an_unset_trace_export_dir_survives_a_save) {
    const char *src = write_file("noex_in.yaml",
        "recording:\n"
        "  dir: /srv/rec\n"
        "  format: wav_ulaw\n");
    sp_config *a = NULL; char err[512] = {0};
    SP_ASSERT(sp_config_load(src, &a, err, sizeof err) == SP_OK);
    if (!a) { printf("  load failed: %s\n", err); return; }
    SP_ASSERT(a->sip_trace_export_dir == NULL);

    const char *dst = path_for("noex_out.yaml");
    SP_ASSERT(sp_config_save(a, dst, err, sizeof err) == SP_OK);
    SP_ASSERT(!file_contains(dst, "sip_trace:"));

    sp_config *b = NULL;
    SP_ASSERT(sp_config_load(dst, &b, err, sizeof err) == SP_OK);
    if (!b) { printf("  reload failed: %s\n", err); sp_config_free(a); return; }

    SP_ASSERT(b->sip_trace_export_dir == NULL);
    SP_ASSERT_STR(b->recording_dir, "/srv/rec");
    SP_ASSERT(b->recording_format == SP_REC_WAV_ULAW);

    sp_config_free(a);
    sp_config_free(b);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(round_trip_preserves_everything);
    SP_RUN(values_needing_quotes_survive);
    SP_RUN(save_to_an_unwritable_path_is_an_error);
    SP_RUN(save_is_atomic);
    SP_RUN(an_unset_trace_export_dir_survives_a_save);
    return SP_TEST_RESULT();
}
