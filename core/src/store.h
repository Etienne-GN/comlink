/*
 * SQLite store owned by the core.
 *
 * The schema version lives in PRAGMA user_version and migrations run
 * inside a transaction, so an interrupted upgrade cannot leave a
 * half-applied schema behind.
 *
 * sp_store_handle returns void * on purpose: this header must be
 * includable without sqlite3.h, which is what keeps the public ABI
 * free of SQLite types.
 */
#ifndef SP_STORE_H
#define SP_STORE_H

#include <stddef.h>

typedef struct sp_store sp_store;

/* Opens or creates the database and migrates it to the current schema.
   NULL on failure, with err filled in. */
sp_store *sp_store_open(const char *path, char *err, size_t errlen);
void      sp_store_close(sp_store *s);

int sp_store_schema_version(sp_store *s);        /* -1 on error */
int sp_store_exec(sp_store *s, const char *sql); /* 0 ok, -1 error */

/* Never NULL, so it is always safe to print. */
const char *sp_store_error(sp_store *s);

/* The underlying sqlite3 *, for the log modules. */
void *sp_store_handle(sp_store *s);

/* Deletes all but the newest keep rows of table. Returns the number of
   rows deleted, or -1. table must be one of the known table names: a
   table name cannot be bound as a parameter, so it is checked against
   an allowlist rather than interpolated blindly. */
int sp_store_prune(sp_store *s, const char *table, int keep);

#endif /* SP_STORE_H */
