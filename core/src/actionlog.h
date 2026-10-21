/*
 * Action log: a persisted, human-readable history of what the phone
 * did, fed from the event stream.
 *
 * It lives in the core rather than in a Qt widget so both frontends
 * show the same history, in the same words, from the same source.
 */
#ifndef SP_ACTIONLOG_H
#define SP_ACTIONLOG_H

#include <stddef.h>

#include "comlink/core.h"

typedef struct sp_actionlog sp_actionlog;
struct sp_store;

sp_actionlog *sp_actionlog_create(struct sp_store *store, int max_rows);
void          sp_actionlog_destroy(sp_actionlog *l);

/* Safe from any thread. */
void sp_actionlog_write(sp_actionlog *l, int level, const char *text);

sp_status sp_actionlog_query(sp_actionlog *l, size_t limit,
                             const sp_action_entry **out, size_t *n_out);
sp_status sp_actionlog_wipe(sp_actionlog *l);

/* One line describing an event, in wording shared by every frontend.
   Returns 0 if this event type is not worth recording. */
int sp_actionlog_format(const sp_event *ev, char *buf, size_t buflen);

/* Newest or oldest first. Applied in SQL, so a capped read returns
   the oldest rows rather than the newest ones reversed. */
void sp_actionlog_set_order(sp_actionlog *h, sp_order order);

#endif /* SP_ACTIONLOG_H */
