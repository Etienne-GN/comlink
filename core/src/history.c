#include "history.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "store.h"

#define PRUNE_EVERY 32

struct sp_history {
    sp_order         order;
    sqlite3         *db;
    struct sp_store *store;
    int              max_rows;
    unsigned         since_prune;
    unsigned long    drops;

    sp_call_record  *rows;
    size_t           n_rows;

    sp_transcript_line *lines;
    size_t              n_lines;

    pthread_mutex_t  lock;
};

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

sp_history *sp_history_create(struct sp_store *store, int max_rows) {
    if (!store)
        return NULL;
    sp_history *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->store = store;
    h->db = sp_store_handle(store);
    h->max_rows = max_rows;
    if (pthread_mutex_init(&h->lock, NULL) != 0) {
        free(h);
        return NULL;
    }
    return h;
}

static void free_rows(sp_history *h) {
    for (size_t i = 0; i < h->n_rows; i++) {
        free((void *)h->rows[i].sip_call_id);
        free((void *)h->rows[i].account);
        free((void *)h->rows[i].remote_uri);
        free((void *)h->rows[i].local_uri);
        free((void *)h->rows[i].codec);
        free((void *)h->rows[i].recording_path);
        free((void *)h->rows[i].transcript_detail);
    }
    free(h->rows);
    h->rows = NULL;
    h->n_rows = 0;
}

static void free_lines(sp_history *h) {
    for (size_t i = 0; i < h->n_lines; i++) {
        free((void *)h->lines[i].speaker);
        free((void *)h->lines[i].text);
    }
    free(h->lines);
    h->lines = NULL;
    h->n_lines = 0;
}

void sp_history_destroy(sp_history *h) {
    if (!h)
        return;
    free_rows(h);
    free_lines(h);
    pthread_mutex_destroy(&h->lock);
    free(h);
}

int64_t sp_history_begin(sp_history *h, bool inbound, const char *account,
                         const char *remote_uri, const char *local_uri,
                         const char *sip_call_id) {
    if (!h)
        return 0;
    static const char *SQL =
        "INSERT INTO call_record"
        "(sip_call_id, account, inbound, remote_uri, local_uri, start_us)"
        " VALUES(?,?,?,?,?,?);";

    pthread_mutex_lock(&h->lock);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h->db, SQL, -1, &st, NULL) != SQLITE_OK) {
        h->drops++;
        pthread_mutex_unlock(&h->lock);
        return 0;
    }
    if (sip_call_id && *sip_call_id)
        sqlite3_bind_text(st, 1, sip_call_id, -1, SQLITE_TRANSIENT);
    if (account && *account)
        sqlite3_bind_text(st, 2, account, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, inbound ? 1 : 0);
    if (remote_uri && *remote_uri)
        sqlite3_bind_text(st, 4, remote_uri, -1, SQLITE_TRANSIENT);
    if (local_uri && *local_uri)
        sqlite3_bind_text(st, 5, local_uri, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)now_us());

    int64_t id = 0;
    if (sqlite3_step(st) == SQLITE_DONE)
        id = (int64_t)sqlite3_last_insert_rowid(h->db);
    else
        h->drops++;
    sqlite3_finalize(st);

    if (h->max_rows > 0 && ++h->since_prune >= PRUNE_EVERY) {
        h->since_prune = 0;
        sp_store_prune(h->store, "call_record", h->max_rows);
    }
    pthread_mutex_unlock(&h->lock);
    return id;
}

/* One field of history at a time: a failed update must not fail a
   call, so nothing here reports upward. */
static void update_text(sp_history *h, int64_t id, const char *sql,
                        const char *value) {
    if (!h || id <= 0 || !value || !*value)
        return;
    pthread_mutex_lock(&h->lock);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h->db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, value, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, id);
        if (sqlite3_step(st) != SQLITE_DONE)
            h->drops++;
    } else {
        h->drops++;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&h->lock);
}

