/*
 * Event queue with a single dispatch thread.
 *
 * pjsip callbacks arrive on pjsip worker threads. Delivering frontend
 * callbacks from those threads would let a slow UI stall signalling,
 * and on Android would require attaching every pjsip worker to the
 * JVM. Everything is funnelled through one core-owned thread instead,
 * so Android needs exactly one AttachCurrentThread.
 *
 * The callback runs outside the queue lock, so a frontend may call
 * back into the core from inside it without deadlocking.
 */
#ifndef SP_EVENTS_H
#define SP_EVENTS_H

#include <stddef.h>

#include "comlink/core.h"

typedef struct sp_event_queue sp_event_queue;

sp_event_queue *sp_evq_create(void);
void            sp_evq_destroy(sp_event_queue *q);

/* Deep-copies the event and every string reachable through it, so
   payload lifetime is the callback's duration only. Safe from any
   thread. 0 on success, -1 if stopped or out of memory. */
int sp_evq_post(sp_event_queue *q, const sp_event *ev);

int  sp_evq_start(sp_event_queue *q, sp_event_cb cb, void *user);
void sp_evq_stop(sp_event_queue *q);   /* drains, joins; idempotent */

size_t sp_evq_depth(sp_event_queue *q);

#endif /* SP_EVENTS_H */
