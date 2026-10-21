#include "sp_test.h"
#include "modules.h"
#include <unistd.h>
#include "comlink/core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* extra is prepended to the YAML, so a case can set a top-level key
   without a second copy of this helper. */
static sp_core *booted_with(sp_account_id *acc, const char *extra) {
    static char p[512];
    snprintf(p, sizeof p, "%s/call.yaml", getenv("SP_TEST_TMP"));
    FILE *f = fopen(p, "w");
    if (!f) return NULL;
    fputs("log_level: 0\n", f);
    if (extra) fputs(extra, f);
    fputs("accounts:\n  - id: a\n    user: 1000\n    domain: 127.0.0.1\n"
          "    register: false\n", f);
    fclose(f);
    sp_core *c = NULL;
    if (sp_core_create(p, &c) != SP_OK) return NULL;
    if (sp_core_start(c) != SP_OK) { sp_core_destroy(c); return NULL; }
    if (acc) *acc = 1;   /* first handle issued by sp_core_start */
    return c;
}

static sp_core *booted(sp_account_id *acc) {
    return booted_with(acc, NULL);
}

static void wait_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void shutdown_core(sp_core *c) { sp_core_stop(c); sp_core_destroy(c); }

SP_TEST(unknown_call_handle_is_not_found_on_every_operation) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    const sp_call_id bogus = 987654321;
    SP_ASSERT(sp_call_hangup(c, bogus, 486) == SP_ERR_NOT_FOUND);
    SP_ASSERT(sp_call_set_hold(c, bogus, true) == SP_ERR_NOT_FOUND);
    SP_ASSERT(sp_call_send_dtmf(c, bogus, "1") == SP_ERR_NOT_FOUND);
    SP_ASSERT(sp_call_xfer_blind(c, bogus, "sip:x@y") == SP_ERR_NOT_FOUND);
    SP_ASSERT(sp_call_record_stop(c, bogus) == SP_ERR_NOT_FOUND);
    SP_ASSERT(sp_call_answer(c, bogus, 200, NULL, 0) == SP_ERR_NOT_FOUND);
    sp_call_info info;
    SP_ASSERT(sp_call_get_info(c, bogus, &info) == SP_ERR_NOT_FOUND);
    shutdown_core(c);
}

SP_TEST(make_rejects_bad_arguments) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    sp_call_id id = 0;
    SP_ASSERT(sp_call_make(c, acc, NULL, NULL, 0, &id) == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_call_make(c, acc, "", NULL, 0, &id) == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_call_make(c, acc, "sip:x@y", NULL, 0, NULL) == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_call_make(c, 999999, "sip:x@y", NULL, 0, &id) == SP_ERR_NOT_FOUND);
    shutdown_core(c);
}

SP_TEST(make_rejects_a_malformed_uri) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    sp_call_id id = 0;
    SP_ASSERT(sp_call_make(c, acc, "this is not a uri", NULL, 0, &id) != SP_OK);
    SP_ASSERT(id == 0);
    SP_ASSERT(sp_last_error_detail(c)[0] != '\0');
    shutdown_core(c);
}

SP_TEST(a_destination_that_cannot_be_reached_fails_once) {
    /* pjsua can run the whole life of a call inside
       pjsua_call_make_call: a host that does not resolve reaches
       DISCONNECTED through on_call_state before the call returns, and
       that callback already retires the handle and frees the record.
       Finishing it a second time on the error path was a double free
       on every unreachable destination, and a second history write
       that replaced the real status code with a zero. */
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    sp_call_id id = 0;
    const sp_status st =
        sp_call_make(c, acc, "sip:nobody@no-such-host.invalid", NULL, 0, &id);
    SP_ASSERT(st != SP_OK);
    SP_ASSERT(id == 0);
    /* Still usable afterwards, which is the part a crash takes away. */
    sp_call_id live[8]; size_t n_live = 0;
    SP_ASSERT(sp_call_list(c, live, 8, &n_live) == SP_OK);
    SP_ASSERT(n_live == 0);
    SP_ASSERT(sp_last_error_detail(c)[0] != '\0');
    shutdown_core(c);
}

SP_TEST(dtmf_rejects_non_dtmf_digits) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    /* Validation precedes the handle lookup, so these report the
       argument error rather than a missing handle. */
    SP_ASSERT(sp_call_send_dtmf(c, 1, "12QZ") == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_call_send_dtmf(c, 1, "") == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_call_send_dtmf(c, 1, NULL) == SP_ERR_INVALID_ARG);
    shutdown_core(c);
}

