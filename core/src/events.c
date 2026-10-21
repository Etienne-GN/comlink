#include "events.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct node {
    sp_event     ev;
    /* Owned copies of every string the event points at. */
    char        *s1;
    struct node *next;
} node;

struct sp_event_queue {
    node           *head, *tail;
    size_t          depth;
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    pthread_t       thread;
    sp_event_cb     cb;
    void           *user;
    int             running;
    int             stopping;
    int             started;
};

sp_event_queue *sp_evq_create(void) {
    sp_event_queue *q = calloc(1, sizeof *q);
    if (!q)
        return NULL;
    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        free(q);
        return NULL;
    }
    if (pthread_cond_init(&q->cv, NULL) != 0) {
        pthread_mutex_destroy(&q->lock);
        free(q);
        return NULL;
    }
    return q;
}

/* The single string field each event variant may carry. Keeping this in
   one place means a new event type cannot quietly skip the deep copy. */
static const char **event_string_slot(sp_event *ev) {
    switch (ev->type) {
    case SP_EV_ACCOUNT_STATE:    return &ev->u.account.detail;
    case SP_EV_INCOMING_CALL:    return &ev->u.incoming.from_uri;
    case SP_EV_XFER_STATUS:      return &ev->u.xfer.reason;
    case SP_EV_TRANSPORT_ERROR:  return &ev->u.transport.detail;
    case SP_EV_LOG:              return &ev->u.log.msg;
    case SP_EV_TRANSCRIPT:       return &ev->u.transcript.detail;
    case SP_EV_CALL_STATE:
    case SP_EV_CALL_MEDIA_STATE:
    case SP_EV_DTMF_RECEIVED:
    case SP_EV_SIP_MESSAGE:   /* carries fixed buffers, nothing to copy */
    case SP_EV_CALL_QUALITY:  /* fixed-size numbers, nothing to copy */
        return NULL;
    }
    return NULL;
}

static void node_free(node *n) {
    if (!n)
        return;
    free(n->s1);
    free(n);
}

int sp_evq_post(sp_event_queue *q, const sp_event *ev) {
    if (!q || !ev)
        return -1;

    node *n = calloc(1, sizeof *n);
    if (!n)
        return -1;
    n->ev = *ev;

    const char **slot = event_string_slot(&n->ev);
    if (slot && *slot) {
        n->s1 = strdup(*slot);
        if (!n->s1) {
            free(n);
            return -1;
        }
        *slot = n->s1;
    }

    pthread_mutex_lock(&q->lock);
    if (q->stopping) {
        pthread_mutex_unlock(&q->lock);
        node_free(n);
        return -1;
    }
    if (q->tail)
        q->tail->next = n;
    else
        q->head = n;
    q->tail = n;
    q->depth++;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

static void *dispatch(void *arg) {
    sp_event_queue *q = arg;
    for (;;) {
        pthread_mutex_lock(&q->lock);
        while (!q->head && q->running)
            pthread_cond_wait(&q->cv, &q->lock);
        node *n = q->head;
        if (!n) {
            pthread_mutex_unlock(&q->lock);
            break;   /* stopped and drained */
        }
        q->head = n->next;
        if (!q->head)
            q->tail = NULL;
        q->depth--;
        sp_event_cb cb = q->cb;
        void *user = q->user;
        pthread_mutex_unlock(&q->lock);

        /* Outside the lock: the frontend is allowed to call back into
           the core from here. */
        if (cb)
            cb(&n->ev, user);
        node_free(n);
    }
    return NULL;
}

int sp_evq_start(sp_event_queue *q, sp_event_cb cb, void *user) {
    if (!q)
        return -1;
    pthread_mutex_lock(&q->lock);
    if (q->started || q->stopping) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    q->cb = cb;
    q->user = user;
    q->running = 1;
    q->started = 1;
    pthread_mutex_unlock(&q->lock);

    if (pthread_create(&q->thread, NULL, dispatch, q) != 0) {
        pthread_mutex_lock(&q->lock);
        q->running = 0;
        q->started = 0;
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    return 0;
}

void sp_evq_stop(sp_event_queue *q) {
    if (!q)
        return;
    pthread_mutex_lock(&q->lock);
    if (q->stopping) {
        pthread_mutex_unlock(&q->lock);
        return;
    }
    q->stopping = 1;
    q->running = 0;
    int joinable = q->started;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->lock);

    if (joinable)
        pthread_join(q->thread, NULL);

    /* Anything the dispatch thread did not reach is freed here. */
    pthread_mutex_lock(&q->lock);
    node *n = q->head;
    q->head = q->tail = NULL;
    q->depth = 0;
    q->started = 0;
    pthread_mutex_unlock(&q->lock);
    while (n) {
        node *next = n->next;
        node_free(n);
        n = next;
    }
}

void sp_evq_destroy(sp_event_queue *q) {
    if (!q)
        return;
    sp_evq_stop(q);
    pthread_cond_destroy(&q->cv);
    pthread_mutex_destroy(&q->lock);
    free(q);
}

size_t sp_evq_depth(sp_event_queue *q) {
    if (!q)
        return 0;
    pthread_mutex_lock(&q->lock);
    size_t d = q->depth;
    pthread_mutex_unlock(&q->lock);
    return d;
}
