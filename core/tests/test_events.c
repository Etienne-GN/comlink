#include "sp_test.h"
#include "events.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

typedef struct {
    pthread_mutex_t m;
    int             count;
    pthread_t       seen_tid;
    int             distinct_tids;
    char            last_uri[128];
    sp_event_type   last_type;
} sink;

static void on_event(const sp_event *ev, void *user) {
    sink *s = user;
    pthread_mutex_lock(&s->m);
    if (s->count && !pthread_equal(s->seen_tid, pthread_self()))
        s->distinct_tids++;
    s->seen_tid = pthread_self();
    s->count++;
    s->last_type = ev->type;
    if (ev->type == SP_EV_INCOMING_CALL && ev->u.incoming.from_uri)
        snprintf(s->last_uri, sizeof s->last_uri, "%s", ev->u.incoming.from_uri);
    pthread_mutex_unlock(&s->m);
}

static int wait_for_count(sink *s, int want, int max_ms) {
    for (int i = 0; i < max_ms; i++) {
        pthread_mutex_lock(&s->m);
        int c = s->count;
        pthread_mutex_unlock(&s->m);
        if (c >= want)
            return 1;
        usleep(1000);
    }
    return 0;
}

/* Hoisted to file scope: a nested function is a GCC extension and the
   build runs with -Werror. */
static int order_seen[100];
/* Shared with the dispatch thread: atomic, or the test itself is racy
   and the sanitizer is right to say so. */
static _Atomic int order_n;

static void record_order(const sp_event *ev, void *user) {
    (void)user;
    int i = atomic_load(&order_n);
    if (i < 100) {
        order_seen[i] = ev->u.call_state.last_code;
        atomic_store(&order_n, i + 1);
    }
}

SP_TEST(posted_event_reaches_the_callback) {
    sink s = {0}; pthread_mutex_init(&s.m, NULL);
    sp_event_queue *q = sp_evq_create();
    SP_ASSERT(sp_evq_start(q, on_event, &s) == 0);
    sp_event ev = {0};
    ev.type = SP_EV_CALL_STATE;
    ev.u.call_state.call = 7;
    SP_ASSERT(sp_evq_post(q, &ev) == 0);
    SP_ASSERT(wait_for_count(&s, 1, 2000));
    SP_ASSERT(s.last_type == SP_EV_CALL_STATE);
    sp_evq_stop(q); sp_evq_destroy(q);
}

/* Payload pointers are only valid during the callback, so the queue must
   own a copy: the caller's buffer is overwritten before dispatch runs. */
SP_TEST(event_strings_are_deep_copied) {
    sink s = {0}; pthread_mutex_init(&s.m, NULL);
    sp_event_queue *q = sp_evq_create();

    char uri[64];
    snprintf(uri, sizeof uri, "sip:alice@example.lan");
    sp_event ev = {0};
    ev.type = SP_EV_INCOMING_CALL;
    ev.u.incoming.from_uri = uri;

    SP_ASSERT(sp_evq_post(q, &ev) == 0);
    memset(uri, 'X', sizeof uri - 1);   /* clobber before dispatch starts */
    uri[sizeof uri - 1] = '\0';

    SP_ASSERT(sp_evq_start(q, on_event, &s) == 0);
    SP_ASSERT(wait_for_count(&s, 1, 2000));
    SP_ASSERT_STR(s.last_uri, "sip:alice@example.lan");
    sp_evq_stop(q); sp_evq_destroy(q);
}

/* Spec 7: events arrive only from the one core-owned dispatch thread. */
SP_TEST(all_events_dispatch_on_a_single_thread) {
    sink s = {0}; pthread_mutex_init(&s.m, NULL);
    sp_event_queue *q = sp_evq_create();
    SP_ASSERT(sp_evq_start(q, on_event, &s) == 0);
    for (int i = 0; i < 200; i++) {
        sp_event ev = {0};
        ev.type = SP_EV_LOG;
        ev.u.log.level = 3;
        ev.u.log.msg = "tick";
        SP_ASSERT(sp_evq_post(q, &ev) == 0);
    }
    SP_ASSERT(wait_for_count(&s, 200, 5000));
    SP_ASSERT(s.distinct_tids == 0);
    sp_evq_stop(q); sp_evq_destroy(q);
}

SP_TEST(order_is_preserved) {
    atomic_store(&order_n, 0);
    sp_event_queue *q = sp_evq_create();
    SP_ASSERT(sp_evq_start(q, record_order, NULL) == 0);
    for (int i = 0; i < 100; i++) {
        sp_event ev = {0};
        ev.type = SP_EV_CALL_STATE;
        ev.u.call_state.last_code = i;
        sp_evq_post(q, &ev);
    }
    for (int i = 0; i < 5000 && atomic_load(&order_n) < 100; i++)
        usleep(1000);
    SP_ASSERT(atomic_load(&order_n) == 100);
    for (int i = 0; i < 100; i++)
        SP_ASSERT(order_seen[i] == i);
    sp_evq_stop(q); sp_evq_destroy(q);
}

