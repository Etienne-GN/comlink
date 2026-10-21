/*
 * The sound library: which ringtones and tones exist, and where.
 *
 * Two directories per slot. The bundled one ships with the package and
 * is read-only; the operator's one lives beside the configuration file
 * and holds anything they imported. A name present in both resolves to
 * the operator's copy, so an import can shadow a bundled sound without
 * the package having to be modified.
 *
 * Deliberately free of pjsip: this is filesystem work and is tested
 * without a media stack. Playback lives in media.c, which needs pjsua.
 */
#ifndef SP_SOUND_H
#define SP_SOUND_H

#include <stdbool.h>
#include <stddef.h>

#include "comlink/core.h"

typedef struct sp_sound_lib sp_sound_lib;

/* config_path is the configuration file; the operator's sounds live in
   a "sounds" directory beside it. */
sp_sound_lib *sp_sound_lib_create(const char *config_path);
void          sp_sound_lib_destroy(sp_sound_lib *lib);

/* The subdirectory a slot uses, e.g. "inbound". Never NULL. */
const char *sp_sound_slot_dir(sp_sound_slot slot);

/* Rescans both directories. Results point into library-owned storage,
   valid until the next scan of the same slot. */
sp_status sp_sound_lib_scan(sp_sound_lib *lib, sp_sound_slot slot,
                            const sp_sound_file **out, size_t *n_out);

/* The sound to use for a slot when the operator has chosen nothing: a
   phone that ships with ringtones should ring out of the box. Returns
   "none" when the slot's directories are empty. Points into
   library-owned storage, valid until the next call for the same slot,
   and deliberately independent of the list sp_sound_lib_scan hands
   out. */
const char *sp_sound_lib_default(sp_sound_lib *lib, sp_sound_slot slot);

/* Turns a configured value into an absolute path.
 *
 * "none" and the empty string mean silence and return NULL with
 * *silent set. An absolute path is taken as given. Anything else is a
 * name looked up in the operator's directory first, then the bundled
 * one. Returns NULL when the name resolves to nothing, which is not
 * the same as silence and is why *silent exists. */
const char *sp_sound_lib_resolve(sp_sound_lib *lib, sp_sound_slot slot,
                                 const char *configured, bool *silent);

/* Copies src into the operator's directory for this slot, creating it
   if needed. Returns the stored path through *stored_out, pointing
   into library-owned storage. Refuses to overwrite a bundled name. */
sp_status sp_sound_lib_import(sp_sound_lib *lib, sp_sound_slot slot,
                              const char *src, const char **stored_out,
                              char *err, size_t errlen);

#endif /* SP_SOUND_H */
