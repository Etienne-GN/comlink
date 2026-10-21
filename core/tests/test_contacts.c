#include "sp_test.h"
#include "contacts.h"
#include "store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static sp_store   *g_store;
static sp_contacts *g_c;

static void open_fresh(void) {
    static char path[512];
    snprintf(path, sizeof path, "%s/contacts_test.db", getenv("SP_TEST_TMP"));
    unlink(path);
    char err[256] = {0};
    g_store = sp_store_open(path, err, sizeof err);
    if (!g_store) {
        printf("  cannot open store: %s\n", err);
        exit(1);
    }
    g_c = sp_contacts_create(g_store);
}

static void close_all(void) {
    sp_contacts_destroy(g_c);
    sp_store_close(g_store);
}

static const char *norm(const char *in) {
    static char out[64];
    sp_contacts_normalise(in, out, sizeof out);
    return out;
}

static int64_t add(const char *name, const char *company, bool fav,
                   const char *n1, const char *n2) {
    sp_contact_number nums[2];
    memset(nums, 0, sizeof nums);
    nums[0].uri = n1;
    nums[0].label = "work";
    nums[1].uri = n2;
    nums[1].label = "mobile";

    sp_contact c;
    memset(&c, 0, sizeof c);
    c.name = name;
    c.company = company;
    c.favourite = fav;
    c.numbers = nums;
    c.n_numbers = n2 ? 2 : 1;

    int64_t id = 0;
    SP_ASSERT(sp_contacts_save(g_c, &c, &id) == SP_OK);
    return id;
}

SP_TEST(two_spellings_of_one_number_normalise_together) {
    /* The whole of matching rests on this, so it is checked before
       anything that depends on it. */
    SP_ASSERT_STR(norm("+1-418-555-1234"), "4185551234");
    SP_ASSERT_STR(norm("1 (418) 555-1234"), "4185551234");
    SP_ASSERT_STR(norm("418.555.1234"), "4185551234");
    SP_ASSERT_STR(norm("sip:+14185551234@pbx.example.lan"), "4185551234");
    SP_ASSERT_STR(norm("\"Night desk\" <sip:4185551234@pbx.lan>"),
                  "4185551234");
}

SP_TEST(an_extension_normalises_to_itself) {
    SP_ASSERT_STR(norm("6046"), "6046");
    SP_ASSERT_STR(norm("sip:6046@pbx.example.lan"), "6046");
    SP_ASSERT_STR(norm("*9198"), "9198");
}

SP_TEST(something_with_no_digits_in_it_normalises_to_nothing) {
    /* And therefore matches nothing, rather than matching the first
       contact whose key is also empty. */
    SP_ASSERT_STR(norm("sip:reception@pbx.example.lan"), "");
    SP_ASSERT_STR(norm(""), "");
    SP_ASSERT_STR(norm(NULL), "");
}

SP_TEST(a_contact_comes_back_the_way_it_went_in) {
    open_fresh();
    const int64_t id = add("Sylvie Tremblay", "Acme", false,
                           "sip:6046@pbx.example.lan", "+1-418-555-1234");
    SP_ASSERT(id > 0);

    const sp_contact *c = NULL;
    SP_ASSERT(sp_contacts_get(g_c, id, &c) == SP_OK);
    if (c) {
        SP_ASSERT_STR(c->name, "Sylvie Tremblay");
        SP_ASSERT_STR(c->company, "Acme");
        SP_ASSERT(c->n_numbers == 2);
        /* In the order they were given: an operator who put the desk
           first meant the desk to be dialled first. */
        SP_ASSERT_STR(c->numbers[0].uri, "sip:6046@pbx.example.lan");
        SP_ASSERT_STR(c->numbers[0].label, "work");
        SP_ASSERT_STR(c->numbers[1].uri, "+1-418-555-1234");
    }
    close_all();
}

SP_TEST(a_contact_that_cannot_be_dialled_is_refused) {
    open_fresh();
    sp_contact c;
    memset(&c, 0, sizeof c);
    c.name = "Nobody";
    SP_ASSERT(sp_contacts_save(g_c, &c, NULL) == SP_ERR_INVALID_ARG);

    sp_contact_number n = {0};
    n.uri = "6046";
    c.name = "";
    c.numbers = &n;
    c.n_numbers = 1;
    SP_ASSERT(sp_contacts_save(g_c, &c, NULL) == SP_ERR_INVALID_ARG);

    /* And neither attempt left anything behind. */
    const sp_contact *rows = NULL;
    size_t n_rows = 1;
    SP_ASSERT(sp_contacts_query(g_c, NULL, 0, &rows, &n_rows) == SP_OK);
    SP_ASSERT(n_rows == 0);
    close_all();
}

