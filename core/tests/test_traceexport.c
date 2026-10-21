#include "sp_test.h"
#include "traceexport.h"
#include "comlink/core.h"
#include <dirent.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Fixture for the sp_trace_export wrapper itself, as opposed to the
   pure functions above: a core with a real SIP log, same shape as
   test_siplog.c's and test_recording.c's fixtures. */
static const char *cfg_with(const char *name, const char *extra) {
    static char p[512];
    snprintf(p, sizeof p, "%s/%s.yaml", getenv("SP_TEST_TMP"), name);
    char db[512];
    snprintf(db, sizeof db, "%s/%s.db", getenv("SP_TEST_TMP"), name);
    unlink(db);
    FILE *f = fopen(p, "w");
    fprintf(f, "log_level: 0\nsip_port: 0\naudio_device: none\n"
               "db_file: %s\n%s"
               "accounts:\n  - id: t\n    user: 1\n    domain: 127.0.0.1\n"
               "    register: false\n", db, extra);
    fclose(f);
    return p;
}

/* A call attempt to a dead port still produces a real INVITE, so the
   log fills without needing a peer. */
static void make_traffic(sp_core *c) {
    sp_call_id call = 0;
    sp_call_make(c, 1, "sip:x@127.0.0.1:65530", NULL, 0, &call);
    usleep(600000);
}

/* Oldest first. sp_siplog_read hands back newest-first because that is
   what a live view wants; somebody reading a trace to understand a
   call reads it in the order it happened. */
SP_TEST(the_export_runs_oldest_first) {
    const sp_sip_message msgs[] = {
        { 2000, "rx", NULL, 200,   "abc", NULL, true, "SIP/2.0 200 OK\r\n" },
        { 1000, "tx", "INVITE", 0, "abc", NULL, true, "INVITE sip:1@x\r\n" },
    };
    char out[4096];
    const int n = sp_trace_format(out, sizeof out, msgs, 2, "abc");
    SP_ASSERT(n > 0);
    const char *inv = strstr(out, "INVITE");
    const char *ok  = strstr(out, "200 OK");
    SP_ASSERT(inv != NULL && ok != NULL);
    if (inv && ok)
        SP_ASSERT(inv < ok);
}

/* The header names the call and says how many messages are in the
   file, so a truncated forward is obvious to whoever receives it. */
SP_TEST(the_header_names_the_call_and_the_count) {
    const sp_sip_message m = { 1000, "tx", "INVITE", 0, "xyz", NULL,
                               true, "INVITE sip:1@x\r\n" };
    char out[4096];
    SP_ASSERT(sp_trace_format(out, sizeof out, &m, 1, "xyz") > 0);
    SP_ASSERT(strstr(out, "xyz") != NULL);
    SP_ASSERT(strstr(out, "1 message") != NULL);
}

/* The whole point. A trace captured with redaction off carries
   Authorization headers in cleartext, and a file is easier to forward
   than a database. Whoever opens it has to meet that fact first. */
SP_TEST(an_unredacted_trace_says_so_at_the_top) {
    const sp_sip_message m = { 1000, "tx", "REGISTER", 0, "u", NULL,
                               false, "REGISTER sip:x\r\nAuthorization: X\r\n" };
    char out[4096];
    SP_ASSERT(sp_trace_format(out, sizeof out, &m, 1, "u") > 0);
    const char *warn = strstr(out, "NOT REDACTED");
    SP_ASSERT(warn != NULL);
    /* At the top, not the bottom: a warning below the messages is a
       warning read after the file has been sent. */
    if (warn)
        SP_ASSERT(warn - out < 400);
}

/* A fully redacted trace must NOT carry the warning, or the warning
   stops meaning anything. */
SP_TEST(a_redacted_trace_carries_no_warning) {
    const sp_sip_message m = { 1000, "tx", "REGISTER", 0, "r", NULL,
                               true, "REGISTER sip:x\r\n" };
    char out[4096];
    SP_ASSERT(sp_trace_format(out, sizeof out, &m, 1, "r") > 0);
    SP_ASSERT(strstr(out, "NOT REDACTED") == NULL);
}

