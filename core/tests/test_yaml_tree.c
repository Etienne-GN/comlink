#include "sp_test.h"
#include "yaml_tree.h"
#include <stdlib.h>

SP_TEST(parses_a_flat_map) {
    char err[256];
    sp_yaml_node *n = sp_yaml_parse_string("a: 1\nb: two\n", err, sizeof err);
    SP_ASSERT(n != NULL);
    SP_ASSERT(sp_yaml_kind_of(n) == SP_YAML_MAP);
    SP_ASSERT(sp_yaml_len(n) == 2);
    SP_ASSERT_STR(sp_yaml_scalar(sp_yaml_get(n, "a")), "1");
    SP_ASSERT_STR(sp_yaml_scalar(sp_yaml_get(n, "b")), "two");
    SP_ASSERT(sp_yaml_get(n, "missing") == NULL);
    sp_yaml_free(n);
}

SP_TEST(parses_a_sequence_of_maps) {
    char err[256];
    const char *doc =
        "rules:\n"
        "  - name: one\n"
        "    direction: tx\n"
        "  - name: two\n"
        "    direction: rx\n";
    sp_yaml_node *n = sp_yaml_parse_string(doc, err, sizeof err);
    SP_ASSERT(n != NULL);
    sp_yaml_node *rules = sp_yaml_get(n, "rules");
    SP_ASSERT(rules != NULL);
    SP_ASSERT(sp_yaml_kind_of(rules) == SP_YAML_SEQ);
    SP_ASSERT(sp_yaml_len(rules) == 2);
    SP_ASSERT_STR(sp_yaml_scalar(sp_yaml_get(sp_yaml_at(rules, 0), "name")), "one");
    SP_ASSERT_STR(sp_yaml_scalar(sp_yaml_get(sp_yaml_at(rules, 1), "direction")), "rx");
    sp_yaml_free(n);
}

SP_TEST(parses_inline_flow_map_and_seq) {
    char err[256];
    sp_yaml_node *n = sp_yaml_parse_string(
        "set: {name: X-Foo, value: bar}\nm: [INVITE, REGISTER]\n", err, sizeof err);
    SP_ASSERT(n != NULL);
    SP_ASSERT_STR(sp_yaml_scalar(sp_yaml_get(sp_yaml_get(n, "set"), "name")), "X-Foo");
    sp_yaml_node *m = sp_yaml_get(n, "m");
    SP_ASSERT(sp_yaml_len(m) == 2);
    SP_ASSERT_STR(sp_yaml_scalar(sp_yaml_at(m, 1)), "REGISTER");
    sp_yaml_free(n);
}

SP_TEST(reports_line_numbers) {
    char err[256];
    sp_yaml_node *n = sp_yaml_parse_string("a: 1\nb: 2\nc: 3\n", err, sizeof err);
    SP_ASSERT(n != NULL);
    SP_ASSERT(sp_yaml_line(sp_yaml_get(n, "c")) == 3);
    sp_yaml_free(n);
}

SP_TEST(malformed_yaml_fails_with_a_message) {
    char err[256] = {0};
    sp_yaml_node *n = sp_yaml_parse_string("a: [1, 2\n", err, sizeof err);
    SP_ASSERT(n == NULL);
    SP_ASSERT(err[0] != '\0');
    sp_yaml_free(n);
}

SP_TEST(empty_document_is_not_a_crash) {
    char err[256] = {0};
    sp_yaml_node *n = sp_yaml_parse_string("", err, sizeof err);
    if (n) { SP_ASSERT(sp_yaml_len(n) == 0); sp_yaml_free(n); }
    else   { SP_ASSERT(err[0] != '\0'); }
}

SP_TEST(missing_file_fails_cleanly) {
    char err[256] = {0};
    sp_yaml_node *n = sp_yaml_parse_file("/nonexistent/nope.yaml", err, sizeof err);
    SP_ASSERT(n == NULL);
    SP_ASSERT(err[0] != '\0');
}

SP_TEST(key_at_enumerates_map_keys_in_order) {
    char err[256];
    sp_yaml_node *n = sp_yaml_parse_string("z: 1\na: 2\nm: 3\n", err, sizeof err);
    SP_ASSERT(n != NULL);
    SP_ASSERT(sp_yaml_len(n) == 3);
    SP_ASSERT_STR(sp_yaml_key_at(n, 0), "z");
    SP_ASSERT_STR(sp_yaml_key_at(n, 1), "a");
    SP_ASSERT_STR(sp_yaml_key_at(n, 2), "m");
    SP_ASSERT(sp_yaml_key_at(n, 3) == NULL);
    sp_yaml_free(n);
}

SP_TEST(nested_containers_survive_three_levels) {
    char err[256];
    sp_yaml_node *n = sp_yaml_parse_string(
        "a:\n  - b:\n      c: deep\n", err, sizeof err);
    SP_ASSERT(n != NULL);
    sp_yaml_node *b = sp_yaml_get(sp_yaml_at(sp_yaml_get(n, "a"), 0), "b");
    SP_ASSERT(b != NULL);
    SP_ASSERT_STR(sp_yaml_scalar(sp_yaml_get(b, "c")), "deep");
    sp_yaml_free(n);
}

SP_TEST(wrong_kind_accessors_return_safe_values) {
    char err[256];
    sp_yaml_node *n = sp_yaml_parse_string("a: scalar\n", err, sizeof err);
    sp_yaml_node *s = sp_yaml_get(n, "a");
    SP_ASSERT(sp_yaml_scalar(n) == NULL);      /* map is not a scalar */
    SP_ASSERT(sp_yaml_get(s, "x") == NULL);    /* scalar has no keys */
    SP_ASSERT(sp_yaml_at(s, 0) == NULL);       /* scalar has no elements */
    SP_ASSERT(sp_yaml_len(s) == 0);
    SP_ASSERT(sp_yaml_get(NULL, "x") == NULL);
    SP_ASSERT(sp_yaml_scalar(NULL) == NULL);
    sp_yaml_free(n);
}

int main(void) {
    SP_RUN(parses_a_flat_map);
    SP_RUN(parses_a_sequence_of_maps);
    SP_RUN(parses_inline_flow_map_and_seq);
    SP_RUN(reports_line_numbers);
    SP_RUN(malformed_yaml_fails_with_a_message);
    SP_RUN(empty_document_is_not_a_crash);
    SP_RUN(missing_file_fails_cleanly);
    SP_RUN(key_at_enumerates_map_keys_in_order);
    SP_RUN(nested_containers_survive_three_levels);
    SP_RUN(wrong_kind_accessors_return_safe_values);
    return SP_TEST_RESULT();
}