SP_TEST(saving_again_replaces_the_numbers_rather_than_adding_to_them) {
    open_fresh();
    const int64_t id = add("Jean", NULL, false, "6046", "6047");

    sp_contact_number one = {0};
    one.uri = "7000";
    sp_contact c;
    memset(&c, 0, sizeof c);
    c.id = id;
    c.name = "Jean Cote";
    c.numbers = &one;
    c.n_numbers = 1;
    SP_ASSERT(sp_contacts_save(g_c, &c, NULL) == SP_OK);

    const sp_contact *got = NULL;
    SP_ASSERT(sp_contacts_get(g_c, id, &got) == SP_OK);
    if (got) {
        SP_ASSERT_STR(got->name, "Jean Cote");
        SP_ASSERT(got->n_numbers == 1);
        SP_ASSERT_STR(got->numbers[0].uri, "7000");
    }
    /* The number that went away must stop resolving, or the book says
       one thing and the panel another. */
    const sp_contact *found = NULL;
    SP_ASSERT(sp_contacts_lookup(g_c, "6046", &found) == SP_ERR_NOT_FOUND);
    SP_ASSERT(sp_contacts_lookup(g_c, "7000", &found) == SP_OK);
    close_all();
}

SP_TEST(updating_something_that_is_not_there_says_so) {
    open_fresh();
    sp_contact_number n = {0};
    n.uri = "6046";
    sp_contact c;
    memset(&c, 0, sizeof c);
    c.id = 4242;
    c.name = "Ghost";
    c.numbers = &n;
    c.n_numbers = 1;
    SP_ASSERT(sp_contacts_save(g_c, &c, NULL) == SP_ERR_NOT_FOUND);
    close_all();
}

SP_TEST(a_call_finds_its_contact_however_the_number_is_written) {
    open_fresh();
    add("Night desk", NULL, false, "+1-418-555-1234", NULL);

    const char *forms[] = {
        "4185551234", "1-418-555-1234", "+14185551234",
        "sip:14185551234@pbx.example.lan",
        "\"Someone\" <sip:+1-418-555-1234@pbx.example.lan>",
    };
    for (size_t i = 0; i < sizeof forms / sizeof *forms; i++) {
        const sp_contact *c = NULL;
        if (sp_contacts_lookup(g_c, forms[i], &c) == SP_OK && c)
            SP_ASSERT_STR(c->name, "Night desk");
        else
            printf("  FAIL no contact for %s\n", forms[i]), sp_test_failures++;
    }
    close_all();
}

SP_TEST(a_number_nobody_has_resolves_to_nobody) {
    open_fresh();
    add("Night desk", NULL, false, "6046", NULL);
    const sp_contact *c = NULL;
    SP_ASSERT(sp_contacts_lookup(g_c, "6047", &c) == SP_ERR_NOT_FOUND);
    SP_ASSERT(c == NULL);
    SP_ASSERT(sp_contacts_lookup(g_c, "sip:reception@pbx", &c)
              == SP_ERR_NOT_FOUND);
    close_all();
}

SP_TEST(an_extension_never_borrows_a_citizens_name) {
    /* The property the whole matching rule exists to hold. 4185 is a
       desk; 418-555-4185 is somebody's phone. A suffix match would put
       the second name on the first call, and on an emergency line a
       wrong name is worse than no name. */
    open_fresh();
    add("Citizen", NULL, false, "418-555-4185", NULL);

    const sp_contact *c = NULL;
    SP_ASSERT(sp_contacts_lookup(g_c, "4185", &c) == SP_ERR_NOT_FOUND);
    SP_ASSERT(sp_contacts_lookup(g_c, "sip:4185@pbx.example.lan", &c)
              == SP_ERR_NOT_FOUND);
    /* And the full number still resolves, so the rule has not simply
       broken matching. */
    SP_ASSERT(sp_contacts_lookup(g_c, "4185554185", &c) == SP_OK);
    close_all();
}

SP_TEST(deleting_takes_the_numbers_with_it) {
    open_fresh();
    const int64_t id = add("Temp", NULL, false, "6046", "6047");
    SP_ASSERT(sp_contacts_delete(g_c, id) == SP_OK);

    const sp_contact *c = NULL;
    SP_ASSERT(sp_contacts_get(g_c, id, &c) == SP_ERR_NOT_FOUND);
    SP_ASSERT(sp_contacts_lookup(g_c, "6046", &c) == SP_ERR_NOT_FOUND);
    SP_ASSERT(sp_contacts_lookup(g_c, "6047", &c) == SP_ERR_NOT_FOUND);
    SP_ASSERT(sp_contacts_delete(g_c, id) == SP_ERR_NOT_FOUND);
    close_all();
}

