/*
 * SIP message log.
 *
 * Capture runs on pjsip worker threads and reads run on a frontend
 * thread, so everything here is behind a mutex.
 *
 * A logging failure must never break a call: an insert that fails is
 * counted and reported once, and signalling carries on.
 */
#ifndef SP_SIPLOG_H
#define SP_SIPLOG_H

#include <stdbool.h>
#include <stddef.h>

#include "comlink/core.h"

typedef struct sp_siplog sp_siplog;
struct sp_store;

sp_siplog *sp_siplog_create(struct sp_store *store, bool redact, int max_rows);
void       sp_siplog_destroy(sp_siplog *l);

/* Safe from any thread. raw need not be NUL-terminated within len. */
void sp_siplog_capture(sp_siplog *l, const char *raw, size_t len,
                       const char *direction, const char *method,
                       int status_code, const char *call_id,
                       const char *account);

sp_status sp_siplog_query(sp_siplog *l, const char *call_id, size_t limit,
                          const sp_sip_message **out, size_t *n_out);
sp_status sp_siplog_wipe(sp_siplog *l);

/* Inserts that failed since creation; 0 when healthy. */
unsigned long sp_siplog_drops(sp_siplog *l);

/* Newest or oldest first. Applied in SQL, so a capped read returns
   the oldest rows rather than the newest ones reversed. */
void sp_siplog_set_order(sp_siplog *h, sp_order order);

#endif /* SP_SIPLOG_H */
