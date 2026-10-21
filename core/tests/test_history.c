#include "sp_test.h"
#include "comlink/core.h"
#include "history.h"
#include "store.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static const char *cfg(const char *name) {
    static char p[512];
    snprintf(p, sizeof p, "%s/%s.yaml", getenv("SP_TEST_TMP"), name);
    char db[512];
    snprintf(db, sizeof db, "%s/%s.db", getenv("SP_TEST_TMP"), name);
    unlink(db);
    FILE *f = fopen(p, "w");
    fprintf(f, "log_level: 0\nsip_port: 0\naudio_device: none\n"
               "db_file: %s\n"
               "accounts:\n  - id: t\n    user: 1\n    domain: 127.0.0.1\n"
               "    register: false\n", db);
    fclose(f);
    return p;
}

/* Wait for a condition, not a duration: records are completed from
   events, which arrive when the stack decides. */
static size_t wait_for_history(sp_core *c, size_t want, int max_ms) {
    const sp_call_record *r = NULL;
    size_t n = 0;
    for (int i = 0; i < max_ms; i += 20) {
        if (sp_history_read(c, 100, &r, &n) == SP_OK && n >= want)
            return n;
        usleep(20000);
    }
    return n;
}

SP_TEST(a_placed_call_produces_one_outgoing_record) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg("h1"), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);

    sp_call_id call = 0;
    SP_ASSERT(sp_call_make(c, 1, "sip:x@127.0.0.1:65530", NULL, 0, &call)
              == SP_OK);
    SP_ASSERT(wait_for_history(c, 1, 5000) >= 1);

    const sp_call_record *r = NULL; size_t n = 0;
    SP_ASSERT(sp_history_read(c, 100, &r, &n) == SP_OK);
    SP_ASSERT(n == 1);
    if (n == 1) {
        SP_ASSERT(r[0].inbound == false);
        SP_ASSERT(r[0].start_us > 0);
        SP_ASSERT(r[0].id > 0);
        SP_ASSERT(r[0].remote_uri != NULL);
    }
    sp_core_stop(c); sp_core_destroy(c);
}

/* The record must be completed, not merely created: a history of calls
   that never end is useless. */
SP_TEST(a_finished_call_records_its_disconnect_code) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg("h2"), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    sp_call_id call = 0;
    SP_ASSERT(sp_call_make(c, 1, "sip:x@127.0.0.1:65530", NULL, 0, &call)
              == SP_OK);
    usleep(300000);
    sp_call_hangup(c, call, 486);

    int code = 0;
    for (int i = 0; i < 250; i++) {
        const sp_call_record *r = NULL; size_t n = 0;
        if (sp_history_read(c, 10, &r, &n) == SP_OK && n >= 1 &&
            r[0].end_us > 0) {
            code = r[0].disconnect_code;
            break;
        }
        usleep(20000);
    }
    SP_ASSERT(code != 0);
    sp_core_stop(c); sp_core_destroy(c);
}

/* The join is the whole point: a record must lead to its own trace. */
SP_TEST(a_record_joins_to_its_sip_messages) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg("h3"), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    sp_call_id call = 0;
    SP_ASSERT(sp_call_make(c, 1, "sip:x@127.0.0.1:65530", NULL, 0, &call)
              == SP_OK);
    SP_ASSERT(wait_for_history(c, 1, 5000) >= 1);

    /* Copy the Call-ID out before another read invalidates it. */
    char cid[256] = {0};
    for (int i = 0; i < 250 && !cid[0]; i++) {
        const sp_call_record *r = NULL; size_t n = 0;
        if (sp_history_read(c, 10, &r, &n) == SP_OK && n >= 1 &&
            r[0].sip_call_id && r[0].sip_call_id[0])
            snprintf(cid, sizeof cid, "%s", r[0].sip_call_id);
        else
            usleep(20000);
    }
    SP_ASSERT(cid[0] != '\0');

    if (cid[0]) {
        const sp_sip_message *m = NULL; size_t mn = 0;
        SP_ASSERT(sp_siplog_read(c, cid, 50, &m, &mn) == SP_OK);
        SP_ASSERT(mn >= 1);
        for (size_t i = 0; i < mn; i++)
            SP_ASSERT_STR(m[i].call_id, cid);
    }
    sp_core_stop(c); sp_core_destroy(c);
}

