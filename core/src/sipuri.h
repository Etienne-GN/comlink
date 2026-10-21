/*
 * Caller identity parsed out of a SIP From/To value.
 *
 * Pure string work, in the core rather than a frontend: both platforms
 * need "who is calling" and a parser written twice would diverge on
 * exactly the awkward cases that matter, like a display name
 * containing an angle bracket or a PSTN number with a plus.
 */
#ifndef SP_SIPURI_H
#define SP_SIPURI_H

#include <stddef.h>

typedef struct {
    char display[96];   /* "" when the URI carried no display name */
    char user[128];     /* extension or number; "" if absent */
    char host[128];     /* no port, no parameters */
} sp_party;

/* Never fails: an unparseable input yields empty fields rather than
   guesses. out is always fully initialised. */
void sp_sipuri_parse(const char *raw, sp_party *out);

/* The best available label for a UI: display name, else user, else
   host, else "unknown". Never NULL and never empty, because a call
   display showing a blank caller is worse than one showing "unknown". */
const char *sp_sipuri_label(const sp_party *p);

#endif /* SP_SIPURI_H */
