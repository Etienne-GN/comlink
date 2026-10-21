#include "siplog.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "redact.h"
#include "store.h"

/* Pruning every insert would put a DELETE in the signalling path. */
#define PRUNE_EVERY 64

struct sp_siplog {
    sp_order         order;
    sqlite3        *db;
    struct sp_store *store;
    sqlite3_stmt   *ins;
    bool            redact;
    int             max_rows;
    unsigned        since_prune;
    unsigned long   drops;

    /* Materialised query results, owned here and freed on the next
       query, matching how sp_media_enum_codecs behaves. */
    sp_sip_message *rows;
    size_t          n_rows;

    pthread_mutex_t lock;
};

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

sp_siplog *sp_siplog_create(struct sp_store *store, bool redact,
                            int max_rows) {
    if (!store)
        return NULL;
    sp_siplog *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->store = store;
    l->db = sp_store_handle(store);
    l->redact = redact;
    l->max_rows = max_rows;
    if (pthread_mutex_init(&l->lock, NULL) != 0) {
        free(l);
        return NULL;
    }
    static const char *SQL =
        "INSERT INTO sip_message"
        "(ts_us, direction, method, status_code, call_id, account, redacted, raw)"
        " VALUES(?,?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(l->db, SQL, -1, &l->ins, NULL) != SQLITE_OK) {
        pthread_mutex_destroy(&l->lock);
        free(l);
        return NULL;
    }
    return l;
}

static void free_rows(sp_siplog *l) {
    for (size_t i = 0; i < l->n_rows; i++) {
        free((void *)l->rows[i].direction);
        free((void *)l->rows[i].method);
        free((void *)l->rows[i].call_id);
        free((void *)l->rows[i].account);
        free((void *)l->rows[i].raw);
    }
    free(l->rows);
    l->rows = NULL;
    l->n_rows = 0;
}

void sp_siplog_destroy(sp_siplog *l) {
    if (!l)
        return;
    sqlite3_finalize(l->ins);
    free_rows(l);
    pthread_mutex_destroy(&l->lock);
    free(l);
}

unsigned long sp_siplog_drops(sp_siplog *l) {
    if (!l)
        return 0;
    pthread_mutex_lock(&l->lock);
    unsigned long d = l->drops;
    pthread_mutex_unlock(&l->lock);
    return d;
}

void sp_siplog_capture(sp_siplog *l, const char *raw, size_t len,
                       const char *direction, const char *method,
                       int status_code, const char *call_id,
                       const char *account) {
    if (!l || !raw || !direction)
        return;

    char *owned = NULL;
    const char *text = raw;
    if (l->redact) {
        /* sp_redact_sip needs a NUL-terminated string. */
        char *copy = malloc(len + 1);
        if (!copy) {
            pthread_mutex_lock(&l->lock);
            l->drops++;
            pthread_mutex_unlock(&l->lock);
            return;
        }
        memcpy(copy, raw, len);
        copy[len] = '\0';
        owned = sp_redact_sip(copy);
        free(copy);
        if (!owned) {
            pthread_mutex_lock(&l->lock);
            l->drops++;
            pthread_mutex_unlock(&l->lock);
            return;
        }
        text = owned;
        len = strlen(owned);
    }

    pthread_mutex_lock(&l->lock);
    sqlite3_reset(l->ins);
    sqlite3_clear_bindings(l->ins);
    sqlite3_bind_int64(l->ins, 1, (sqlite3_int64)now_us());
    sqlite3_bind_text(l->ins, 2, direction, -1, SQLITE_STATIC);
    if (method && *method)
        sqlite3_bind_text(l->ins, 3, method, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(l->ins, 4, status_code);
    if (call_id && *call_id)
        sqlite3_bind_text(l->ins, 5, call_id, -1, SQLITE_TRANSIENT);
    if (account && *account)
        sqlite3_bind_text(l->ins, 6, account, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(l->ins, 7, l->redact ? 1 : 0);
    sqlite3_bind_text(l->ins, 8, text, (int)len, SQLITE_TRANSIENT);

    /* A failed insert must not fail the call it belongs to. */
    if (sqlite3_step(l->ins) != SQLITE_DONE)
        l->drops++;

    if (l->max_rows > 0 && ++l->since_prune >= PRUNE_EVERY) {
        l->since_prune = 0;
        sp_store_prune(l->store, "sip_message", l->max_rows);
    }
    pthread_mutex_unlock(&l->lock);

    free(owned);
}

static char *dup_col(sqlite3_stmt *st, int col) {
    const unsigned char *t = sqlite3_column_text(st, col);
    return t ? strdup((const char *)t) : NULL;
}

sp_status sp_siplog_query(sp_siplog *l, const char *call_id, size_t limit,
                          const sp_sip_message **out, size_t *n_out) {
    if (!out || !n_out)
        return SP_ERR_INVALID_ARG;
    *out = NULL;
    *n_out = 0;
    if (!l)
        return SP_OK;
    if (limit == 0 || limit > 100000)
        limit = 100;

    /* Our own enum, never operator input, so substituted rather than
       bound: a sort direction cannot be a bound parameter in SQL. */
    const char *dir = l->order == SP_ORDER_OLDEST_FIRST ? "ASC" : "DESC";
    char sql[512];
    snprintf(sql, sizeof sql,
             call_id
             ? "SELECT ts_us, direction, method, status_code, call_id,"
               " account, redacted, raw FROM sip_message"
               " WHERE call_id = ? ORDER BY id %s LIMIT ?;"
             : "SELECT ts_us, direction, method, status_code, call_id,"
               " account, redacted, raw FROM sip_message"
               " ORDER BY id %s LIMIT ?;", dir);

    pthread_mutex_lock(&l->lock);
    free_rows(l);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(l->db, sql, -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&l->lock);
        return SP_ERR_INTERNAL;
    }
    if (call_id) {
        sqlite3_bind_text(st, 1, call_id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)limit);
    } else {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)limit);
    }

    size_t cap = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (l->n_rows == cap) {
            size_t grow = cap ? cap * 2 : 32;
            sp_sip_message *r = realloc(l->rows, grow * sizeof *r);
            if (!r)
                break;
            l->rows = r;
            cap = grow;
        }
        sp_sip_message *m = &l->rows[l->n_rows];
        memset(m, 0, sizeof *m);
        m->ts_us = (uint64_t)sqlite3_column_int64(st, 0);
        m->direction = dup_col(st, 1);
        m->method = dup_col(st, 2);
        m->status_code = sqlite3_column_int(st, 3);
        m->call_id = dup_col(st, 4);
        m->account = dup_col(st, 5);
        m->redacted = sqlite3_column_int(st, 6) != 0;
        m->raw = dup_col(st, 7);
        l->n_rows++;
    }
    sqlite3_finalize(st);

    *out = l->rows;
    *n_out = l->n_rows;
    pthread_mutex_unlock(&l->lock);
    return SP_OK;
}

sp_status sp_siplog_wipe(sp_siplog *l) {
    if (!l)
        return SP_OK;
    pthread_mutex_lock(&l->lock);
    free_rows(l);
    int rc = sp_store_exec(l->store, "DELETE FROM sip_message;");
    pthread_mutex_unlock(&l->lock);
    return rc == 0 ? SP_OK : SP_ERR_INTERNAL;
}

void sp_siplog_set_order(sp_siplog *h, sp_order order) {
    if (h)
        h->order = order;
}