SP_TEST(history_survives_a_restart) {
    const char *p = cfg("h4");
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    sp_call_id call = 0;
    sp_call_make(c, 1, "sip:x@127.0.0.1:65530", NULL, 0, &call);
    SP_ASSERT(wait_for_history(c, 1, 5000) >= 1);
    sp_core_stop(c); sp_core_destroy(c);

    sp_core *c2 = NULL;
    SP_ASSERT(sp_core_create(p, &c2) == SP_OK);
    if (!c2) return;
    const sp_call_record *r = NULL; size_t n = 0;
    SP_ASSERT(sp_history_read(c2, 100, &r, &n) == SP_OK);
    SP_ASSERT(n >= 1);
    sp_core_destroy(c2);
}

SP_TEST(clearing_history_works) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg("h5"), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    sp_call_id call = 0;
    sp_call_make(c, 1, "sip:x@127.0.0.1:65530", NULL, 0, &call);
    wait_for_history(c, 1, 5000);
    SP_ASSERT(sp_history_clear(c) == SP_OK);
    const sp_call_record *r = NULL; size_t n = 99;
    SP_ASSERT(sp_history_read(c, 100, &r, &n) == SP_OK);
    SP_ASSERT(n == 0);
    sp_core_stop(c); sp_core_destroy(c);
}

SP_TEST(bad_arguments_are_rejected) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg("h6"), &c) == SP_OK);
    if (!c) return;
    size_t n = 0;
    SP_ASSERT(sp_history_read(c, 10, NULL, &n) == SP_ERR_INVALID_ARG);
    const sp_call_record *r = NULL;
    SP_ASSERT(sp_history_read(c, 10, &r, NULL) == SP_ERR_INVALID_ARG);
    sp_core_destroy(c);
}

SP_TEST(sort_order_reverses_the_history_and_is_not_a_reversed_page) {
    const char *p = cfg("h6");
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);

    /* Three calls to distinguishable addresses, placed in order. */
    for (int i = 0; i < 3; i++) {
        sp_call_id call = 0;
        char uri[64];
        snprintf(uri, sizeof uri, "sip:seq%d@127.0.0.1:65530", i);
        sp_call_make(c, 1, uri, NULL, 0, &call);
    }
    SP_ASSERT(wait_for_history(c, 3, 8000) >= 3);

    const sp_call_record *r = NULL;
    size_t n = 0;

    /* Newest first is the default. */
    SP_ASSERT(sp_log_get_order(c) == SP_ORDER_NEWEST_FIRST);
    SP_ASSERT(sp_history_read(c, 100, &r, &n) == SP_OK);
    SP_ASSERT(n >= 3);
    if (n < 3) { sp_core_stop(c); sp_core_destroy(c); return; }
    const int64_t newest_first_top = r[0].id;
    const int64_t newest_first_end = r[n - 1].id;
    SP_ASSERT(newest_first_top > newest_first_end);

    /* Oldest first turns it round. */
    SP_ASSERT(sp_log_set_order(c, SP_ORDER_OLDEST_FIRST) == SP_OK);
    SP_ASSERT(sp_log_get_order(c) == SP_ORDER_OLDEST_FIRST);
    SP_ASSERT(sp_history_read(c, 100, &r, &n) == SP_OK);
    SP_ASSERT(n >= 3);
    if (n < 3) { sp_core_stop(c); sp_core_destroy(c); return; }
    SP_ASSERT(r[0].id < r[n - 1].id);
    SP_ASSERT(r[0].id == newest_first_end);

    /* The part that a frontend-side reverse would get wrong: with the
       read capped at one row, "oldest first" must return the OLDEST
       row, not the newest one. That is the whole reason this lives in
       SQL rather than in a list widget. */
    SP_ASSERT(sp_history_read(c, 1, &r, &n) == SP_OK);
    SP_ASSERT(n == 1);
    if (n == 1)
        SP_ASSERT(r[0].id == newest_first_end);

    SP_ASSERT(sp_log_set_order(c, SP_ORDER_NEWEST_FIRST) == SP_OK);
    SP_ASSERT(sp_history_read(c, 1, &r, &n) == SP_OK);
    SP_ASSERT(n == 1);
    if (n == 1)
        SP_ASSERT(r[0].id == newest_first_top);

    /* A nonsense order is rejected. */
    SP_ASSERT(sp_log_set_order(c, (sp_order)99) == SP_ERR_INVALID_ARG);

    sp_core_stop(c);
    sp_core_destroy(c);
}