void sp_history_set_call_id(sp_history *h, int64_t id, const char *cid) {
    update_text(h, id,
                "UPDATE call_record SET sip_call_id = ?"
                " WHERE id = ? AND (sip_call_id IS NULL OR sip_call_id = '');",
                cid);
}

void sp_history_recording(sp_history *h, int64_t id, const char *path) {
    update_text(h, id,
                "UPDATE call_record SET recording_path = ? WHERE id = ?;",
                path);
}

void sp_history_answered(sp_history *h, int64_t id, const char *codec) {
    if (!h || id <= 0)
        return;
    pthread_mutex_lock(&h->lock);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h->db,
            "UPDATE call_record SET answer_us = ?, codec = COALESCE(?, codec)"
            " WHERE id = ? AND answer_us = 0;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)now_us());
        if (codec && *codec)
            sqlite3_bind_text(st, 2, codec, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, id);
        if (sqlite3_step(st) != SQLITE_DONE)
            h->drops++;
    } else {
        h->drops++;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&h->lock);
}

void sp_history_ended(sp_history *h, int64_t id, int code, uint32_t dur_s) {
    if (!h || id <= 0)
        return;
    pthread_mutex_lock(&h->lock);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h->db,
            "UPDATE call_record SET end_us = ?, duration_s = ?,"
            " disconnect_code = ? WHERE id = ? AND end_us = 0;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)now_us());
        sqlite3_bind_int(st, 2, (int)dur_s);
        sqlite3_bind_int(st, 3, code);
        sqlite3_bind_int64(st, 4, id);
        if (sqlite3_step(st) != SQLITE_DONE)
            h->drops++;
    } else {
        h->drops++;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&h->lock);
}

void sp_history_quality(sp_history *h, int64_t id,
                        const sp_call_quality *q) {
    if (!h || id <= 0 || !q || !q->valid)
        return;
    pthread_mutex_lock(&h->lock);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h->db,
            "UPDATE call_record SET q_mos = ?, q_r_factor = ?,"
            " q_rx_loss_pct = ?, q_tx_loss_pct = ?,"
            " q_rx_jitter_avg_us = ?, q_rx_jitter_max_us = ?,"
            " q_rtt_avg_us = ?, q_rtt_max_us = ?,"
            " q_rx_packets = ?, q_tx_packets = ?"
            " WHERE id = ?;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_double(st, 1, q->mos);
        sqlite3_bind_int(st, 2, q->r_factor);
        sqlite3_bind_double(st, 3, q->rx_loss_pct);
        sqlite3_bind_double(st, 4, q->tx_loss_pct);
        sqlite3_bind_int(st, 5, (int)q->rx_jitter.avg_us);
        sqlite3_bind_int(st, 6, (int)q->rx_jitter.max_us);
        sqlite3_bind_int(st, 7, (int)q->rtt.avg_us);
        sqlite3_bind_int(st, 8, (int)q->rtt.max_us);
        sqlite3_bind_int(st, 9, (int)q->rx_packets);
        sqlite3_bind_int(st, 10, (int)q->tx_packets);
        sqlite3_bind_int64(st, 11, id);
        if (sqlite3_step(st) != SQLITE_DONE)
            h->drops++;
    } else {
        h->drops++;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&h->lock);
}

static char *dup_col(sqlite3_stmt *st, int col) {
    const unsigned char *t = sqlite3_column_text(st, col);
    return t ? strdup((const char *)t) : NULL;
}