/* One unredacted message among many is still an unredacted file. */
SP_TEST(one_unredacted_message_warns_for_the_whole_file) {
    const sp_sip_message msgs[] = {
        { 1000, "tx", "INVITE", 0, "m", NULL, true,  "INVITE sip:1@x\r\n" },
        { 2000, "tx", "ACK",    0, "m", NULL, false, "ACK sip:1@x\r\n" },
    };
    char out[4096];
    SP_ASSERT(sp_trace_format(out, sizeof out, msgs, 2, "m") > 0);
    SP_ASSERT(strstr(out, "NOT REDACTED") != NULL);
}

/* Truncation is a refusal, not a short file. A trace that stops
   mid-message looks complete to whoever receives it. */
SP_TEST(a_buffer_too_small_is_refused_not_truncated) {
    const sp_sip_message m = { 1000, "tx", "INVITE", 0, "t", NULL, true,
                               "INVITE sip:1@example.invalid SIP/2.0\r\n" };
    char out[16];
    SP_ASSERT(sp_trace_format(out, sizeof out, &m, 1, "t") == -1);
}

/* The exact boundary, pinned rather than approximated: a buffer sized
   to precisely what the format needs (its returned length, plus the
   one byte for the trailing NUL that sp_trace_format's own contract
   never truncates away) must succeed, and the same buffer one byte
   shorter must be refused. append()'s byte-exact accounting is the
   thing this codebase has gotten wrong four times across two
   branches, and the too-small test above never reaches it -- it
   overflows on the header alone, before the per-message loop that
   does the real work of append() ever runs. The size is derived from
   sp_trace_format's own return value on a generous buffer, so this
   cannot drift if the format changes. */
static int exact_fit_length(void) {
    const sp_sip_message msgs[] = {
        { 1000, "tx", "INVITE", 0, "eb", NULL, true,
          "INVITE sip:1@example.invalid SIP/2.0\r\n" },
        { 2000, "rx", NULL, 200, "eb", NULL, true,
          "SIP/2.0 200 OK\r\n" },
    };
    char big[4096];
    return sp_trace_format(big, sizeof big, msgs, 2, "eb");
}

SP_TEST(a_buffer_exactly_the_needed_size_succeeds) {
    const sp_sip_message msgs[] = {
        { 1000, "tx", "INVITE", 0, "eb", NULL, true,
          "INVITE sip:1@example.invalid SIP/2.0\r\n" },
        { 2000, "rx", NULL, 200, "eb", NULL, true,
          "SIP/2.0 200 OK\r\n" },
    };
    const int need = exact_fit_length();
    SP_ASSERT(need > 0);
    if (need <= 0) return;

    char *exact = malloc((size_t)need + 1);
    SP_ASSERT(exact != NULL);
    if (!exact) return;
    SP_ASSERT(sp_trace_format(exact, (size_t)need + 1, msgs, 2, "eb")
              == need);
    free(exact);
}

SP_TEST(a_buffer_one_byte_short_of_that_is_refused) {
    const sp_sip_message msgs[] = {
        { 1000, "tx", "INVITE", 0, "eb", NULL, true,
          "INVITE sip:1@example.invalid SIP/2.0\r\n" },
        { 2000, "rx", NULL, 200, "eb", NULL, true,
          "SIP/2.0 200 OK\r\n" },
    };
    const int need = exact_fit_length();
    SP_ASSERT(need > 0);
    if (need <= 0) return;

    char *tight = malloc((size_t)need);
    SP_ASSERT(tight != NULL);
    if (!tight) return;
    SP_ASSERT(sp_trace_format(tight, (size_t)need, msgs, 2, "eb") == -1);
    free(tight);
}

SP_TEST(no_messages_is_refused) {
    char out[4096];
    SP_ASSERT(sp_trace_format(out, sizeof out, NULL, 0, "none") == -1);
}

/* A Call-ID comes from the far end. It reaches a filename, so it is
   sanitised first -- a peer that sends "../../etc/x" as its Call-ID
   must not choose where this file lands. */
SP_TEST(a_call_id_cannot_escape_the_export_directory) {
    char out[256];
    SP_ASSERT(sp_trace_safe_name(out, sizeof out, "../../etc/passwd") == 0);
    SP_ASSERT(strstr(out, "..") == NULL);
    SP_ASSERT(strchr(out, '/') == NULL);
}

/* An empty Call-ID is degenerate on its own terms, before it ever
   reaches the escaping question above. */