/* --- transcripts, at the store layer --------------------------------- */

static sp_store *g_tr_store;

static sp_history *fresh_history(const char *name) {
    char db[512], err[256] = {0};
    snprintf(db, sizeof db, "%s/%s.db", getenv("SP_TEST_TMP"), name);
    unlink(db);
    g_tr_store = sp_store_open(db, err, sizeof err);
    if (!g_tr_store) { printf("  store: %s\n", err); return NULL; }
    return sp_history_create(g_tr_store, 1000);
}

static void done_history(sp_history *h) {
    sp_history_destroy(h);
    sp_store_close(g_tr_store);
    g_tr_store = NULL;
}

SP_TEST(a_transcript_round_trips_in_order) {
    sp_history *h = fresh_history("tr_rt");
    if (!h) { SP_ASSERT(0); return; }
    int64_t id = sp_history_begin(h, true, "a", "sip:1@x", "sip:2@x", "cid-tr");
    SP_ASSERT(id > 0);
    const sp_transcript_line in[] = {
        { 0,    900,  false, "Alice", "Bonjour." },
        { 1000, 1800, true,  "Me",    "Allo, oui." },
    };
    SP_ASSERT(sp_history_transcript_store(h, id, in, 2) == 0);

    const sp_transcript_line *out = NULL; size_t n = 0;
    SP_ASSERT(sp_history_transcript_read(h, id, &out, &n) == SP_OK);
    SP_ASSERT(n == 2);
    if (n == 2) {
        SP_ASSERT_STR(out[0].text, "Bonjour.");
        SP_ASSERT_STR(out[0].speaker, "Alice");
        SP_ASSERT(out[0].local == false);
        SP_ASSERT(out[1].local == true);
        SP_ASSERT(out[1].start_ms == 1000);
        SP_ASSERT(out[1].end_ms == 1800);
    }
    const sp_call_record *rows = NULL; size_t nr = 0;
    SP_ASSERT(sp_history_query(h, 10, &rows, &nr) == SP_OK);
    SP_ASSERT(nr >= 1 && rows[0].transcript == SP_TRANSCRIPT_READY);
    done_history(h);
}

SP_TEST(storing_again_replaces_the_lines) {
    sp_history *h = fresh_history("tr_again");
    if (!h) { SP_ASSERT(0); return; }
    int64_t id = sp_history_begin(h, true, "a", "sip:1@x", "sip:2@x", "cid-a");
    const sp_transcript_line one[] = { { 0, 10, true, "Me", "first" } };
    const sp_transcript_line two[] = { { 0, 10, true, "Me", "second" } };
    SP_ASSERT(sp_history_transcript_store(h, id, one, 1) == 0);
    SP_ASSERT(sp_history_transcript_store(h, id, two, 1) == 0);
    const sp_transcript_line *out = NULL; size_t n = 0;
    SP_ASSERT(sp_history_transcript_read(h, id, &out, &n) == SP_OK);
    SP_ASSERT(n == 1);
    if (n == 1) SP_ASSERT_STR(out[0].text, "second");
    done_history(h);
}