sp_status sp_history_query(sp_history *h, size_t limit,
                           const sp_call_record **out, size_t *n_out) {
    if (!out || !n_out)
        return SP_ERR_INVALID_ARG;
    *out = NULL;
    *n_out = 0;
    if (!h)
        return SP_OK;
    if (limit == 0 || limit > 100000)
        limit = 100;

    pthread_mutex_lock(&h->lock);
    free_rows(h);

    /* The direction is an enum of ours, never operator input, so it is
       substituted rather than bound: a direction cannot be a bound
       parameter in SQL. */
    const char *dir = h->order == SP_ORDER_OLDEST_FIRST ? "ASC" : "DESC";
    char sql[768];
    snprintf(sql, sizeof sql,
            "SELECT id, sip_call_id, account, inbound, remote_uri, local_uri,"
            " start_us, answer_us, end_us, duration_s, disconnect_code,"
            " codec, recording_path,"
            " q_mos, q_r_factor, q_rx_loss_pct, q_tx_loss_pct,"
            " q_rx_jitter_avg_us, q_rx_jitter_max_us,"
            " q_rtt_avg_us, q_rtt_max_us, q_rx_packets, q_tx_packets,"
            " transcript_state, transcript_detail"
            " FROM call_record ORDER BY id %s LIMIT ?;", dir);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h->db, sql, -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&h->lock);
        return SP_ERR_INTERNAL;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)limit);

    size_t cap = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (h->n_rows == cap) {
            size_t grow = cap ? cap * 2 : 32;
            sp_call_record *r = realloc(h->rows, grow * sizeof *r);
            if (!r)
                break;
            h->rows = r;
            cap = grow;
        }
        sp_call_record *c = &h->rows[h->n_rows];
        memset(c, 0, sizeof *c);
        c->id = (int64_t)sqlite3_column_int64(st, 0);
        c->sip_call_id = dup_col(st, 1);
        c->account = dup_col(st, 2);
        c->inbound = sqlite3_column_int(st, 3) != 0;
        c->remote_uri = dup_col(st, 4);
        c->local_uri = dup_col(st, 5);
        c->start_us = (uint64_t)sqlite3_column_int64(st, 6);
        c->answer_us = (uint64_t)sqlite3_column_int64(st, 7);
        c->end_us = (uint64_t)sqlite3_column_int64(st, 8);
        c->duration_s = (uint32_t)sqlite3_column_int(st, 9);
        c->disconnect_code = sqlite3_column_int(st, 10);
        c->codec = dup_col(st, 11);
        c->recording_path = dup_col(st, 12);
        c->q_mos              = sqlite3_column_double(st, 13);
        c->q_r_factor         = sqlite3_column_int(st, 14);
        c->q_rx_loss_pct      = sqlite3_column_double(st, 15);
        c->q_tx_loss_pct      = sqlite3_column_double(st, 16);
        c->q_rx_jitter_avg_us = (uint32_t)sqlite3_column_int(st, 17);
        c->q_rx_jitter_max_us = (uint32_t)sqlite3_column_int(st, 18);
        c->q_rtt_avg_us       = (uint32_t)sqlite3_column_int(st, 19);
        c->q_rtt_max_us       = (uint32_t)sqlite3_column_int(st, 20);
        c->q_rx_packets       = (uint32_t)sqlite3_column_int(st, 21);
        c->q_tx_packets       = (uint32_t)sqlite3_column_int(st, 22);
        c->transcript = (sp_transcript_state)sqlite3_column_int(st, 23);
        c->transcript_detail = dup_col(st, 24);
        h->n_rows++;
    }
    sqlite3_finalize(st);

    *out = h->rows;
    *n_out = h->n_rows;
    pthread_mutex_unlock(&h->lock);
    return SP_OK;
}

static int run_id(sp_history *h, const char *sql, int64_t a, int64_t b,
                  int n_bind) {
    sqlite3_stmt *st = NULL;
    int rc = -1;
    if (sqlite3_prepare_v2(h->db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, a);
        if (n_bind > 1)
            sqlite3_bind_int64(st, 2, b);
        rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    }
    sqlite3_finalize(st);
    if (rc != 0)
        h->drops++;
    return rc;
}

void sp_history_transcript_state(sp_history *h, int64_t id,
                                 sp_transcript_state state) {
    if (!h || id <= 0)
        return;
    pthread_mutex_lock(&h->lock);
    run_id(h, "UPDATE call_record SET transcript_state = ?,"
              " transcript_detail = NULL WHERE id = ?;",
           (int64_t)state, id, 2);
    pthread_mutex_unlock(&h->lock);
}

