#include "sp_test.h"
#include "redact.h"
#include <stdlib.h>

SP_TEST(authorization_value_is_replaced) {
    char *out = sp_redact_sip(
        "REGISTER sip:pbx SIP/2.0\r\n"
        "Authorization: Digest username=\"1001\", response=\"deadbeef\"\r\n"
        "CSeq: 1 REGISTER\r\n\r\n");
    SP_ASSERT(out != NULL);
    if (!out) return;
    SP_ASSERT(strstr(out, "deadbeef") == NULL);
    SP_ASSERT(strstr(out, "1001") == NULL);
    SP_ASSERT(strstr(out, "Authorization: <redacted>") != NULL);
    SP_ASSERT(strstr(out, "CSeq: 1 REGISTER") != NULL);
    SP_ASSERT(strstr(out, "REGISTER sip:pbx SIP/2.0") != NULL);
    free(out);
}

SP_TEST(all_four_credential_headers_are_covered) {
    const char *names[] = {"Authorization", "Proxy-Authorization",
                           "WWW-Authenticate", "Proxy-Authenticate"};
    for (size_t i = 0; i < 4; i++) {
        char raw[256];
        snprintf(raw, sizeof raw, "SIP/2.0 401\r\n%s: secret-stuff\r\n\r\n",
                 names[i]);
        char *out = sp_redact_sip(raw);
        SP_ASSERT(out != NULL);
        if (!out) continue;
        SP_ASSERT(strstr(out, "secret-stuff") == NULL);
        SP_ASSERT(strstr(out, "<redacted>") != NULL);
        free(out);
    }
}

SP_TEST(header_matching_is_case_insensitive) {
    char *out = sp_redact_sip("REGISTER sip:x SIP/2.0\r\n"
                              "aUtHoRiZaTiOn: Digest secret\r\n\r\n");
    SP_ASSERT(out != NULL);
    if (!out) return;
    SP_ASSERT(strstr(out, "secret") == NULL);
    free(out);
}

/* A header whose name merely contains a credential name must survive,
   or a custom X-Authorization-Note would vanish. */
SP_TEST(only_whole_header_names_match) {
    char *out = sp_redact_sip("INVITE sip:x SIP/2.0\r\n"
                              "X-Authorization-Note: keep-me\r\n\r\n");
    SP_ASSERT(out != NULL);
    if (!out) return;
    SP_ASSERT(strstr(out, "keep-me") != NULL);
    free(out);
}

/* The body must survive: SDP is the most useful part of a trace. */
SP_TEST(the_body_is_untouched) {
    char *out = sp_redact_sip(
        "INVITE sip:x SIP/2.0\r\n"
        "Authorization: Digest secret\r\n"
        "Content-Type: application/sdp\r\n\r\n"
        "v=0\r\nm=audio 4000 RTP/AVP 0\r\n");
    SP_ASSERT(out != NULL);
    if (!out) return;
    SP_ASSERT(strstr(out, "m=audio 4000 RTP/AVP 0") != NULL);
    SP_ASSERT(strstr(out, "secret") == NULL);
    free(out);
}

/* A folded continuation line carries the rest of the value, so it must
   go too. */
SP_TEST(continuation_lines_of_a_credential_header_are_dropped) {
    char *out = sp_redact_sip(
        "REGISTER sip:x SIP/2.0\r\n"
        "Authorization: Digest username=\"u\",\r\n"
        "  response=\"leaked\"\r\n"
        "CSeq: 2 REGISTER\r\n\r\n");
    SP_ASSERT(out != NULL);
    if (!out) return;
    SP_ASSERT(strstr(out, "leaked") == NULL);
    SP_ASSERT(strstr(out, "CSeq: 2 REGISTER") != NULL);
    free(out);
}

/* A body line that happens to look like a credential header must not
   be touched: redaction stops at the header boundary. */
SP_TEST(redaction_stops_at_the_body) {
    char *out = sp_redact_sip(
        "MESSAGE sip:x SIP/2.0\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "Authorization: this is body text\r\n");
    SP_ASSERT(out != NULL);
    if (!out) return;
    SP_ASSERT(strstr(out, "this is body text") != NULL);
    free(out);
}

SP_TEST(a_message_with_nothing_to_redact_is_unchanged) {
    const char *raw = "OPTIONS sip:x SIP/2.0\r\nCSeq: 1 OPTIONS\r\n\r\n";
    char *out = sp_redact_sip(raw);
    SP_ASSERT(out != NULL);
    if (!out) return;
    SP_ASSERT_STR(out, raw);
    free(out);
}

SP_TEST(edge_inputs_do_not_crash) {
    SP_ASSERT(sp_redact_sip(NULL) == NULL);
    char *e = sp_redact_sip("");
    SP_ASSERT(e != NULL);
    if (e) { SP_ASSERT_STR(e, ""); free(e); }
    char *nohdr = sp_redact_sip("Authorization:");
    SP_ASSERT(nohdr != NULL);
    free(nohdr);
    char *nocolon = sp_redact_sip("Authorization\r\n\r\n");
    SP_ASSERT(nocolon != NULL);
    free(nocolon);
}

int main(void) {
    SP_RUN(authorization_value_is_replaced);
    SP_RUN(all_four_credential_headers_are_covered);
    SP_RUN(header_matching_is_case_insensitive);
    SP_RUN(only_whole_header_names_match);
    SP_RUN(the_body_is_untouched);
    SP_RUN(continuation_lines_of_a_credential_header_are_dropped);
    SP_RUN(redaction_stops_at_the_body);
    SP_RUN(a_message_with_nothing_to_redact_is_unchanged);
    SP_RUN(edge_inputs_do_not_crash);
    return SP_TEST_RESULT();
}