SP_TEST(the_list_puts_favourites_first_then_names_in_order) {
    open_fresh();
    add("Zoe", NULL, false, "6001", NULL);
    add("Alain", NULL, false, "6002", NULL);
    add("Repartition", NULL, true, "6003", NULL);

    const sp_contact *rows = NULL;
    size_t n = 0;
    SP_ASSERT(sp_contacts_query(g_c, NULL, 0, &rows, &n) == SP_OK);
    SP_ASSERT(n == 3);
    if (n == 3) {
        SP_ASSERT_STR(rows[0].name, "Repartition");
        SP_ASSERT_STR(rows[1].name, "Alain");
        SP_ASSERT_STR(rows[2].name, "Zoe");
    }
    close_all();
}

SP_TEST(search_reaches_the_name_the_company_and_the_number) {
    open_fresh();
    add("Sylvie Tremblay", "Acme", false, "6046", NULL);
    add("Marc Gagnon", "Hydro", false, "418-555-9999", NULL);

    struct { const char *q; size_t want; const char *first; } cases[] = {
        { "trem",  1, "Sylvie Tremblay" },
        { "Acme", 1, "Sylvie Tremblay" },
        { "6046",  1, "Sylvie Tremblay" },
        { "9999",  1, "Marc Gagnon" },
        { "a",     2, NULL },
        { "zzz",   0, NULL },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const sp_contact *rows = NULL;
        size_t n = 0;
        SP_ASSERT(sp_contacts_query(g_c, cases[i].q, 0, &rows, &n) == SP_OK);
        if (n != cases[i].want) {
            printf("  FAIL search \"%s\": %zu rows, wanted %zu\n",
                   cases[i].q, n, cases[i].want);
            sp_test_failures++;
        } else if (cases[i].first && n) {
            SP_ASSERT_STR(rows[0].name, cases[i].first);
        }
    }
    close_all();
}

SP_TEST(a_search_for_a_wildcard_is_a_search_for_a_wildcard) {
    /* Binding the search keeps it out of the SQL, and says nothing
       about LIKE: % and _ are wildcards wherever they come from. Bound
       straight in, a search for "%" matched the whole book and a
       contact called "100%" could never be found. */
    open_fresh();
    add("100%", NULL, false, "6046", NULL);
    add("A_B line", NULL, false, "6047", NULL);
    add("Someone else", NULL, false, "6048", NULL);

    const sp_contact *rows = NULL;
    size_t n = 0;
    SP_ASSERT(sp_contacts_query(g_c, "%", 0, &rows, &n) == SP_OK);
    SP_ASSERT(n == 1);
    if (n == 1)
        SP_ASSERT_STR(rows[0].name, "100%");

    SP_ASSERT(sp_contacts_query(g_c, "A_B", 0, &rows, &n) == SP_OK);
    SP_ASSERT(n == 1);
    if (n == 1)
        SP_ASSERT_STR(rows[0].name, "A_B line");

    /* And an underscore is not a stand-in for any character. */
    SP_ASSERT(sp_contacts_query(g_c, "A_", 0, &rows, &n) == SP_OK);
    SP_ASSERT(n == 1);
    close_all();
}

SP_TEST(a_lookup_does_not_free_the_list_someone_is_reading) {
    /* The panel asks who is calling on every refresh, which is while
       the contacts tab may be on screen. One shared result buffer
       would hand the frontend freed memory. */
    open_fresh();
    add("Sylvie Tremblay", NULL, false, "6046", NULL);
    add("Marc Gagnon", NULL, false, "6047", NULL);

    const sp_contact *rows = NULL;
    size_t n = 0;
    SP_ASSERT(sp_contacts_query(g_c, NULL, 0, &rows, &n) == SP_OK);
    SP_ASSERT(n == 2);

    const sp_contact *found = NULL;
    SP_ASSERT(sp_contacts_lookup(g_c, "6046", &found) == SP_OK);
    const sp_contact *got = NULL;
    SP_ASSERT(sp_contacts_get(g_c, rows ? rows[0].id : 0, &got) == SP_OK);

    /* The list is still the list. */
    if (n == 2) {
        SP_ASSERT_STR(rows[0].name, "Marc Gagnon");
        SP_ASSERT_STR(rows[1].name, "Sylvie Tremblay");
    }
    close_all();
}