void sp_history_transcript_failed(sp_history *h, int64_t id,
                                  const char *why) {
    if (!h || id <= 0)
        return;
    pthread_mutex_lock(&h->lock);
    sqlite3_stmt *st = NULL;
    int rc = -1;
    if (sqlite3_prepare_v2(h->db,
            "UPDATE call_record SET transcript_state = ?,"
            " transcript_detail = ? WHERE id = ?;", -1, &st, NULL)
        == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (int64_t)SP_TRANSCRIPT_FAILED);
        if (why)
            sqlite3_bind_text(st, 2, why, -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(st, 2);
        sqlite3_bind_int64(st, 3, id);
        rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    }
    sqlite3_finalize(st);
    if (rc != 0)
        h->drops++;
    pthread_mutex_unlock(&h->lock);
}

/* Rows per INSERT: seven parameters each, far below SQLite's limit on
   bound parameters. */
#define TRANSCRIPT_BATCH 400

/* A few statements, not one per line. Every statement is a commit,
   and on a handset's flash each commit is a sync that holds h->lock
   -- which the call that is up right now needs. No explicit
   transaction: the connection is shared with the SIP log and the
   action log, which write from other threads, and a BEGIN here would
   sweep their inserts into this one. A failure part-way removes what
   was written and marks the call FAILED, which reads the same to
   anybody looking. */
static int insert_batch(sp_history *h, int64_t id,
                        const sp_transcript_line *lines, size_t first,
                        size_t count) {
    /* The column list, then "(?,?,?,?,?,?,?)," per row. */
    char sql[128 + TRANSCRIPT_BATCH * 16];
    size_t len = (size_t)snprintf(sql, sizeof sql,
        "INSERT INTO call_transcript"
        "(call_id, seq, start_ms, end_ms, local, speaker, text) VALUES");
    for (size_t k = 0; k < count; k++)
        len += (size_t)snprintf(sql + len, sizeof sql - len, "%s(?,?,?,?,?,?,?)",
                                k ? "," : "");
    if (len >= sizeof sql)
        return -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h->db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    for (size_t k = 0; k < count; k++) {
        const sp_transcript_line *l = &lines[first + k];
        const int p = (int)k * 7;
        sqlite3_bind_int64(st, p + 1, id);
        sqlite3_bind_int64(st, p + 2, (sqlite3_int64)(first + k));
        sqlite3_bind_int64(st, p + 3, (sqlite3_int64)l->start_ms);
        sqlite3_bind_int64(st, p + 4, (sqlite3_int64)l->end_ms);
        sqlite3_bind_int(st, p + 5, l->local ? 1 : 0);
        sqlite3_bind_text(st, p + 6, l->speaker ? l->speaker : "", -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(st, p + 7, l->text ? l->text : "", -1,
                          SQLITE_TRANSIENT);
    }
    const int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return rc;
}

int sp_history_transcript_store(sp_history *h, int64_t id,
                                const sp_transcript_line *lines, size_t n) {
    if (!h || id <= 0 || (n > 0 && !lines))
        return -1;
    pthread_mutex_lock(&h->lock);
    int rc = run_id(h, "DELETE FROM call_transcript WHERE call_id = ?;",
                    id, 0, 1);
    for (size_t first = 0; rc == 0 && first < n; first += TRANSCRIPT_BATCH)
        rc = insert_batch(h, id, lines, first,
                          n - first < TRANSCRIPT_BATCH ? n - first
                                                       : TRANSCRIPT_BATCH);
    if (rc == 0)
        rc = run_id(h, "UPDATE call_record SET transcript_state = ?"
                       " WHERE id = ?;",
                    (int64_t)SP_TRANSCRIPT_READY, id, 2);
    if (rc != 0) {
        h->drops++;
        run_id(h, "DELETE FROM call_transcript WHERE call_id = ?;", id, 0, 1);
        run_id(h, "UPDATE call_record SET transcript_state = ?,"
                  " transcript_detail = 'the transcript could not be saved'"
                  " WHERE id = ?;",
               (int64_t)SP_TRANSCRIPT_FAILED, id, 2);
    }
    pthread_mutex_unlock(&h->lock);
    return rc;
}

sp_status sp_history_transcript_read(sp_history *h, int64_t id,
                                     const sp_transcript_line **out,
                                     size_t *n_out) {
    if (!out || !n_out)
        return SP_ERR_INVALID_ARG;
    *out = NULL;
    *n_out = 0;
    if (!h)
        return SP_OK;
    pthread_mutex_lock(&h->lock);
    free_lines(h);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h->db,
            "SELECT start_ms, end_ms, local, speaker, text"
            " FROM call_transcript WHERE call_id = ? ORDER BY seq;",
            -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&h->lock);
        return SP_ERR_INTERNAL;
    }
    sqlite3_bind_int64(st, 1, id);
    size_t cap = 0;
    sp_status ret = SP_OK;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (h->n_lines == cap) {
            size_t grow = cap ? cap * 2 : 32;
            sp_transcript_line *l = realloc(h->lines, grow * sizeof *l);
            if (!l) {
                ret = SP_ERR_INTERNAL;
                break;
            }
            h->lines = l;
            cap = grow;
        }
        sp_transcript_line *l = &h->lines[h->n_lines];
        l->start_ms = (uint32_t)sqlite3_column_int64(st, 0);
        l->end_ms = (uint32_t)sqlite3_column_int64(st, 1);
        l->local = sqlite3_column_int(st, 2) != 0;
        l->speaker = dup_col(st, 3);
        l->text = dup_col(st, 4);
        h->n_lines++;
    }
    sqlite3_finalize(st);
    *out = h->lines;
    *n_out = h->n_lines;
    pthread_mutex_unlock(&h->lock);
    return ret;
}

