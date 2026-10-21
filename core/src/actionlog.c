#include "actionlog.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "store.h"

#define PRUNE_EVERY 64

struct sp_actionlog {
    sp_order         order;
    sqlite3         *db;
    struct sp_store *store;
    sqlite3_stmt    *ins;
    int              max_rows;
    unsigned         since_prune;
    unsigned long    drops;

    sp_action_entry *rows;
    size_t           n_rows;

    pthread_mutex_t  lock;
};

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

static const char *call_state_name(sp_call_state s) {
    switch (s) {
    case SP_CALL_NULL:         return "idle";
    case SP_CALL_CALLING:      return "calling";
    case SP_CALL_INCOMING:     return "ringing";
    case SP_CALL_EARLY:        return "ringing";
    case SP_CALL_CONNECTING:   return "connecting";
    case SP_CALL_CONFIRMED:    return "connected";
    case SP_CALL_DISCONNECTED: return "ended";
    }
    return "unknown";
}

int sp_actionlog_format(const sp_event *ev, char *buf, size_t buflen) {
    if (!ev || !buf || buflen == 0)
        return 0;
    switch (ev->type) {
    case SP_EV_ACCOUNT_STATE:
        snprintf(buf, buflen, "account %llu %s (%d)%s%s",
                 (unsigned long long)ev->u.account.acc,
                 ev->u.account.registered ? "registered" : "not registered",
                 ev->u.account.reg_code,
                 ev->u.account.detail ? ": " : "",
                 ev->u.account.detail ? ev->u.account.detail : "");
        return 1;
    case SP_EV_CALL_STATE:
        snprintf(buf, buflen, "call %llu %s (%d)",
                 (unsigned long long)ev->u.call_state.call,
                 call_state_name(ev->u.call_state.state),
                 ev->u.call_state.last_code);
        return 1;
    case SP_EV_CALL_MEDIA_STATE:
        snprintf(buf, buflen, "call %llu audio %s",
                 (unsigned long long)ev->u.media.call,
                 ev->u.media.active ? "active" : "inactive");
        return 1;
    case SP_EV_INCOMING_CALL:
        snprintf(buf, buflen, "incoming call %llu from %s",
                 (unsigned long long)ev->u.incoming.call,
                 ev->u.incoming.from_uri ? ev->u.incoming.from_uri : "unknown");
        return 1;
    case SP_EV_DTMF_RECEIVED:
        snprintf(buf, buflen, "call %llu received DTMF %c",
                 (unsigned long long)ev->u.dtmf.call, ev->u.dtmf.digit);
        return 1;
    case SP_EV_XFER_STATUS:
        snprintf(buf, buflen, "transfer of call %llu: %d %s",
                 (unsigned long long)ev->u.xfer.call, ev->u.xfer.code,
                 ev->u.xfer.reason ? ev->u.xfer.reason : "");
        return 1;
    case SP_EV_TRANSPORT_ERROR:
        snprintf(buf, buflen, "transport error: %s",
                 ev->u.transport.detail ? ev->u.transport.detail : "unknown");
        return 1;
    case SP_EV_LOG:
        snprintf(buf, buflen, "%s", ev->u.log.msg ? ev->u.log.msg : "");
        return ev->u.log.msg && ev->u.log.msg[0] ? 1 : 0;
    case SP_EV_SIP_MESSAGE:
        /* Not recorded: one row per SIP message would duplicate the
           SIP log and bury everything else in the history. */
        return 0;
    case SP_EV_CALL_QUALITY:
        /* Not recorded either: a sample every couple of seconds per
           call would bury the log. The final figures go to the call
           history instead, which is where they are useful later. */
        return 0;
    case SP_EV_TRANSCRIPT:
        /* Pending is said by the call ending; only the outcome is
           news. */
        if (ev->u.transcript.state == SP_TRANSCRIPT_READY)
            snprintf(buf, buflen, "transcript of call record %lld ready",
                     (long long)ev->u.transcript.history_id);
        else if (ev->u.transcript.state == SP_TRANSCRIPT_FAILED)
            snprintf(buf, buflen, "transcript of call record %lld failed%s%s",
                     (long long)ev->u.transcript.history_id,
                     ev->u.transcript.detail ? ": " : "",
                     ev->u.transcript.detail ? ev->u.transcript.detail : "");
        else
            return 0;
        return 1;
    }
    return 0;
}