/* A picture survives the round trip byte for byte. Anything less and
   the operator's chosen image is not the one they get back. */
SP_TEST(a_picture_round_trips_unchanged) {
    open_fresh();
    /* A PNG signature plus filler: the core stores bytes and does not
       parse them, so a real image is not needed to test storage. */
    unsigned char png[300];
    memcpy(png, "\x89PNG\r\n\x1a\n", 8);
    for (size_t i = 8; i < sizeof png; i++)
        png[i] = (unsigned char)(i & 0xff);

    sp_contact_number n = {0};
    n.uri = "1001";
    n.label = "work";
    sp_contact c;
    memset(&c, 0, sizeof c);
    c.name = "Ada";
    c.numbers = &n;
    c.n_numbers = 1;
    c.picture = png;
    c.picture_len = sizeof png;
    c.picture_type = "png";

    int64_t id = 0;
    SP_ASSERT(sp_contacts_save(g_c, &c, &id) == SP_OK);
    SP_ASSERT(id > 0);

    const sp_contact *back = NULL;
    SP_ASSERT(sp_contacts_get(g_c, id, &back) == SP_OK);
    SP_ASSERT(back != NULL);
    if (back) {
        SP_ASSERT(back->picture_len == sizeof png);
        SP_ASSERT(back->picture != NULL);
        if (back->picture)
            SP_ASSERT(memcmp(back->picture, png, sizeof png) == 0);
        SP_ASSERT_STR(back->picture_type, "png");
    }
    close_all();
}

/* A contact with no picture reads back as no picture, not as an empty
   one: a zero-length blob and NULL are different answers to "is there
   a picture", and the widget asks that question. */
SP_TEST(a_contact_without_a_picture_reads_back_null) {
    open_fresh();
    sp_contact_number n = {0};
    n.uri = "1002";
    sp_contact c;
    memset(&c, 0, sizeof c);
    c.name = "Grace";
    c.numbers = &n;
    c.n_numbers = 1;

    int64_t id = 0;
    SP_ASSERT(sp_contacts_save(g_c, &c, &id) == SP_OK);
    const sp_contact *back = NULL;
    SP_ASSERT(sp_contacts_get(g_c, id, &back) == SP_OK);
    if (back) {
        SP_ASSERT(back->picture == NULL);
        SP_ASSERT(back->picture_len == 0);
    }
    close_all();
}

/* The upper bound the format and the size together should never
   reach, kept as the thing that catches it if they do. */
SP_TEST(a_picture_over_the_limit_is_refused) {
    open_fresh();
    static unsigned char big[520 * 1024];
    memcpy(big, "\x89PNG\r\n\x1a\n", 8);
    sp_contact_number n = {0};
    n.uri = "1003";
    sp_contact c;
    memset(&c, 0, sizeof c);
    c.name = "Too Big";
    c.numbers = &n;
    c.n_numbers = 1;
    c.picture = big;
    c.picture_len = sizeof big;
    c.picture_type = "png";

    int64_t id = 0;
    SP_ASSERT(sp_contacts_save(g_c, &c, &id) == SP_ERR_INVALID_ARG);
    close_all();
}

/* PNG is the only thing this stores. A type it cannot draw later is
   refused now rather than discovered by a widget. */
SP_TEST(a_picture_type_other_than_png_is_refused) {
    open_fresh();
    unsigned char bytes[64] = { 0xff, 0xd8 };
    sp_contact_number n = {0};
    n.uri = "1004";
    sp_contact c;
    memset(&c, 0, sizeof c);
    c.name = "Jay Peg";
    c.numbers = &n;
    c.n_numbers = 1;
    c.picture = bytes;
    c.picture_len = sizeof bytes;
    c.picture_type = "jpeg";

    int64_t id = 0;
    SP_ASSERT(sp_contacts_save(g_c, &c, &id) == SP_ERR_INVALID_ARG);
    close_all();
}

/* picture and picture_len disagreeing about whether there is a
   picture is a caller bug, not a "no picture": a NULL pointer with a
   length says there is data nobody handed over, and must be refused
   rather than silently stored as no picture. */
SP_TEST(a_null_picture_with_a_nonzero_length_is_refused) {
    open_fresh();
    sp_contact_number n = {0};
    n.uri = "1006";
    sp_contact c;
    memset(&c, 0, sizeof c);
    c.name = "Mismatched One";
    c.numbers = &n;
    c.n_numbers = 1;
    c.picture = NULL;
    c.picture_len = 128;
    c.picture_type = "png";

    int64_t id = 0;
    SP_ASSERT(sp_contacts_save(g_c, &c, &id) == SP_ERR_INVALID_ARG);
    close_all();
}