int sp_history_transcript_pending(sp_history *h, sp_pending_call **out,
                                  size_t *n_out) {
    *out = NULL;
    *n_out = 0;
    if (!h)
        return 0;
    pthread_mutex_lock(&h->lock);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h->db,
            "SELECT id, remote_uri FROM call_record"
            " WHERE transcript_state = ? ORDER BY id;", -1, &st, NULL)
        != SQLITE_OK) {
        pthread_mutex_unlock(&h->lock);
        return -1;
    }
    sqlite3_bind_int64(st, 1, (int64_t)SP_TRANSCRIPT_PENDING);
    sp_pending_call *v = NULL;
    size_t n = 0, cap = 0;
    int rc = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            const size_t grow = cap ? cap * 2 : 8;
            sp_pending_call *g = realloc(v, grow * sizeof *g);
            if (!g) {
                rc = -1;
                break;
            }
            v = g;
            cap = grow;
        }
        v[n].id = (int64_t)sqlite3_column_int64(st, 0);
        v[n].remote_uri = (char *)dup_col(st, 1);
        n++;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&h->lock);
    if (rc != 0) {
        sp_history_pending_free(v, n);
        return -1;
    }
    *out = v;
    *n_out = n;
    return 0;
}

void sp_history_pending_free(sp_pending_call *p, size_t n) {
    for (size_t i = 0; p && i < n; i++)
        free(p[i].remote_uri);
    free(p);
}

sp_status sp_history_wipe(sp_history *h) {
    if (!h)
        return SP_OK;
    pthread_mutex_lock(&h->lock);
    free_rows(h);
    /* The last transcript read is a copy of what is being wiped. */
    free_lines(h);
    int rc = sp_store_exec(h->store, "DELETE FROM call_record;");
    pthread_mutex_unlock(&h->lock);
    return rc == 0 ? SP_OK : SP_ERR_INTERNAL;
}

void sp_history_set_order(sp_history *h, sp_order order) {
    if (h)
        h->order = order;
}
