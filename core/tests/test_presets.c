#include "sp_test.h"
#include "comlink/core.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static const char *cfg(const char *name, const char *body) {
    static char p[512];
    snprintf(p, sizeof p, "%s/%s.yaml", getenv("SP_TEST_TMP"), name);
    char db[512];
    snprintf(db, sizeof db, "%s/%s.db", getenv("SP_TEST_TMP"), name);
    unlink(db);
    FILE *f = fopen(p, "w");
    fprintf(f, "log_level: 0\nsip_port: 0\naudio_device: none\n"
               "db_file: %s\n%s"
               "accounts:\n  - id: t\n    user: 1\n    domain: 127.0.0.1\n"
               "    register: false\n", db, body);
    fclose(f);
    return p;
}

SP_TEST(presets_are_read_from_config_in_order) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg("pr1",
        "header_presets:\n"
        "  - name: incident\n"
        "    headers:\n"
        "      - {name: X-Ticket, value: INC-42}\n"
        "      - {name: X-Priority, value: high}\n"
        "  - name: trace\n"
        "    headers:\n"
        "      - {name: X-Debug, value: \"1\"}\n"), &c) == SP_OK);
    if (!c) return;

    const sp_header_preset *p = NULL; size_t n = 0;
    SP_ASSERT(sp_presets_list(c, &p, &n) == SP_OK);
    SP_ASSERT(n == 2);
    if (n == 2) {
        SP_ASSERT_STR(p[0].name, "incident");
        SP_ASSERT(p[0].n_headers == 2);
        SP_ASSERT_STR(p[0].headers[0].name, "X-Ticket");
        SP_ASSERT_STR(p[0].headers[0].value, "INC-42");
        SP_ASSERT_STR(p[0].headers[1].name, "X-Priority");
        SP_ASSERT_STR(p[1].name, "trace");
        SP_ASSERT(p[1].n_headers == 1);
    }
    sp_core_destroy(c);
}

SP_TEST(no_presets_is_not_an_error) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg("pr2", ""), &c) == SP_OK);
    if (!c) return;
    const sp_header_preset *p = NULL; size_t n = 99;
    SP_ASSERT(sp_presets_list(c, &p, &n) == SP_OK);
    SP_ASSERT(n == 0);
    sp_core_destroy(c);
}

/* Saving must persist, or a preset is just a session variable. */
SP_TEST(a_saved_preset_survives_a_restart) {
    const char *path = cfg("pr3", "");
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(path, &c) == SP_OK);
    if (!c) return;
    sp_header h[2] = { {"X-Ticket", "INC-7"}, {"X-Site", "c1"} };
    SP_ASSERT(sp_presets_save(c, "dispatch", h, 2) == SP_OK);
    sp_core_destroy(c);

    sp_core *c2 = NULL;
    SP_ASSERT(sp_core_create(path, &c2) == SP_OK);
    if (!c2) return;
    const sp_header_preset *p = NULL; size_t n = 0;
    SP_ASSERT(sp_presets_list(c2, &p, &n) == SP_OK);
    SP_ASSERT(n == 1);
    if (n == 1) {
        SP_ASSERT_STR(p[0].name, "dispatch");
        SP_ASSERT(p[0].n_headers == 2);
        SP_ASSERT_STR(p[0].headers[1].name, "X-Site");
        SP_ASSERT_STR(p[0].headers[1].value, "c1");
    }
    sp_core_destroy(c2);
}

/* Saving over an existing name replaces it rather than making a
   duplicate the user then has to hunt down. */
SP_TEST(saving_an_existing_name_replaces_it) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg("pr4", ""), &c) == SP_OK);
    if (!c) return;
    sp_header a[1] = { {"X-One", "1"} };
    sp_header b[2] = { {"X-Two", "2"}, {"X-Three", "3"} };
    SP_ASSERT(sp_presets_save(c, "same", a, 1) == SP_OK);
    SP_ASSERT(sp_presets_save(c, "same", b, 2) == SP_OK);
    const sp_header_preset *p = NULL; size_t n = 0;
    SP_ASSERT(sp_presets_list(c, &p, &n) == SP_OK);
    SP_ASSERT(n == 1);
    if (n == 1) {
        SP_ASSERT(p[0].n_headers == 2);
        SP_ASSERT_STR(p[0].headers[0].name, "X-Two");
    }
    sp_core_destroy(c);
}