SP_TEST(dtmf_accepts_the_full_valid_alphabet) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    /* Valid digits with an unknown handle proves the alphabet passed
       and the failure came from the lookup. */
    SP_ASSERT(sp_call_send_dtmf(c, 424242, "0123456789*#ABCD")
              == SP_ERR_NOT_FOUND);
    shutdown_core(c);
}

SP_TEST(codec_enumeration_returns_the_codecs_built_in) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    const char **names = NULL; size_t n = 0;
    SP_ASSERT(sp_media_enum_codecs(c, &names, &n) == SP_OK);
    SP_ASSERT(n > 0);
    int saw_pcmu = 0, saw_opus = 0;
    for (size_t i = 0; i < n; i++) {
        if (strstr(names[i], "PCMU")) saw_pcmu = 1;
        if (strstr(names[i], "opus")) saw_opus = 1;
    }
    SP_ASSERT(saw_pcmu);
    /* Opus was enabled at configure time; if this fails the build
       config regressed. */
    SP_ASSERT(saw_opus);
    shutdown_core(c);
}

SP_TEST(setting_priority_for_an_unknown_codec_is_an_error) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    SP_ASSERT(sp_media_set_codec_priority(c, "NOSUCHCODEC/8000/1", 200) != SP_OK);
    SP_ASSERT(sp_media_set_codec_priority(c, NULL, 200) == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_media_set_codec_priority(c, "PCMU/8000/1", 999)
              == SP_ERR_INVALID_ARG);
    shutdown_core(c);
}

SP_TEST(blind_xfer_rejects_a_null_or_empty_target) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    SP_ASSERT(sp_call_xfer_blind(c, 1, NULL) == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_call_xfer_blind(c, 1, "") == SP_ERR_INVALID_ARG);
    shutdown_core(c);
}

SP_TEST(attended_xfer_rejects_transferring_a_call_to_itself) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    SP_ASSERT(sp_call_xfer_attended(c, 5, 5) == SP_ERR_INVALID_ARG);
    shutdown_core(c);
}

SP_TEST(attended_xfer_reports_an_unknown_handle) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    SP_ASSERT(sp_call_xfer_attended(c, 111111, 222222) == SP_ERR_NOT_FOUND);
    shutdown_core(c);
}

SP_TEST(record_start_rejects_a_null_or_empty_path) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    SP_ASSERT(sp_call_record_start(c, 1, NULL) == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_call_record_start(c, 1, "") == SP_ERR_INVALID_ARG);
    shutdown_core(c);
}

SP_TEST(auto_recording_refuses_an_unknown_call) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    const char *path = NULL;
    SP_ASSERT(sp_call_record_start_auto(c, 999999, &path)
              == SP_ERR_NOT_FOUND);
    shutdown_core(c);
}

SP_TEST(auto_recording_refuses_a_null_out_pointer) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    SP_ASSERT(sp_call_record_start_auto(c, 1, NULL) == SP_ERR_INVALID_ARG);
    shutdown_core(c);
}

SP_TEST(answer_rejects_an_out_of_range_code) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    SP_ASSERT(sp_call_answer(c, 1, 42, NULL, 0) == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_call_answer(c, 1, 999, NULL, 0) == SP_ERR_INVALID_ARG);
    shutdown_core(c);
}

SP_TEST(too_many_adhoc_headers_is_rejected) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    sp_header many[64];
    for (int i = 0; i < 64; i++) { many[i].name = "X-Many"; many[i].value = "1"; }
    sp_call_id id = 0;
    SP_ASSERT(sp_call_make(c, acc, "sip:x@127.0.0.1", many, 64, &id)
              == SP_ERR_INVALID_ARG);
    shutdown_core(c);
}

SP_TEST(adhoc_header_with_empty_name_is_rejected) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    sp_header bad[1] = { { .name = "", .value = "v" } };
    sp_call_id id = 0;
    SP_ASSERT(sp_call_make(c, acc, "sip:x@127.0.0.1", bad, 1, &id)
              == SP_ERR_INVALID_ARG);
    shutdown_core(c);
}

SP_TEST(quality_rejects_bad_arguments) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    sp_call_quality q;
    /* A handle that was never issued. */
    SP_ASSERT(sp_call_get_quality(c, 987654321, &q) == SP_ERR_NOT_FOUND);
    /* No output buffer, and no core. */
    SP_ASSERT(sp_call_get_quality(c, 987654321, NULL) == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_call_get_quality(NULL, 987654321, &q) == SP_ERR_INVALID_ARG);
    shutdown_core(c);
}