/* The other half of the same pair: a real pointer with a zero length
   is just as inconsistent, and just as much a caller bug. */
SP_TEST(a_picture_with_a_zero_length_is_refused) {
    open_fresh();
    unsigned char png[64];
    memcpy(png, "\x89PNG\r\n\x1a\n", 8);
    sp_contact_number n = {0};
    n.uri = "1007";
    sp_contact c;
    memset(&c, 0, sizeof c);
    c.name = "Mismatched Two";
    c.numbers = &n;
    c.n_numbers = 1;
    c.picture = png;
    c.picture_len = 0;
    c.picture_type = "png";

    int64_t id = 0;
    SP_ASSERT(sp_contacts_save(g_c, &c, &id) == SP_ERR_INVALID_ARG);
    close_all();
}

/* The list read leaves pictures out deliberately: it exists to fill a
   scrolling view, and a book of two hundred could otherwise put a
   hundred megabytes through one call. */
SP_TEST(the_list_read_omits_pictures) {
    open_fresh();
    unsigned char png[128];
    memcpy(png, "\x89PNG\r\n\x1a\n", 8);
    sp_contact_number n = {0};
    n.uri = "1005";
    sp_contact c;
    memset(&c, 0, sizeof c);
    c.name = "Listed";
    c.numbers = &n;
    c.n_numbers = 1;
    c.picture = png;
    c.picture_len = sizeof png;
    c.picture_type = "png";

    int64_t id = 0;
    SP_ASSERT(sp_contacts_save(g_c, &c, &id) == SP_OK);

    const sp_contact *rows = NULL;
    size_t n_rows = 0;
    SP_ASSERT(sp_contacts_query(g_c, "Listed", 10, &rows, &n_rows) == SP_OK);
    SP_ASSERT(n_rows == 1);
    if (n_rows == 1) {
        SP_ASSERT(rows[0].picture == NULL);
        SP_ASSERT(rows[0].picture_len == 0);
    }

    /* ...and the dedicated accessor is how the delegate gets it. */
    const void *data = NULL;
    size_t len = 0;
    const char *type = NULL;
    SP_ASSERT(sp_contacts_picture(g_c, id, &data, &len, &type) == SP_OK);
    SP_ASSERT(len == sizeof png);
    SP_ASSERT(data != NULL);
    SP_ASSERT_STR(type, "png");
    close_all();
}

SP_TEST(the_picture_accessor_rejects_an_unknown_id) {
    open_fresh();
    const void *data = NULL;
    size_t len = 0;
    const char *type = NULL;
    SP_ASSERT(sp_contacts_picture(g_c, 999999, &data, &len, &type)
              == SP_ERR_NOT_FOUND);
    close_all();
}

int main(void) {
    SP_RUN(two_spellings_of_one_number_normalise_together);
    SP_RUN(an_extension_normalises_to_itself);
    SP_RUN(something_with_no_digits_in_it_normalises_to_nothing);
    SP_RUN(a_contact_comes_back_the_way_it_went_in);
    SP_RUN(a_contact_that_cannot_be_dialled_is_refused);
    SP_RUN(saving_again_replaces_the_numbers_rather_than_adding_to_them);
    SP_RUN(updating_something_that_is_not_there_says_so);
    SP_RUN(a_call_finds_its_contact_however_the_number_is_written);
    SP_RUN(a_number_nobody_has_resolves_to_nobody);
    SP_RUN(an_extension_never_borrows_a_citizens_name);
    SP_RUN(deleting_takes_the_numbers_with_it);
    SP_RUN(the_list_puts_favourites_first_then_names_in_order);
    SP_RUN(search_reaches_the_name_the_company_and_the_number);
    SP_RUN(a_search_for_a_wildcard_is_a_search_for_a_wildcard);
    SP_RUN(a_lookup_does_not_free_the_list_someone_is_reading);
    SP_RUN(a_picture_round_trips_unchanged);
    SP_RUN(a_contact_without_a_picture_reads_back_null);
    SP_RUN(a_picture_over_the_limit_is_refused);
    SP_RUN(a_picture_type_other_than_png_is_refused);
    SP_RUN(a_null_picture_with_a_nonzero_length_is_refused);
    SP_RUN(a_picture_with_a_zero_length_is_refused);
    SP_RUN(the_list_read_omits_pictures);
    SP_RUN(the_picture_accessor_rejects_an_unknown_id);
    return SP_TEST_RESULT();
}
