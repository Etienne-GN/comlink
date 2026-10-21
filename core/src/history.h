/*
 * Call history: one durable record per call, joined to its own SIP
 * trace by the SIP Call-ID.
 *
 * Written from pjsip worker threads, so state is behind a mutex. A
 * failed write loses one field of history, never a call.
 */
#ifndef SP_HISTORY_H
#define SP_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "comlink/core.h"

typedef struct sp_history sp_history;
struct sp_store;

sp_history *sp_history_create(struct sp_store *store, int max_rows);
void        sp_history_destroy(sp_history *h);

/* Returns the new rowid, or 0 on failure. sip_call_id may be NULL: an
   outgoing call has no Call-ID until its INVITE exists. */
int64_t sp_history_begin(sp_history *h, bool inbound, const char *account,
                         const char *remote_uri, const char *local_uri,
                         const char *sip_call_id);

void sp_history_set_call_id(sp_history *h, int64_t id, const char *cid);
void sp_history_answered(sp_history *h, int64_t id, const char *codec);
void sp_history_ended(sp_history *h, int64_t id, int code, uint32_t dur_s);
void sp_history_recording(sp_history *h, int64_t id, const char *path);

/* Records the final quality snapshot. A snapshot with valid == false
   writes nothing: a call that never carried media must not read back
   as a flawless zero-loss one. */
void sp_history_quality(sp_history *h, int64_t id,
                        const sp_call_quality *q);

sp_status sp_history_query(sp_history *h, size_t limit,
                           const sp_call_record **out, size_t *n_out);
sp_status sp_history_wipe(sp_history *h);

/* Clears any earlier failure reason. */
void sp_history_transcript_state(sp_history *h, int64_t id,
                                 sp_transcript_state st);
/* FAILED, with why for the Calls list. why may be NULL. */
void sp_history_transcript_failed(sp_history *h, int64_t id,
                                  const char *why);
/* Replaces the call's lines and marks it READY. 0 on success; on -1
   the call is left with no lines and marked FAILED. */
int  sp_history_transcript_store(sp_history *h, int64_t id,
                                 const sp_transcript_line *lines, size_t n);
/* Held by h, valid until the next read or sp_history_destroy. */
sp_status sp_history_transcript_read(sp_history *h, int64_t id,
                                     const sp_transcript_line **out,
                                     size_t *n_out);
/* A call still waiting for its transcript at startup. */
typedef struct {
    int64_t id;
    char   *remote_uri;
} sp_pending_call;

/* Every PENDING call, oldest first, so a resumed queue keeps its
   order. *out is the caller's: sp_history_pending_free. */
int  sp_history_transcript_pending(sp_history *h, sp_pending_call **out,
                                   size_t *n_out);
void sp_history_pending_free(sp_pending_call *p, size_t n);

/* Newest or oldest first. Applied in SQL, so a capped read returns
   the oldest rows rather than the newest ones reversed. */
void sp_history_set_order(sp_history *h, sp_order order);

#endif /* SP_HISTORY_H */
