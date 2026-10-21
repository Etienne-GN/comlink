/*
 * Playback of a recorded call through the core's own audio path.
 *
 * One player at a time, connected to conf slot 0 (the sound device)
 * the same way sp_sound_start connects a ringtone: local audio only,
 * routed to whatever output device the operator already chose, and
 * never mixed into a call's own ports.
 *
 * Unlike sound.h, this needs pjsua directly -- the position and
 * duration come straight from the pjmedia WAV port -- which is why it
 * lives behind core_internal.h rather than being reachable from the
 * public header. The public API (sp_playback_start and friends) is
 * declared in comlink/core.h and implemented in playback.c.
 */
#ifndef SP_PLAYBACK_H
#define SP_PLAYBACK_H

#include <pjsua-lib/pjsua.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* WAV goes through pjsua's own player (pjsua_player_create), the same
   as always. OggOpus has no pjsua-level player at all -- it is our
   own reader port (oggopus.h), added to the conference bridge and
   torn down by hand exactly as call.c's Opus recorder is -- so the
   two need different teardown and different position/duration
   queries. `kind` is which shape the rest of the state is in. */
typedef enum {
    SP_PLAYBACK_NONE,
    SP_PLAYBACK_WAV,
    SP_PLAYBACK_OGGOPUS
} sp_playback_kind;

/* Embedded directly in sp_core; there is exactly one playback slot
   per core, matching the "starting while something is already
   playing stops that first" rule in sp_playback_start's contract. */
typedef struct {
    sp_playback_kind kind;
    pjsua_player_id id;      /* WAV: PJSUA_INVALID_ID when not loaded */
    pjmedia_port    *ogg_port; /* OggOpus: NULL when not loaded */
    pj_pool_t       *ogg_pool; /* kept alive while ogg_port exists;
                                  see speed_pool below for why ogg_port
                                  itself no longer sits in the bridge */
    uint32_t        ogg_duration_ms; /* cached at start: see
                                        sp_oggopus_duration_ms's own
                                        comment on why it is not a
                                        cheap per-poll query */
    /* The speed wrapper (speedport.h) sits between whichever source
       is loaded (the WAV player's own port, or ogg_port) and
       conference bridge slot 0: source -> speed_port -> slot 0. It,
       not the source, is what gets added to the bridge and connected
       -- see playback.c's sp_playback_start and destroy_loaded for
       why, and sp_playback_pause/resume for why they now disconnect
       speed_slot instead of the source's own slot. NULL/PJSUA_INVALID_ID
       when nothing is loaded. speed_pool is kept alive while
       speed_port sits in the bridge, same rule as ogg_pool. */
    pjmedia_port    *speed_port;
    pj_pool_t       *speed_pool;
    pjsua_conf_port_id speed_slot;
    /* Current playback speed multiplier: 0.5, 1.0 or 2.0. Reset to
       1.0 by sp_playback_start. Mirrored into speed_port via
       sp_speedport_set_speed whenever sp_playback_set_speed changes
       it; kept here too so sp_playback_get can report it without
       reaching into the wrapper's own (pjproject-visible) type. */
    float           speed;
    char            path[512];
    bool            paused;
    /* WAV: set from pjmedia_wav_player_set_eof_cb2's callback, which
       runs on a pjmedia thread. OggOpus: no such callback exists for
       a hand-rolled port, so sp_playback_get instead polls
       sp_oggopus_reader_is_eof and stores the result here itself,
       under the same rule -- never touched under core->lock from a
       pjmedia thread: taking the lock there risks inversion against
       whatever thread already holds it while waiting on pjmedia, and
       a callback must not block. sp_playback_get reaps this flag
       either way and tears the player down on the caller's own
       thread instead. _Atomic keeps the flag itself race-free without
       a lock. */
    _Atomic bool    eof;
} sp_playback_state;

/* PJSUA_INVALID_ID is a pjsua enum value, not usable in a static
   initializer from a header with no pjsua visibility, so the state is
   brought to its rest position here instead of by calloc alone. */
void sp_playback_state_init(sp_playback_state *ps);

#endif /* SP_PLAYBACK_H */
