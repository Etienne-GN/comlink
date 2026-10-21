#include "store.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sp_store {
    sqlite3 *db;
    char     err[512];
};

/* Indexed by target version: MIGRATIONS[0] takes the schema to
   version 1. Append only; never edit a released entry. */
static const char *const MIGRATIONS[] = {
    /* -> 1 */
    "CREATE TABLE sip_message ("
    "    id          INTEGER PRIMARY KEY,"
    "    ts_us       INTEGER NOT NULL,"
    "    direction   TEXT    NOT NULL,"
    "    method      TEXT,"
    "    status_code INTEGER NOT NULL DEFAULT 0,"
    "    call_id     TEXT,"
    "    account     TEXT,"
    "    redacted    INTEGER NOT NULL DEFAULT 1,"
    "    raw         TEXT    NOT NULL"
    ");"
    "CREATE INDEX sip_message_call_id ON sip_message(call_id);"
    "CREATE INDEX sip_message_ts ON sip_message(ts_us);"
    "CREATE TABLE action_log ("
    "    id     INTEGER PRIMARY KEY,"
    "    ts_us  INTEGER NOT NULL,"
    "    level  INTEGER NOT NULL,"
    "    text   TEXT    NOT NULL"
    ");"
    "CREATE INDEX action_log_ts ON action_log(ts_us);",

    /* -> 2 */
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
    "    recording_path  TEXT"
    ");"
    "CREATE INDEX call_record_start ON call_record(start_us);"
    "CREATE INDEX call_record_sip_call_id ON call_record(sip_call_id);",

    /* -> 3 : the final audio-quality figures for the call. Columns
       rather than a second table: there is exactly one row of these
       per call, and a join to read a MOS would be a worse trade. */
    "ALTER TABLE call_record ADD COLUMN q_mos REAL NOT NULL DEFAULT 0;"
    "ALTER TABLE call_record ADD COLUMN q_r_factor"
    "    INTEGER NOT NULL DEFAULT 0;"
    "ALTER TABLE call_record ADD COLUMN q_rx_loss_pct"
    "    REAL NOT NULL DEFAULT 0;"
    "ALTER TABLE call_record ADD COLUMN q_tx_loss_pct"
    "    REAL NOT NULL DEFAULT 0;"
    "ALTER TABLE call_record ADD COLUMN q_rx_jitter_avg_us"
    "    INTEGER NOT NULL DEFAULT 0;"
    "ALTER TABLE call_record ADD COLUMN q_rx_jitter_max_us"
    "    INTEGER NOT NULL DEFAULT 0;"
    "ALTER TABLE call_record ADD COLUMN q_rtt_avg_us"
    "    INTEGER NOT NULL DEFAULT 0;"
    "ALTER TABLE call_record ADD COLUMN q_rtt_max_us"
    "    INTEGER NOT NULL DEFAULT 0;"
    "ALTER TABLE call_record ADD COLUMN q_rx_packets"
    "    INTEGER NOT NULL DEFAULT 0;"
    "ALTER TABLE call_record ADD COLUMN q_tx_packets"
    "    INTEGER NOT NULL DEFAULT 0;",

    /* -> 4 : the contact book. Two tables, because a person has more
       than one number and pretending otherwise means migrating again
       the first time somebody has a mobile.

       uri is kept exactly as it was typed, so what an operator sees is
       what gets dialled. match_key is derived from it and never shown,
       which makes a change to the matching rules a recomputation of a
       column rather than a loss of what was entered. */
    "CREATE TABLE contact ("
    "    id          INTEGER PRIMARY KEY,"
    "    name        TEXT    NOT NULL,"
    "    company     TEXT,"
    "    note        TEXT,"
    "    favourite   INTEGER NOT NULL DEFAULT 0,"
    "    created_us  INTEGER NOT NULL,"
    "    updated_us  INTEGER NOT NULL"
    ");"
    "CREATE INDEX contact_name ON contact(name COLLATE NOCASE);"
    "CREATE TABLE contact_number ("
    "    id          INTEGER PRIMARY KEY,"
    "    contact_id  INTEGER NOT NULL"
    "                REFERENCES contact(id) ON DELETE CASCADE,"
    "    label       TEXT,"
    "    uri         TEXT    NOT NULL,"
    "    match_key   TEXT    NOT NULL,"
    "    position    INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX contact_number_match ON contact_number(match_key);"
    "CREATE INDEX contact_number_owner ON contact_number(contact_id);",

    /* -> 5 : a picture for the contact, in the database rather than
       beside it so the book stays one file: it backs up, copies and
       deletes atomically, and a deleted contact cannot leave an
       orphan behind. */
    /* Parenthesised because clang's -Wstring-concatenation reads two
       whole statements on two lines as a missing comma. It is not:
       every element here is one migration, and this one is two ALTERs
       run together. GCC has no such warning, so the Debian build
       never said a word and the Android cross-build refused to
       compile. */
    ("ALTER TABLE contact ADD COLUMN picture      BLOB;"
     "ALTER TABLE contact ADD COLUMN picture_type TEXT;"),

    /* -> 6 : call transcripts. A table rather than a column: a call
       has as many lines as it has turns of speech. The state lives on
       the call row so the Calls list reads it without a join. */
    ("ALTER TABLE call_record ADD COLUMN transcript_state"
     "    INTEGER NOT NULL DEFAULT 0;"
     "CREATE TABLE call_transcript ("
     "    call_id   INTEGER NOT NULL"
     "              REFERENCES call_record(id) ON DELETE CASCADE,"
     "    seq       INTEGER NOT NULL,"
     "    start_ms  INTEGER NOT NULL,"
     "    end_ms    INTEGER NOT NULL,"
     "    local     INTEGER NOT NULL,"
     "    speaker   TEXT    NOT NULL,"
     "    text      TEXT    NOT NULL,"
     "    PRIMARY KEY (call_id, seq)"
     ");"),

    /* -> 7 : why a transcript failed, beside the state that says it
       did. The event carrying it is gone by the time anyone looks at
       the Calls list. */
    "ALTER TABLE call_record ADD COLUMN transcript_detail TEXT;"
};