SP_TEST(an_empty_call_id_is_refused) {
    char out[256];
    SP_ASSERT(sp_trace_safe_name(out, sizeof out, "") == -1);
}

/* A Call-ID that is nothing but dots must not be handed through
   unchanged either -- "." and ".." are both meaningful to a
   filesystem, and neither belongs in a name this code composes. */
SP_TEST(a_call_id_that_is_all_dots_is_replaced) {
    char out[256];
    SP_ASSERT(sp_trace_safe_name(out, sizeof out, ".") == 0);
    SP_ASSERT(strcmp(out, ".") != 0);
    SP_ASSERT(strchr(out, '.') == NULL);
}

/* The wrapper's own job, beyond formatting: read the log, resolve the
   directory, compose the filename and write the file. Assert on what
   landed on disk, not merely that something did -- a zero-byte file
   would also pass a bare existence check. */
SP_TEST(exporting_writes_a_file_matching_the_format) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_with("te1", ""), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);
    make_traffic(c);

    const sp_sip_message *msgs = NULL;
    size_t n = 0;
    SP_ASSERT(sp_siplog_read(c, NULL, 500, &msgs, &n) == SP_OK);
    SP_ASSERT(n >= 1);
    /* Copied out before the next sp_siplog_read call: msgs[0].call_id
       points into the core-owned buffer that call is about to
       overwrite, and passing it back in as that call's own filter
       argument would read it out from under itself. */
    char cidbuf[256] = "";
    if (n >= 1 && msgs[0].call_id)
        snprintf(cidbuf, sizeof cidbuf, "%s", msgs[0].call_id);
    const char *cid = cidbuf[0] ? cidbuf : NULL;
    SP_ASSERT(cid != NULL);
    if (!cid) {
        sp_core_stop(c);
        sp_core_destroy(c);
        return;
    }

    /* Same query sp_trace_export runs internally, so the expected
       text is built from the same messages the export itself sees. */
    const sp_sip_message *fmsgs = NULL;
    size_t fn = 0;
    SP_ASSERT(sp_siplog_read(c, cid, 100000, &fmsgs, &fn) == SP_OK);
    SP_ASSERT(fn >= 1);
    char expected[16384];
    const int elen =
        sp_trace_format(expected, sizeof expected, fmsgs, fn, cid);
    SP_ASSERT(elen > 0);

    const char *path = NULL;
    SP_ASSERT(sp_trace_export(c, cid, &path) == SP_OK);
    SP_ASSERT(path != NULL);

    FILE *f = path ? fopen(path, "rb") : NULL;
    SP_ASSERT(f != NULL);
    char actual[16384] = {0};
    size_t got = f ? fread(actual, 1, sizeof actual - 1, f) : 0;
    if (f)
        fclose(f);
    SP_ASSERT(got > 0);

    /* Compare everything past the header's second line, which is the
       only line carrying a wall-clock stamp that could in principle
       differ between the two sp_trace_format calls above; the
       messages -- the substance of the file -- must match exactly. */
    const char *exp_body = strstr(expected, "\n\n");
    const char *act_body = strstr(actual, "\n\n");
    SP_ASSERT(exp_body != NULL && act_body != NULL);
    if (exp_body && act_body)
        SP_ASSERT(strcmp(exp_body, act_body) == 0);

    sp_core_stop(c);
    sp_core_destroy(c);
}

/* No messages under that Call-ID means nothing to export -- and
   nothing written, not an empty file left behind. */
SP_TEST(an_unknown_call_id_is_not_found_and_writes_nothing) {
    /* Its own export directory: the default one sits beside the
       config file, and every cfg_with() in this file shares that
       same top-level SP_TEST_TMP, so an earlier test's export would
       otherwise still be sitting there when this one counts files. */
    char want[512];
    snprintf(want, sizeof want, "%s/te2_traces", getenv("SP_TEST_TMP"));
    char extra[576];
    snprintf(extra, sizeof extra, "sip_trace:\n  export_dir: %s\n", want);

    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_with("te2", extra), &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_core_start(c) == SP_OK);

    const char *dir = NULL;
    SP_ASSERT(sp_trace_get_export_dir(c, &dir) == SP_OK);
    SP_ASSERT(dir != NULL);

    const char *path = NULL;
    SP_ASSERT(sp_trace_export(c, "no-such-call-id", &path)
              == SP_ERR_NOT_FOUND);

    DIR *d = dir ? opendir(dir) : NULL;
    SP_ASSERT(d != NULL);
    int count = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
            if (e->d_name[0] != '.')
                count++;
        closedir(d);
    }
    SP_ASSERT(count == 0);

    sp_core_stop(c);
    sp_core_destroy(c);
}

