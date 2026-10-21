/*
 * A call's two sides, captured for its transcript while the call is
 * up: the far end from the call's own slot, this phone from slot 0.
 *
 * The capture outlives its call briefly. The bridge closes the files
 * on its own clock after the call is torn down, and the job goes to
 * the transcriber only once both are complete.
 */
#ifndef SP_TRANSCAP_H
#define SP_TRANSCAP_H

#include <stdint.h>

#include <pjsua-lib/pjsua.h>

#include "comlink/core.h"

typedef struct sp_transcap sp_transcap;

/* NULL on failure, with nothing left in the bridge or on disk. */
sp_transcap *sp_transcap_start(sp_core *core, int64_t history_id,
                               pjsua_conf_port_id call_slot);

/* After a re-INVITE the call may have a new slot. Idempotent. */
void sp_transcap_follow(sp_transcap *c, pjsua_conf_port_id call_slot);

/* Whether this phone's microphone feeds the "Me" side: only while the
   call is the one being spoken to. A held call keeps its timeline, in
   silence, and never hears what is said to the next caller. */
void sp_transcap_hear_mic(sp_transcap *c, bool on);

/* Tests only. */
pjsua_conf_port_id sp_transcap_mic_slot(const sp_transcap *c);

/* Takes both sinks out of the bridge. far_label is copied. The
   capture frees itself once the bridge has closed both files; the
   caller forgets the pointer. */
void sp_transcap_stop(sp_transcap *c, const char *far_label);

#endif /* SP_TRANSCAP_H */