SP_TEST(quality_is_invalid_before_media) {
    sp_account_id acc = 0; sp_core *c = booted(&acc);
    SP_ASSERT(c != NULL); if (!c) return;
    /* A call to a port nobody listens on still has a handle, and
       asking for its quality must report "no data" rather than fail or
       hand back whatever was in the caller's buffer. */
    sp_call_id id = 0;
    if (sp_call_make(c, acc, "sip:nobody@127.0.0.1:1", NULL, 0, &id) == SP_OK) {
        sp_call_quality q;
        memset(&q, 0xAA, sizeof q);
        SP_ASSERT(sp_call_get_quality(c, id, &q) == SP_OK);
        SP_ASSERT(q.valid == false);
        SP_ASSERT(q.final == false);
        SP_ASSERT(q.rx_packets == 0);
        SP_ASSERT(q.mos == 0.0);
        SP_ASSERT(q.r_factor == 0);
        sp_call_hangup(c, id, 0);
    }
    shutdown_core(c);
}

/* The sampler must not be the reason a core cannot be torn down: a
   pending timer that fires into a freed core is the failure mode this
   covers. Several ticks fire in each iteration, and a cancel races the
   next fire every time. Run under valgrind for it to mean something. */
SP_TEST(sampler_starts_and_stops_cleanly) {
    for (int i = 0; i < 5; i++) {
        sp_account_id acc = 0;
        sp_core *c = booted_with(&acc, "metrics_interval_ms: 100\n");
        SP_ASSERT(c != NULL); if (!c) return;
        wait_ms(350);
        SP_ASSERT(sp_core_stop(c) == SP_OK);
        sp_core_destroy(c);
    }
}

SP_TEST(sampling_can_be_disabled) {
    /* Zero is a supported setting, not a busy loop and not a failure
       to start. */
    sp_account_id acc = 0;
    sp_core *c = booted_with(&acc, "metrics_interval_ms: 0\n");
    SP_ASSERT(c != NULL); if (!c) return;
    wait_ms(200);
    SP_ASSERT(sp_core_stop(c) == SP_OK);
    sp_core_destroy(c);
}

SP_TEST(the_signal_level_of_an_unknown_call_is_not_found) {
    sp_core *c = NULL;
    static char path[512];
    char db[512];
    snprintf(path, sizeof path, "%s/sig.yaml", getenv("SP_TEST_TMP"));
    snprintf(db, sizeof db, "%s/sig.db", getenv("SP_TEST_TMP"));
    unlink(db);
    FILE *f = fopen(path, "w");
    if (!f) { SP_ASSERT(0); return; }
    fprintf(f, "log_level: 0\nsip_port: 0\naudio_device: none\n"
               "db_file: %s\naccounts: []\n", db);
    fclose(f);
    SP_ASSERT(sp_core_create(path, &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);

    sp_signal_level lv;
    SP_ASSERT(sp_call_get_signal(c, 999999, &lv) == SP_ERR_NOT_FOUND);
    SP_ASSERT(sp_call_get_signal(c, 1, NULL) == SP_ERR_INVALID_ARG);

    /* Not gated on the audio quality module: the meter has to work
       whether or not anyone wants RTCP statistics kept. */
    SP_ASSERT(sp_module_set(c, "audio_quality", false) == SP_OK);
    SP_ASSERT(sp_call_get_signal(c, 999999, &lv) == SP_ERR_NOT_FOUND);
    sp_core_destroy(c);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(unknown_call_handle_is_not_found_on_every_operation);
    SP_RUN(make_rejects_bad_arguments);
    SP_RUN(make_rejects_a_malformed_uri);
    SP_RUN(a_destination_that_cannot_be_reached_fails_once);
    SP_RUN(dtmf_rejects_non_dtmf_digits);
    SP_RUN(dtmf_accepts_the_full_valid_alphabet);
    SP_RUN(codec_enumeration_returns_the_codecs_built_in);
    SP_RUN(setting_priority_for_an_unknown_codec_is_an_error);
    SP_RUN(blind_xfer_rejects_a_null_or_empty_target);
    SP_RUN(attended_xfer_rejects_transferring_a_call_to_itself);
    SP_RUN(attended_xfer_reports_an_unknown_handle);
    SP_RUN(record_start_rejects_a_null_or_empty_path);
    SP_RUN(auto_recording_refuses_an_unknown_call);
    SP_RUN(auto_recording_refuses_a_null_out_pointer);
    SP_RUN(answer_rejects_an_out_of_range_code);
    SP_RUN(too_many_adhoc_headers_is_rejected);
    SP_RUN(adhoc_header_with_empty_name_is_rejected);
    SP_RUN(quality_rejects_bad_arguments);
    SP_RUN(quality_is_invalid_before_media);
    SP_RUN(sampler_starts_and_stops_cleanly);
    SP_RUN(sampling_can_be_disabled);
    SP_RUN(the_signal_level_of_an_unknown_call_is_not_found);
    return SP_TEST_RESULT();
}
