#include "sp_test.h"
#include "sipuri.h"

static void check_parse(const char *in, const char *want_display,
                        const char *want_user, const char *want_host) {
    sp_party p;
    sp_sipuri_parse(in, &p);
    if (strcmp(p.display, want_display) != 0)
        printf("  FAIL display of \"%s\": got \"%s\" want \"%s\"\n",
               in, p.display, want_display);
    if (strcmp(p.user, want_user) != 0)
        printf("  FAIL user of \"%s\": got \"%s\" want \"%s\"\n",
               in, p.user, want_user);
    if (strcmp(p.host, want_host) != 0)
        printf("  FAIL host of \"%s\": got \"%s\" want \"%s\"\n",
               in, p.host, want_host);
    SP_ASSERT(strcmp(p.display, want_display) == 0);
    SP_ASSERT(strcmp(p.user, want_user) == 0);
    SP_ASSERT(strcmp(p.host, want_host) == 0);
}

SP_TEST(a_quoted_display_name_with_angle_brackets) {
    check_parse("\"Alice Smith\" <sip:1001@pbx.example.lan>",
                "Alice Smith", "1001", "pbx.example.lan");
}

SP_TEST(an_unquoted_display_name) {
    check_parse("Bob <sip:bob@example.lan>", "Bob", "bob", "example.lan");
}

SP_TEST(no_display_name_with_angle_brackets) {
    check_parse("<sip:2002@10.0.0.1>", "", "2002", "10.0.0.1");
}

SP_TEST(a_bare_uri_without_angle_brackets) {
    check_parse("sip:3003@pbx.lan", "", "3003", "pbx.lan");
}

SP_TEST(a_port_is_not_part_of_the_host) {
    check_parse("<sip:alice@127.0.0.1:5080>", "", "alice", "127.0.0.1");
}

SP_TEST(uri_parameters_are_stripped) {
    check_parse("<sip:alice@pbx.lan;transport=tls>", "", "alice", "pbx.lan");
    check_parse("<sip:alice@pbx.lan:5061;transport=tls>", "", "alice",
                "pbx.lan");
}

SP_TEST(the_sips_scheme_is_understood) {
    check_parse("\"Secure\" <sips:carol@pbx.lan>", "Secure", "carol",
                "pbx.lan");
}

/* A PSTN number is the common real case and must survive intact. */
SP_TEST(a_phone_number_user_part_is_preserved) {
    check_parse("\"Mobile\" <sip:+15145551234@carrier.net>",
                "Mobile", "+15145551234", "carrier.net");
}

SP_TEST(a_uri_with_no_user_part_yields_a_host_only) {
    check_parse("<sip:pbx.example.lan>", "", "", "pbx.example.lan");
}

SP_TEST(edge_inputs_do_not_crash) {
    sp_party p;
    sp_sipuri_parse(NULL, &p);
    SP_ASSERT(p.display[0] == '\0');
    SP_ASSERT(p.user[0] == '\0');
    sp_sipuri_parse("", &p);
    SP_ASSERT(p.user[0] == '\0');
    sp_sipuri_parse("garbage with no uri", &p);
    /* Nothing parseable: must not invent a user. */
    SP_ASSERT(p.user[0] == '\0');
    sp_sipuri_parse("\"unterminated <sip:x@y>", &p);
    sp_sipuri_parse("<sip:", &p);
    sp_sipuri_parse("@", &p);
}

/* Whichever field is populated, the label must never be empty, or the
   call display shows a blank caller. */
SP_TEST(a_label_is_always_produced) {
    const char *inputs[] = {
        "\"Alice\" <sip:1001@pbx.lan>",
        "<sip:1001@pbx.lan>",
        "sip:1001@pbx.lan",
        "<sip:pbx.lan>",
        "garbage",
        ""
    };
    for (size_t i = 0; i < 6; i++) {
        sp_party p;
        sp_sipuri_parse(inputs[i], &p);
        const char *label = sp_sipuri_label(&p);
        SP_ASSERT(label != NULL);
        SP_ASSERT(label[0] != '\0');
    }
    /* A display name wins; otherwise the user; otherwise the host. */
    sp_party a, b, c;
    sp_sipuri_parse("\"Alice\" <sip:1001@pbx.lan>", &a);
    sp_sipuri_parse("<sip:1001@pbx.lan>", &b);
    sp_sipuri_parse("<sip:pbx.lan>", &c);
    SP_ASSERT_STR(sp_sipuri_label(&a), "Alice");
    SP_ASSERT_STR(sp_sipuri_label(&b), "1001");
    SP_ASSERT_STR(sp_sipuri_label(&c), "pbx.lan");
}

SP_TEST(overlong_input_is_truncated_not_overflowed) {
    char big[2048];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    memcpy(big, "\"", 1);
    sp_party p;
    sp_sipuri_parse(big, &p);
    /* Whatever it decides, the buffers must remain NUL-terminated. */
    SP_ASSERT(p.display[sizeof p.display - 1] == '\0');
    SP_ASSERT(p.user[sizeof p.user - 1] == '\0');
    SP_ASSERT(p.host[sizeof p.host - 1] == '\0');
}

int main(void) {
    SP_RUN(a_quoted_display_name_with_angle_brackets);
    SP_RUN(an_unquoted_display_name);
    SP_RUN(no_display_name_with_angle_brackets);
    SP_RUN(a_bare_uri_without_angle_brackets);
    SP_RUN(a_port_is_not_part_of_the_host);
    SP_RUN(uri_parameters_are_stripped);
    SP_RUN(the_sips_scheme_is_understood);
    SP_RUN(a_phone_number_user_part_is_preserved);
    SP_RUN(a_uri_with_no_user_part_yields_a_host_only);
    SP_RUN(edge_inputs_do_not_crash);
    SP_RUN(a_label_is_always_produced);
    SP_RUN(overlong_input_is_truncated_not_overflowed);
    return SP_TEST_RESULT();
}
