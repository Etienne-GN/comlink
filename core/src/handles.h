/*
 * Generation-tagged handle table.
 *
 * pjsua identifies accounts and calls by a small int indexing a
 * fixed-size table, and reuses a slot once its occupant is gone.
 * Handing that index across an asynchronous ABI boundary is a defect:
 * a frontend holding slot 3 after its call disconnects would operate
 * on whatever call next occupies slot 3.
 *
 * Handles here come from a monotonic counter and are never reused, so
 * a stale handle always resolves to "not found" rather than to the
 * wrong object. Retired rows are removed, not kept: the monotonic
 * counter is what prevents reuse, so retaining them would only grow
 * memory without end on a long-running phone.
 */
#ifndef SP_HANDLES_H
#define SP_HANDLES_H

#include <stddef.h>
#include <stdint.h>

typedef struct sp_handle_table sp_handle_table;

sp_handle_table *sp_ht_create(void);
void             sp_ht_destroy(sp_handle_table *t);

/* Returns a fresh never-reused handle, or 0 on allocation failure. */
uint64_t sp_ht_add(sp_handle_table *t, int native_id, void *data);

/* 0 on success, -1 if the handle is unknown or retired.
   native_id and data may each be NULL if not wanted. */
int sp_ht_lookup(sp_handle_table *t, uint64_t id, int *native_id, void **data);

/* Live handle currently bound to native_id, or 0 if none. */
uint64_t sp_ht_by_native(sp_handle_table *t, int native_id);

/* Replace the stored data pointer. 0 on success, -1 if not found. */
int sp_ht_set_data(sp_handle_table *t, uint64_t id, void *data);

/* Rebind the native id. Needed because a handle must exist before the
   native id is known: pjsua fires callbacks during the call that
   creates the object. 0 on success, -1 if not found. */
int sp_ht_set_native(sp_handle_table *t, uint64_t id, int native_id);

/* Copies every live handle under one lock. Enumerating by index is
   not safe: retiring a row moves the last row into its place, so an
   index walk can skip an entry or visit one twice. Writes at most cap
   ids and reports the true count in n_out. */
void sp_ht_snapshot(sp_handle_table *t, uint64_t *ids, size_t cap,
                    size_t *n_out);

void   sp_ht_retire(sp_handle_table *t, uint64_t id);
size_t sp_ht_live_count(sp_handle_table *t);

#endif /* SP_HANDLES_H */