SP_TEST(clearing_history_removes_its_transcripts) {
    sp_history *h = fresh_history("tr_wipe");
    if (!h) { SP_ASSERT(0); return; }
    int64_t id = sp_history_begin(h, true, "a", "sip:1@x", "sip:2@x", "cid-w");
    const sp_transcript_line in[] = { { 0, 500, true, "Me", "x" } };
    SP_ASSERT(sp_history_transcript_store(h, id, in, 1) == 0);
    SP_ASSERT(sp_history_wipe(h) == SP_OK);
    const sp_transcript_line *out = NULL; size_t n = 99;
    SP_ASSERT(sp_history_transcript_read(h, id, &out, &n) == SP_OK);
    SP_ASSERT(n == 0);
    done_history(h);
}

SP_TEST(pending_calls_are_listed_oldest_first) {
    sp_history *h = fresh_history("tr_pend");
    if (!h) { SP_ASSERT(0); return; }
    int64_t a = sp_history_begin(h, false, "a", "sip:1@x", "sip:2@x", "cid-p1");
    int64_t done = sp_history_begin(h, false, "a", "sip:1@x", "sip:3@x", "cid-p2");
    int64_t b = sp_history_begin(h, false, "a", "sip:1@x", "sip:4@x", "cid-p3");
    sp_history_transcript_state(h, a, SP_TRANSCRIPT_PENDING);
    sp_history_transcript_state(h, b, SP_TRANSCRIPT_PENDING);
    sp_history_transcript_failed(h, done, "x");
    sp_pending_call *p = NULL;
    size_t n = 0;
    SP_ASSERT(sp_history_transcript_pending(h, &p, &n) == 0);
    SP_ASSERT(n == 2);
    if (n == 2) {
        SP_ASSERT(p[0].id == a && p[1].id == b);
        SP_ASSERT_STR(p[1].remote_uri, "sip:1@x");
    }
    sp_history_pending_free(p, n);
    done_history(h);
}

SP_TEST(a_long_transcript_round_trips_whole) {
    sp_history *h = fresh_history("tr_long");
    if (!h) { SP_ASSERT(0); return; }
    int64_t id = sp_history_begin(h, true, "a", "sip:1@x", "sip:2@x", "cid-l");
    enum { N = 1000 };
    static sp_transcript_line in[N];
    static char text[N][16];
    for (int i = 0; i < N; i++) {
        snprintf(text[i], sizeof text[i], "line %d", i);
        in[i] = (sp_transcript_line){ (uint32_t)i * 10, (uint32_t)i * 10 + 5,
                                      i % 2 == 0, "Me", text[i] };
    }
    SP_ASSERT(sp_history_transcript_store(h, id, in, N) == 0);
    const sp_transcript_line *out = NULL; size_t n = 0;
    SP_ASSERT(sp_history_transcript_read(h, id, &out, &n) == SP_OK);
    SP_ASSERT(n == N);
    if (n == N) {
        SP_ASSERT_STR(out[0].text, "line 0");
        SP_ASSERT_STR(out[N - 1].text, "line 999");
        SP_ASSERT(out[401].start_ms == 4010 && out[401].local == false);
    }
    done_history(h);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(a_placed_call_produces_one_outgoing_record);
    SP_RUN(a_finished_call_records_its_disconnect_code);
    SP_RUN(a_record_joins_to_its_sip_messages);
    SP_RUN(history_survives_a_restart);
    SP_RUN(clearing_history_works);
    SP_RUN(bad_arguments_are_rejected);
    SP_RUN(sort_order_reverses_the_history_and_is_not_a_reversed_page);
    SP_RUN(a_transcript_round_trips_in_order);
    SP_RUN(storing_again_replaces_the_lines);
    SP_RUN(clearing_history_removes_its_transcripts);
    SP_RUN(pending_calls_are_listed_oldest_first);
    SP_RUN(a_long_transcript_round_trips_whole);
    return SP_TEST_RESULT();
}