#define SP_SCHEMA_TARGET ((int)(sizeof MIGRATIONS / sizeof MIGRATIONS[0]))

/* A table name cannot be a bound parameter, so pruning accepts only
   these. */
static const char *const PRUNABLE[] = {
    "sip_message", "action_log", "call_record", NULL
};

static int prunable(const char *table) {
    if (!table)
        return 0;
    for (size_t i = 0; PRUNABLE[i]; i++)
        if (strcmp(PRUNABLE[i], table) == 0)
            return 1;
    return 0;
}

const char *sp_store_error(sp_store *s) {
    return s ? s->err : "";
}

void *sp_store_handle(sp_store *s) {
    return s ? (void *)s->db : NULL;
}

int sp_store_exec(sp_store *s, const char *sql) {
    if (!s || !sql)
        return -1;
    char *msg = NULL;
    if (sqlite3_exec(s->db, sql, NULL, NULL, &msg) != SQLITE_OK) {
        snprintf(s->err, sizeof s->err, "%s", msg ? msg : "sql error");
        sqlite3_free(msg);
        return -1;
    }
    sqlite3_free(msg);
    return 0;
}

int sp_store_schema_version(sp_store *s) {
    if (!s)
        return -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, "PRAGMA user_version;", -1, &st, NULL)
            != SQLITE_OK)
        return -1;
    int v = -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

static int migrate(sp_store *s) {
    int have = sp_store_schema_version(s);
    if (have < 0) {
        snprintf(s->err, sizeof s->err, "cannot read schema version");
        return -1;
    }
    for (int v = have; v < SP_SCHEMA_TARGET; v++) {
        char tail[64];
        snprintf(tail, sizeof tail, "PRAGMA user_version = %d;", v + 1);
        /* One transaction per step, so an interrupted upgrade leaves
           the schema at a version that actually matches the tables. */
        if (sp_store_exec(s, "BEGIN;") != 0)
            return -1;
        if (sp_store_exec(s, MIGRATIONS[v]) != 0 ||
            sp_store_exec(s, tail) != 0) {
            sp_store_exec(s, "ROLLBACK;");
            return -1;
        }
        if (sp_store_exec(s, "COMMIT;") != 0)
            return -1;
    }
    return 0;
}

sp_store *sp_store_open(const char *path, char *err, size_t errlen) {
    if (!path) {
        snprintf(err, errlen, "no database path");
        return NULL;
    }
    sp_store *s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }

    int rc = sqlite3_open_v2(path, &s->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                 SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (rc != SQLITE_OK) {
        snprintf(err, errlen, "cannot open %s: %s", path,
                 s->db ? sqlite3_errmsg(s->db) : sqlite3_errstr(rc));
        sqlite3_close(s->db);
        free(s);
        return NULL;
    }

    /* WAL and a busy timeout: pjsip worker threads and a frontend
       thread both write, and a writer must wait rather than fail. */
    if (sp_store_exec(s, "PRAGMA journal_mode=WAL;") != 0 ||
        sp_store_exec(s, "PRAGMA busy_timeout=3000;") != 0 ||
        sp_store_exec(s, "PRAGMA foreign_keys=ON;") != 0 ||
        /* Debian builds SQLite with this on and the amalgamation the
           Android build compiles has it off: deleted calls and
           transcripts would otherwise linger in free pages there. */
        sp_store_exec(s, "PRAGMA secure_delete=ON;") != 0) {
        snprintf(err, errlen, "cannot configure %s: %s", path, s->err);
        sqlite3_close(s->db);
        free(s);
        return NULL;
    }

    if (migrate(s) != 0) {
        snprintf(err, errlen, "cannot migrate %s: %s", path, s->err);
        sqlite3_close(s->db);
        free(s);
        return NULL;
    }
    return s;
}

void sp_store_close(sp_store *s) {
    if (!s)
        return;
    sqlite3_close(s->db);
    free(s);
}

int sp_store_prune(sp_store *s, const char *table, int keep) {
    if (!s || !prunable(table) || keep < 0) {
        if (s)
            snprintf(s->err, sizeof s->err,
                     "refusing to prune \"%s\": not a known table",
                     table ? table : "(null)");
        return -1;
    }
    char sql[256];
    snprintf(sql, sizeof sql,
             "DELETE FROM %s WHERE id NOT IN "
             "(SELECT id FROM %s ORDER BY id DESC LIMIT %d);",
             table, table, keep);
    if (sp_store_exec(s, sql) != 0)
        return -1;
    return sqlite3_changes(s->db);
}
