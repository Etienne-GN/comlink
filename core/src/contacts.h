/*
 * The contact book: who a number belongs to.
 *
 * Embedded rather than a module, on purpose. Once the call panel can
 * show a name, a build where it sometimes does and sometimes does not
 * is a build whose panel cannot be trusted, and an operator reading a
 * name off a screen has to be able to trust it.
 *
 * Read from the UI thread and from pjsip workers -- an incoming call
 * asks "who is this" from a callback -- so state is behind a mutex,
 * as in history.
 */
#ifndef SP_CONTACTS_H
#define SP_CONTACTS_H

#include <stddef.h>
#include <stdint.h>

#include "comlink/core.h"

typedef struct sp_contacts sp_contacts;
struct sp_store;

sp_contacts *sp_contacts_create(struct sp_store *store);
void         sp_contacts_destroy(sp_contacts *c);

/* The form two spellings of the same number have in common.
 *
 * Takes the user part of a SIP URI, drops everything that is not a
 * digit apart from a leading '+', and reduces a NANP number to its ten
 * significant digits, so +1-418-555-1234 and (418) 555-1234 are one
 * number. An extension is left alone and therefore matches only
 * itself.
 *
 * out is always NUL-terminated. Never fails: an input with no digits
 * in it normalises to the empty string, which matches nothing.
 */
void sp_contacts_normalise(const char *in, char *out, size_t outlen);

sp_status sp_contacts_query(sp_contacts *c, const char *search, size_t limit,
                            const sp_contact **out, size_t *n_out);
sp_status sp_contacts_get(sp_contacts *c, int64_t id, const sp_contact **out);
sp_status sp_contacts_save(sp_contacts *c, const sp_contact *in,
                           int64_t *id_out);
sp_status sp_contacts_delete(sp_contacts *c, int64_t id);
sp_status sp_contacts_lookup(sp_contacts *c, const char *uri,
                             const sp_contact **out);

/* Same contract as sp_contact_picture: SP_OK with *data NULL and *len
 * 0 for a contact with no picture, SP_ERR_NOT_FOUND for an unknown id.
 * Core-owned, valid until the next sp_contacts_picture on this book. */
sp_status sp_contacts_picture(sp_contacts *c, int64_t id, const void **data,
                              size_t *len, const char **type);

#endif /* SP_CONTACTS_H */