SP_TEST(events_posted_before_start_are_not_lost) {
    sink s = {0}; pthread_mutex_init(&s.m, NULL);
    sp_event_queue *q = sp_evq_create();
    sp_event ev = {0}; ev.type = SP_EV_LOG; ev.u.log.msg = "early";
    SP_ASSERT(sp_evq_post(q, &ev) == 0);
    SP_ASSERT(sp_evq_depth(q) == 1);
    SP_ASSERT(sp_evq_start(q, on_event, &s) == 0);
    SP_ASSERT(wait_for_count(&s, 1, 2000));
    sp_evq_stop(q); sp_evq_destroy(q);
}

SP_TEST(stop_is_idempotent) {
    sink s = {0}; pthread_mutex_init(&s.m, NULL);
    sp_event_queue *q = sp_evq_create();
    sp_evq_start(q, on_event, &s);
    sp_evq_stop(q);
    sp_evq_stop(q);
    sp_evq_destroy(q);
}

SP_TEST(post_after_stop_is_rejected_not_a_crash) {
    sink s = {0}; pthread_mutex_init(&s.m, NULL);
    sp_event_queue *q = sp_evq_create();
    sp_evq_start(q, on_event, &s);
    sp_evq_stop(q);
    sp_event ev = {0}; ev.type = SP_EV_LOG; ev.u.log.msg = "late";
    SP_ASSERT(sp_evq_post(q, &ev) != 0);
    sp_evq_destroy(q);
}

SP_TEST(queued_events_still_drain_when_stop_is_called) {
    sink s = {0}; pthread_mutex_init(&s.m, NULL);
    sp_event_queue *q = sp_evq_create();
    for (int i = 0; i < 50; i++) {
        sp_event ev = {0}; ev.type = SP_EV_LOG; ev.u.log.msg = "queued";
        sp_evq_post(q, &ev);
    }
    SP_ASSERT(sp_evq_start(q, on_event, &s) == 0);
    sp_evq_stop(q);
    /* Every queued event was either delivered or freed; nothing leaks
       and depth is zero after stop. */
    SP_ASSERT(sp_evq_depth(q) == 0);
    sp_evq_destroy(q);
}

/* A frontend is promised it may call back into the core from inside the
   callback, which requires the callback to run outside the queue lock. */
static sp_event_queue *reentrant_q;
static _Atomic int reentrant_ok;

static void reentrant_cb(const sp_event *ev, void *user) {
    (void)user;
    if (ev->type == SP_EV_LOG) {
        /* Touching the queue from inside its own callback must not
           deadlock. */
        (void)sp_evq_depth(reentrant_q);
        atomic_store(&reentrant_ok, 1);
    }
}

SP_TEST(callback_may_reenter_the_queue_without_deadlock) {
    atomic_store(&reentrant_ok, 0);
    reentrant_q = sp_evq_create();
    SP_ASSERT(sp_evq_start(reentrant_q, reentrant_cb, NULL) == 0);
    sp_event ev = {0}; ev.type = SP_EV_LOG; ev.u.log.msg = "reenter";
    sp_evq_post(reentrant_q, &ev);
    for (int i = 0; i < 2000 && !atomic_load(&reentrant_ok); i++)
        usleep(1000);
    SP_ASSERT(atomic_load(&reentrant_ok) == 1);
    sp_evq_stop(reentrant_q); sp_evq_destroy(reentrant_q);
}

static char g_tr_detail[64];
static _Atomic int g_tr_seen;
static void record_transcript(const sp_event *ev, void *user) {
    (void)user;
    if (ev->type != SP_EV_TRANSCRIPT)
        return;
    snprintf(g_tr_detail, sizeof g_tr_detail, "%s",
             ev->u.transcript.detail ? ev->u.transcript.detail : "");
    atomic_store(&g_tr_seen, 1);
}

SP_TEST(a_transcript_event_carries_its_own_copy_of_the_detail) {
    atomic_store(&g_tr_seen, 0);
    sp_event_queue *q = sp_evq_create();
    char detail[32] = "no speech model";
    sp_event ev = {0};
    ev.type = SP_EV_TRANSCRIPT;
    ev.u.transcript.history_id = 7;
    ev.u.transcript.state = SP_TRANSCRIPT_FAILED;
    ev.u.transcript.detail = detail;
    SP_ASSERT(sp_evq_post(q, &ev) == 0);
    strcpy(detail, "overwritten");
    SP_ASSERT(sp_evq_start(q, record_transcript, NULL) == 0);
    for (int i = 0; i < 2000 && !atomic_load(&g_tr_seen); i++)
        usleep(1000);
    SP_ASSERT(atomic_load(&g_tr_seen));
    SP_ASSERT_STR(g_tr_detail, "no speech model");
    sp_evq_stop(q); sp_evq_destroy(q);
}

int main(void) {
    SP_RUN(posted_event_reaches_the_callback);
    SP_RUN(event_strings_are_deep_copied);
    SP_RUN(all_events_dispatch_on_a_single_thread);
    SP_RUN(order_is_preserved);
    SP_RUN(events_posted_before_start_are_not_lost);
    SP_RUN(stop_is_idempotent);
    SP_RUN(post_after_stop_is_rejected_not_a_crash);
    SP_RUN(queued_events_still_drain_when_stop_is_called);
    SP_RUN(callback_may_reenter_the_queue_without_deadlock);
    SP_RUN(a_transcript_event_carries_its_own_copy_of_the_detail);
    return SP_TEST_RESULT();
}