/* NULL and empty are both degenerate call_id, and a NULL path_out is
   degenerate on its own -- all rejected before any log read. */
SP_TEST(null_or_empty_arguments_are_rejected) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_with("te3", ""), &c) == SP_OK);
    if (!c) return;
    const char *path = NULL;
    SP_ASSERT(sp_trace_export(c, NULL, &path) == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_trace_export(c, "", &path) == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_trace_export(c, "abc", NULL) == SP_ERR_INVALID_ARG);
    sp_core_destroy(c);
}

/* A Call-ID of 256 bytes or more does not fit compose_trace_path's
   safe[256] scratch buffer. That is a bad Call-ID, not a disk
   problem, so it must come back as SP_ERR_INVALID_ARG rather than
   SP_ERR_IO -- the caller needs to know which of those it is dealing
   with. Reaching compose_trace_path at all needs sp_siplog_read to
   find a message first, so a row with that over-long Call-ID is
   inserted directly into the store -- the same table and columns
   siplog.c's own insert uses -- rather than generated through a live
   call, which has no way to choose its own Call-ID. The core is
   created but never started: sp_store_open runs at create time, and
   an unstarted core has no signalling thread to contend with this
   connection for the database. */
SP_TEST(an_overlong_call_id_is_invalid_not_an_io_error) {
    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(cfg_with("te4", ""), &c) == SP_OK);
    if (!c) return;

    char db[512];
    snprintf(db, sizeof db, "%s/te4.db", getenv("SP_TEST_TMP"));

    char long_id[300];
    memset(long_id, 'x', sizeof long_id - 1);
    long_id[sizeof long_id - 1] = '\0';

    sqlite3 *conn = NULL;
    SP_ASSERT(sqlite3_open(db, &conn) == SQLITE_OK);
    if (conn) {
        static const char *SQL =
            "INSERT INTO sip_message"
            "(ts_us, direction, method, status_code, call_id, account,"
            " redacted, raw) VALUES(1000, 'tx', 'INVITE', 0, ?, 't', 1,"
            " 'INVITE sip:1@x\r\n');";
        sqlite3_stmt *st = NULL;
        SP_ASSERT(sqlite3_prepare_v2(conn, SQL, -1, &st, NULL)
                  == SQLITE_OK);
        if (st) {
            sqlite3_bind_text(st, 1, long_id, -1, SQLITE_STATIC);
            SP_ASSERT(sqlite3_step(st) == SQLITE_DONE);
            sqlite3_finalize(st);
        }
        sqlite3_close(conn);
    }

    const char *path = NULL;
    SP_ASSERT(sp_trace_export(c, long_id, &path) == SP_ERR_INVALID_ARG);
    sp_core_destroy(c);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) {
        printf("SP_TEST_TMP unset\n");
        return 1;
    }
    SP_RUN(the_export_runs_oldest_first);
    SP_RUN(the_header_names_the_call_and_the_count);
    SP_RUN(an_unredacted_trace_says_so_at_the_top);
    SP_RUN(a_redacted_trace_carries_no_warning);
    SP_RUN(one_unredacted_message_warns_for_the_whole_file);
    SP_RUN(a_buffer_too_small_is_refused_not_truncated);
    SP_RUN(a_buffer_exactly_the_needed_size_succeeds);
    SP_RUN(a_buffer_one_byte_short_of_that_is_refused);
    SP_RUN(no_messages_is_refused);
    SP_RUN(a_call_id_cannot_escape_the_export_directory);
    SP_RUN(an_empty_call_id_is_refused);
    SP_RUN(a_call_id_that_is_all_dots_is_replaced);
    SP_RUN(exporting_writes_a_file_matching_the_format);
    SP_RUN(an_unknown_call_id_is_not_found_and_writes_nothing);
    SP_RUN(null_or_empty_arguments_are_rejected);
    SP_RUN(an_overlong_call_id_is_invalid_not_an_io_error);
    return SP_TEST_RESULT();
}
