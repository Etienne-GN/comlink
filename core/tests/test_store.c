#include "sp_test.h"
#include "store.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static const char *db_path(const char *name) {
    static char p[512];
    snprintf(p, sizeof p, "%s/%s", getenv("SP_TEST_TMP"), name);
    unlink(p);
    return p;
}

/* Unlike sp_store_exec(), which reports success the moment a SELECT
   *runs* rather than the moment it matches anything, this reports what
   actually matched. A migration test asserting a row survived has to
   go through this, or a table that lost every row it was supposed to
   keep still reads as a passing test. -1 on a prepare or step error. */
static int row_count(sp_store *s, const char *sql) {
    sqlite3 *db = (sqlite3 *)sp_store_handle(s);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    int n = -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

SP_TEST(opening_creates_a_database_at_the_current_schema) {
    char err[256] = {0};
    sp_store *s = sp_store_open(db_path("store1.db"), err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) { printf("  %s\n", err); return; }
    SP_ASSERT(sp_store_schema_version(s) >= 1);
    sp_store_close(s);
}

SP_TEST(deleted_rows_do_not_linger_in_the_file) {
    char err[256] = {0};
    sp_store *s = sp_store_open(db_path("store-sd.db"), err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) return;
    /* A wiped history and its transcripts are personal information;
       without this SQLite only marks their pages free. */
    SP_ASSERT(row_count(s, "PRAGMA secure_delete;") == 1);
    sp_store_close(s);
}

SP_TEST(reopening_does_not_migrate_twice) {
    const char *p = db_path("store2.db");
    char err[256] = {0};
    sp_store *a = sp_store_open(p, err, sizeof err);
    SP_ASSERT(a != NULL);
    if (!a) return;
    int v = sp_store_schema_version(a);
    sp_store_close(a);

    sp_store *b = sp_store_open(p, err, sizeof err);
    SP_ASSERT(b != NULL);
    if (!b) return;
    SP_ASSERT(sp_store_schema_version(b) == v);
    sp_store_close(b);
}

SP_TEST(the_expected_tables_exist) {
    char err[256] = {0};
    sp_store *s = sp_store_open(db_path("store3.db"), err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) return;
    SP_ASSERT(sp_store_exec(s, "SELECT count(*) FROM sip_message;") == 0);
    SP_ASSERT(sp_store_exec(s, "SELECT count(*) FROM action_log;") == 0);
    SP_ASSERT(sp_store_exec(s, "SELECT count(*) FROM contact;") == 0);
    SP_ASSERT(sp_store_exec(s, "SELECT count(*) FROM contact_number;") == 0);
    sp_store_close(s);
}

SP_TEST(a_bad_path_fails_with_a_message_rather_than_a_crash) {
    char err[256] = {0};
    sp_store *s = sp_store_open("/proc/nope/x.db", err, sizeof err);
    SP_ASSERT(s == NULL);
    SP_ASSERT(err[0] != '\0');
}

SP_TEST(prune_keeps_the_newest_rows) {
    char err[256] = {0};
    sp_store *s = sp_store_open(db_path("store4.db"), err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) return;
    for (int i = 0; i < 50; i++)
        SP_ASSERT(sp_store_exec(s,
            "INSERT INTO action_log(ts_us, level, text) VALUES(1, 3, 'x');") == 0);
    SP_ASSERT(sp_store_prune(s, "action_log", 10) == 40);
    SP_ASSERT(sp_store_prune(s, "action_log", 10) == 0);
    sp_store_close(s);
}

/* A table name cannot be bound as a parameter, so it must never be
   taken from an untrusted string. */
SP_TEST(prune_rejects_a_table_outside_the_allowlist) {
    char err[256] = {0};
    sp_store *s = sp_store_open(db_path("store5.db"), err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) return;
    SP_ASSERT(sp_store_prune(s, "sqlite_master", 1) == -1);
    SP_ASSERT(sp_store_prune(s, "action_log; DROP TABLE sip_message", 1) == -1);
    /* The real table is still there. */
    SP_ASSERT(sp_store_exec(s, "SELECT count(*) FROM sip_message;") == 0);
    sp_store_close(s);
}

SP_TEST(exec_of_invalid_sql_reports_an_error) {
    char err[256] = {0};
    sp_store *s = sp_store_open(db_path("store6.db"), err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) return;
    SP_ASSERT(sp_store_exec(s, "THIS IS NOT SQL") == -1);
    SP_ASSERT(sp_store_error(s)[0] != '\0');
    sp_store_close(s);
}

SP_TEST(null_arguments_do_not_crash) {
    SP_ASSERT(sp_store_schema_version(NULL) == -1);
    SP_ASSERT(sp_store_exec(NULL, "SELECT 1;") == -1);
    SP_ASSERT(sp_store_prune(NULL, "action_log", 1) == -1);
    SP_ASSERT(sp_store_handle(NULL) == NULL);
    SP_ASSERT(sp_store_error(NULL) != NULL);
    sp_store_close(NULL);
}

SP_TEST(schema_reaches_the_current_version_with_the_quality_columns) {
    char err[256] = {0};
    sp_store *s = sp_store_open(db_path("store-v3.db"), err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) { printf("  %s\n", err); return; }
    SP_ASSERT(sp_store_schema_version(s) == 7);
    /* Naming every column: a migration that added nine of ten would
       otherwise pass. */
    SP_ASSERT(sp_store_exec(s,
        "SELECT q_mos, q_r_factor, q_rx_loss_pct, q_tx_loss_pct,"
        " q_rx_jitter_avg_us, q_rx_jitter_max_us, q_rtt_avg_us,"
        " q_rtt_max_us, q_rx_packets, q_tx_packets FROM call_record;") == 0);
    sp_store_close(s);
}

SP_TEST(a_version_2_database_upgrades_with_its_rows_intact) {
    /* The operator's existing history has to survive. Build the
       database, pretend it is still at version 2, reopen. */
    const char *p = db_path("store-upgrade.db");
    char err[256] = {0};
    sp_store *s = sp_store_open(p, err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) return;
    SP_ASSERT(sp_store_exec(s,
        "INSERT INTO call_record(remote_uri, start_us)"
        " VALUES('sip:old@example', 1);") == 0);
    SP_ASSERT(sp_store_exec(s, "DROP TABLE call_transcript;") == 0);
    SP_ASSERT(sp_store_exec(s, "DROP TABLE call_record;") == 0);
    SP_ASSERT(sp_store_exec(s,
        "CREATE TABLE call_record ("
        "    id              INTEGER PRIMARY KEY,"
        "    sip_call_id     TEXT,"
        "    account         TEXT,"
        "    inbound         INTEGER NOT NULL DEFAULT 0,"
        "    remote_uri      TEXT,"
        "    local_uri       TEXT,"
        "    start_us        INTEGER NOT NULL,"
        "    answer_us       INTEGER NOT NULL DEFAULT 0,"
        "    end_us          INTEGER NOT NULL DEFAULT 0,"
        "    duration_s      INTEGER NOT NULL DEFAULT 0,"
        "    disconnect_code INTEGER NOT NULL DEFAULT 0,"
        "    codec           TEXT,"
        "    recording_path  TEXT);") == 0);
    SP_ASSERT(sp_store_exec(s,
        "INSERT INTO call_record(remote_uri, start_us)"
        " VALUES('sip:old@example', 1);") == 0);
    /* Everything the later migrations created goes too, or reopening
       re-runs them against tables that already exist. */
    SP_ASSERT(sp_store_exec(s, "DROP TABLE contact_number;") == 0);
    SP_ASSERT(sp_store_exec(s, "DROP TABLE contact;") == 0);
    SP_ASSERT(sp_store_exec(s, "PRAGMA user_version = 2;") == 0);
    sp_store_close(s);

    s = sp_store_open(p, err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) { printf("  %s\n", err); return; }
    SP_ASSERT(sp_store_schema_version(s) == 7);
    /* Naming the column: a missing one is a prepare error. */
    SP_ASSERT(sp_store_exec(s, "SELECT q_mos FROM call_record;") == 0);
    /* The row is still there, and its new column defaulted rather
       than nulled: q_mos is NOT NULL DEFAULT 0. */
    SP_ASSERT(row_count(s,
        "SELECT COUNT(*) FROM call_record"
        " WHERE remote_uri = 'sip:old@example' AND q_mos = 0;") == 1);
    sp_store_close(s);
}

SP_TEST(a_version_3_database_gains_a_contact_book_without_losing_history) {
    /* The upgrade an operator running 0.5.0 will actually take. */
    const char *p = db_path("store-v3-to-v4.db");
    char err[256] = {0};
    sp_store *s = sp_store_open(p, err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) return;
    SP_ASSERT(sp_store_exec(s,
        "INSERT INTO call_record(remote_uri, start_us)"
        " VALUES('sip:old@example', 1);") == 0);
    SP_ASSERT(sp_store_exec(s, "DROP TABLE contact_number;") == 0);
    SP_ASSERT(sp_store_exec(s, "DROP TABLE contact;") == 0);
    SP_ASSERT(sp_store_exec(s, "DROP TABLE call_transcript;") == 0);
    SP_ASSERT(sp_store_exec(s,
        "ALTER TABLE call_record DROP COLUMN transcript_state;") == 0);
    SP_ASSERT(sp_store_exec(s,
        "ALTER TABLE call_record DROP COLUMN transcript_detail;") == 0);
    SP_ASSERT(sp_store_exec(s, "PRAGMA user_version = 3;") == 0);
    sp_store_close(s);

    s = sp_store_open(p, err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) { printf("  %s\n", err); return; }
    SP_ASSERT(sp_store_schema_version(s) == 7);
    SP_ASSERT(sp_store_exec(s, "SELECT count(*) FROM contact;") == 0);
    SP_ASSERT(sp_store_exec(s,
        "SELECT match_key FROM contact_number;") == 0);
    SP_ASSERT(row_count(s,
        "SELECT COUNT(*) FROM call_record"
        " WHERE remote_uri = 'sip:old@example';") == 1);
    sp_store_close(s);
}

SP_TEST(a_version_4_database_gains_a_picture_column_without_losing_rows) {
    /* The upgrade an operator with an existing contact book actually
       takes: the contact table is already there, at the version-4
       shape (no picture columns), and a real contact is in it. */
    const char *p = db_path("store-v4-to-v5.db");
    char err[256] = {0};
    sp_store *s = sp_store_open(p, err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) return;
    SP_ASSERT(sp_store_exec(s,
        "INSERT INTO contact(name, favourite, created_us, updated_us)"
        " VALUES('Sylvie Tremblay', 0, 1, 1);") == 0);
    SP_ASSERT(sp_store_exec(s,
        "INSERT INTO contact_number"
        " (contact_id, label, uri, match_key, position)"
        " SELECT id, 'work', '6046', '6046', 0 FROM contact"
        " WHERE name = 'Sylvie Tremblay';") == 0);
    /* Back to the version-4 shape: no picture columns on contact. */
    SP_ASSERT(sp_store_exec(s,
        "CREATE TABLE contact_v4 ("
        "    id          INTEGER PRIMARY KEY,"
        "    name        TEXT    NOT NULL,"
        "    company     TEXT,"
        "    note        TEXT,"
        "    favourite   INTEGER NOT NULL DEFAULT 0,"
        "    created_us  INTEGER NOT NULL,"
        "    updated_us  INTEGER NOT NULL"
        ");") == 0);
    SP_ASSERT(sp_store_exec(s,
        "INSERT INTO contact_v4"
        " (id, name, company, note, favourite, created_us, updated_us)"
        " SELECT id, name, company, note, favourite, created_us, updated_us"
        " FROM contact;") == 0);
    /* Off for the swap below: contact_number's FK is ON DELETE
       CASCADE, and with enforcement on, dropping the table it
       references cascades the delete to contact_number too -- a
       fixture artifact of simulating the old shape by dropping and
       renaming, not something a real migration does. Reopening below
       turns enforcement back on. */
    SP_ASSERT(sp_store_exec(s, "PRAGMA foreign_keys=OFF;") == 0);
    SP_ASSERT(sp_store_exec(s, "DROP TABLE contact;") == 0);
    SP_ASSERT(sp_store_exec(s,
        "ALTER TABLE contact_v4 RENAME TO contact;") == 0);
    SP_ASSERT(sp_store_exec(s, "DROP TABLE call_transcript;") == 0);
    SP_ASSERT(sp_store_exec(s,
        "ALTER TABLE call_record DROP COLUMN transcript_state;") == 0);
    SP_ASSERT(sp_store_exec(s,
        "ALTER TABLE call_record DROP COLUMN transcript_detail;") == 0);
    SP_ASSERT(sp_store_exec(s, "PRAGMA user_version = 4;") == 0);
    sp_store_close(s);

    s = sp_store_open(p, err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) { printf("  %s\n", err); return; }
    SP_ASSERT(sp_store_schema_version(s) == 7);
    SP_ASSERT(sp_store_exec(s,
        "SELECT picture, picture_type FROM contact;") == 0);
    SP_ASSERT(row_count(s,
        "SELECT COUNT(*) FROM contact WHERE name = 'Sylvie Tremblay'"
        " AND picture IS NULL;") == 1);
    SP_ASSERT(row_count(s,
        "SELECT COUNT(*) FROM contact_number WHERE uri = '6046';") == 1);
    sp_store_close(s);
}

SP_TEST(a_version_5_database_gains_transcripts_without_losing_history) {
    const char *p = db_path("store-v5-to-v6.db");
    char err[256] = {0};
    sp_store *s = sp_store_open(p, err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) return;
    SP_ASSERT(sp_store_exec(s,
        "INSERT INTO call_record(remote_uri, start_us)"
        " VALUES('sip:old@example', 1);") == 0);
    SP_ASSERT(sp_store_exec(s, "DROP TABLE call_transcript;") == 0);
    SP_ASSERT(sp_store_exec(s,
        "ALTER TABLE call_record DROP COLUMN transcript_state;") == 0);
    SP_ASSERT(sp_store_exec(s,
        "ALTER TABLE call_record DROP COLUMN transcript_detail;") == 0);
    SP_ASSERT(sp_store_exec(s, "PRAGMA user_version = 5;") == 0);
    sp_store_close(s);

    s = sp_store_open(p, err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) { printf("  %s\n", err); return; }
    SP_ASSERT(sp_store_schema_version(s) == 7);
    SP_ASSERT(sp_store_exec(s,
        "SELECT call_id, seq, start_ms, end_ms, local, speaker, text"
        " FROM call_transcript;") == 0);
    SP_ASSERT(sp_store_exec(s,
        "SELECT 1 FROM call_record WHERE remote_uri = 'sip:old@example'"
        " AND transcript_state = 0;") == 0);
    sp_store_close(s);
}

SP_TEST(a_version_6_database_gains_a_transcript_reason) {
    const char *p = db_path("store-v6-to-v7.db");
    char err[256] = {0};
    sp_store *s = sp_store_open(p, err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) return;
    SP_ASSERT(sp_store_exec(s,
        "INSERT INTO call_record(remote_uri, start_us, transcript_state)"
        " VALUES('sip:old@example', 1, 3);") == 0);
    SP_ASSERT(sp_store_exec(s,
        "ALTER TABLE call_record DROP COLUMN transcript_detail;") == 0);
    SP_ASSERT(sp_store_exec(s, "PRAGMA user_version = 6;") == 0);
    sp_store_close(s);

    s = sp_store_open(p, err, sizeof err);
    SP_ASSERT(s != NULL);
    if (!s) { printf("  %s\n", err); return; }
    SP_ASSERT(sp_store_schema_version(s) == 7);
    SP_ASSERT(row_count(s,
        "SELECT COUNT(*) FROM call_record WHERE remote_uri = 'sip:old@example'"
        " AND transcript_state = 3 AND transcript_detail IS NULL;") == 1);
    sp_store_close(s);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(opening_creates_a_database_at_the_current_schema);
    SP_RUN(deleted_rows_do_not_linger_in_the_file);
    SP_RUN(reopening_does_not_migrate_twice);
    SP_RUN(the_expected_tables_exist);
    SP_RUN(a_bad_path_fails_with_a_message_rather_than_a_crash);
    SP_RUN(prune_keeps_the_newest_rows);
    SP_RUN(prune_rejects_a_table_outside_the_allowlist);
    SP_RUN(exec_of_invalid_sql_reports_an_error);
    SP_RUN(null_arguments_do_not_crash);
    SP_RUN(schema_reaches_the_current_version_with_the_quality_columns);
    SP_RUN(a_version_2_database_upgrades_with_its_rows_intact);
    SP_RUN(a_version_3_database_gains_a_contact_book_without_losing_history);
    SP_RUN(a_version_4_database_gains_a_picture_column_without_losing_rows);
    SP_RUN(a_version_5_database_gains_transcripts_without_losing_history);
    SP_RUN(a_version_6_database_gains_a_transcript_reason);
    return SP_TEST_RESULT();
}