sp_actionlog *sp_actionlog_create(struct sp_store *store, int max_rows) {
    if (!store)
        return NULL;
    sp_actionlog *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->store = store;
    l->db = sp_store_handle(store);
    l->max_rows = max_rows;
    if (pthread_mutex_init(&l->lock, NULL) != 0) {
        free(l);
        return NULL;
    }
    static const char *SQL =
        "INSERT INTO action_log(ts_us, level, text) VALUES(?,?,?);";
    if (sqlite3_prepare_v2(l->db, SQL, -1, &l->ins, NULL) != SQLITE_OK) {
        pthread_mutex_destroy(&l->lock);
        free(l);
        return NULL;
    }
    return l;
}

static void free_rows(sp_actionlog *l) {
    for (size_t i = 0; i < l->n_rows; i++)
        free((void *)l->rows[i].text);
    free(l->rows);
    l->rows = NULL;
    l->n_rows = 0;
}

void sp_actionlog_destroy(sp_actionlog *l) {
    if (!l)
        return;
    sqlite3_finalize(l->ins);
    free_rows(l);
    pthread_mutex_destroy(&l->lock);
    free(l);
}

void sp_actionlog_write(sp_actionlog *l, int level, const char *text) {
    if (!l || !text || !*text)
        return;
    pthread_mutex_lock(&l->lock);
    sqlite3_reset(l->ins);
    sqlite3_clear_bindings(l->ins);
    sqlite3_bind_int64(l->ins, 1, (sqlite3_int64)now_us());
    sqlite3_bind_int(l->ins, 2, level);
    sqlite3_bind_text(l->ins, 3, text, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(l->ins) != SQLITE_DONE)
        l->drops++;
    if (l->max_rows > 0 && ++l->since_prune >= PRUNE_EVERY) {
        l->since_prune = 0;
        sp_store_prune(l->store, "action_log", l->max_rows);
    }
    pthread_mutex_unlock(&l->lock);
}

sp_status sp_actionlog_query(sp_actionlog *l, size_t limit,
                             const sp_action_entry **out, size_t *n_out) {
    if (!out || !n_out)
        return SP_ERR_INVALID_ARG;
    *out = NULL;
    *n_out = 0;
    if (!l)
        return SP_OK;
    if (limit == 0 || limit > 100000)
        limit = 100;

    pthread_mutex_lock(&l->lock);
    free_rows(l);

    /* Our own enum, never operator input; a direction cannot be bound. */
    const char *dir = l->order == SP_ORDER_OLDEST_FIRST ? "ASC" : "DESC";
    char sql[256];
    snprintf(sql, sizeof sql,
             "SELECT ts_us, level, text FROM action_log"
             " ORDER BY id %s LIMIT ?;", dir);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(l->db, sql, -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&l->lock);
        return SP_ERR_INTERNAL;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)limit);

    size_t cap = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (l->n_rows == cap) {
            size_t grow = cap ? cap * 2 : 64;
            sp_action_entry *r = realloc(l->rows, grow * sizeof *r);
            if (!r)
                break;
            l->rows = r;
            cap = grow;
        }
        sp_action_entry *e = &l->rows[l->n_rows];
        memset(e, 0, sizeof *e);
        e->ts_us = (uint64_t)sqlite3_column_int64(st, 0);
        e->level = sqlite3_column_int(st, 1);
        const unsigned char *t = sqlite3_column_text(st, 2);
        e->text = t ? strdup((const char *)t) : strdup("");
        l->n_rows++;
    }
    sqlite3_finalize(st);

    *out = l->rows;
    *n_out = l->n_rows;
    pthread_mutex_unlock(&l->lock);
    return SP_OK;
}

sp_status sp_actionlog_wipe(sp_actionlog *l) {
    if (!l)
        return SP_OK;
    pthread_mutex_lock(&l->lock);
    free_rows(l);
    int rc = sp_store_exec(l->store, "DELETE FROM action_log;");
    pthread_mutex_unlock(&l->lock);
    return rc == 0 ? SP_OK : SP_ERR_INTERNAL;
}

void sp_actionlog_set_order(sp_actionlog *h, sp_order order) {
    if (h)
        h->order = order;
}