SP_TEST(deleting_a_preset_persists) {
    const char *path = cfg("pr5", "");
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(path, &c) == SP_OK);
    if (!c) return;
    sp_header h[1] = { {"X-Gone", "y"} };
    SP_ASSERT(sp_presets_save(c, "doomed", h, 1) == SP_OK);
    SP_ASSERT(sp_presets_save(c, "keeper", h, 1) == SP_OK);
    SP_ASSERT(sp_presets_delete(c, "doomed") == SP_OK);
    SP_ASSERT(sp_presets_delete(c, "doomed") == SP_ERR_NOT_FOUND);
    sp_core_destroy(c);

    sp_core *c2 = NULL;
    SP_ASSERT(sp_core_create(path, &c2) == SP_OK);
    if (!c2) return;
    const sp_header_preset *p = NULL; size_t n = 0;
    SP_ASSERT(sp_presets_list(c2, &p, &n) == SP_OK);
    SP_ASSERT(n == 1);
    if (n == 1)
        SP_ASSERT_STR(p[0].name, "keeper");
    sp_core_destroy(c2);
}

SP_TEST(bad_preset_arguments_are_rejected) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg("pr6", ""), &c) == SP_OK);
    if (!c) return;
    sp_header h[1] = { {"X-A", "1"} };
    SP_ASSERT(sp_presets_save(c, NULL, h, 1) == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_presets_save(c, "", h, 1) == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_presets_save(c, "x", NULL, 1) == SP_ERR_INVALID_ARG);
    /* A preset with no headers is meaningless. */
    SP_ASSERT(sp_presets_save(c, "x", h, 0) == SP_ERR_INVALID_ARG);
    /* A header with an empty name would produce an invalid message. */
    sp_header bad[1] = { {"", "1"} };
    SP_ASSERT(sp_presets_save(c, "x", bad, 1) == SP_ERR_INVALID_ARG);
    sp_core_destroy(c);
}

// --- remembered header names -------------------------------------------

/* Names used once should be offered again, so the operator does not
   retype X-Geo-Location every time. */
SP_TEST(header_names_are_remembered_and_persist) {
    const char *path = cfg("pr7", "");
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(path, &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_header_names_remember(c, "X-Geo-Location") == SP_OK);
    SP_ASSERT(sp_header_names_remember(c, "X-Ticket") == SP_OK);
    /* Remembering the same name twice must not duplicate it. */
    SP_ASSERT(sp_header_names_remember(c, "X-Ticket") == SP_OK);

    const char **names = NULL; size_t n = 0;
    SP_ASSERT(sp_header_names_list(c, &names, &n) == SP_OK);
    SP_ASSERT(n == 2);
    sp_core_destroy(c);

    sp_core *c2 = NULL;
    SP_ASSERT(sp_core_create(path, &c2) == SP_OK);
    if (!c2) return;
    SP_ASSERT(sp_header_names_list(c2, &names, &n) == SP_OK);
    SP_ASSERT(n == 2);
    int saw_geo = 0;
    for (size_t i = 0; i < n; i++)
        if (strcmp(names[i], "X-Geo-Location") == 0)
            saw_geo = 1;
    SP_ASSERT(saw_geo == 1);
    sp_core_destroy(c2);
}

/* Saving a preset should teach the names it contains, so the editor
   offers them without the operator doing anything extra. */
SP_TEST(saving_a_preset_remembers_its_header_names) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg("pr8", ""), &c) == SP_OK);
    if (!c) return;
    sp_header h[2] = { {"X-Learned", "a"}, {"X-Also", "b"} };
    SP_ASSERT(sp_presets_save(c, "teach", h, 2) == SP_OK);
    const char **names = NULL; size_t n = 0;
    SP_ASSERT(sp_header_names_list(c, &names, &n) == SP_OK);
    int a = 0, b = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(names[i], "X-Learned") == 0) a = 1;
        if (strcmp(names[i], "X-Also") == 0) b = 1;
    }
    SP_ASSERT(a == 1 && b == 1);
    sp_core_destroy(c);
}

SP_TEST(an_empty_header_name_is_not_remembered) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg("pr9", ""), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_header_names_remember(c, "") == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_header_names_remember(c, NULL) == SP_ERR_INVALID_ARG);
    const char **names = NULL; size_t n = 99;
    SP_ASSERT(sp_header_names_list(c, &names, &n) == SP_OK);
    SP_ASSERT(n == 0);
    sp_core_destroy(c);
}

SP_TEST(an_unknown_preset_key_is_refused) {
    sp_core *c = NULL;
    sp_status st = sp_core_create(cfg("pr10",
        "header_presets:\n  - name: x\n    headerz: []\n"), &c);
    SP_ASSERT(st == SP_ERR_CONFIG);
    SP_ASSERT(c == NULL);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(presets_are_read_from_config_in_order);
    SP_RUN(no_presets_is_not_an_error);
    SP_RUN(a_saved_preset_survives_a_restart);
    SP_RUN(saving_an_existing_name_replaces_it);
    SP_RUN(deleting_a_preset_persists);
    SP_RUN(bad_preset_arguments_are_rejected);
    SP_RUN(header_names_are_remembered_and_persist);
    SP_RUN(saving_a_preset_remembers_its_header_names);
    SP_RUN(an_empty_header_name_is_not_remembered);
    SP_RUN(an_unknown_preset_key_is_refused);
    return SP_TEST_RESULT();
}
