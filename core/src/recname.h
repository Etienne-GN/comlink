/*
 * What a recording is called.
 *
 * Its own file, and free of pjproject, so the rule can be tested
 * directly: core_internal.h pulls in pjsua-lib and the core tests are
 * given no pjproject include path.
 */
#ifndef SP_RECNAME_H
#define SP_RECNAME_H

#include <stddef.h>

#include "comlink/core.h"

/* call-<stamp>-<id>.wav into out. 0 on success. -1 if it would not
   fit, with out set to the empty string rather than a truncated name
   -- sp_recording_path sizes its own check off strlen(out), so a
   truncated name here must not be handed back as if it were whole. */
int sp_recording_name(char *out, size_t outlen, unsigned long long call,
                      sp_recording_format fmt);

/* <dir>/<generated name> into out. 0 on success. -1 if it would not
   fit, with out set to the empty string rather than a truncated path
   -- a caller that forgets to check the return must not be handed
   something that looks like a usable path but is not where it
   claims to be. */
int sp_recording_path(char *out, size_t outlen, const char *dir,
                      unsigned long long call, sp_recording_format fmt);

#endif
