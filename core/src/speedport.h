/* Internal to core/src -- this header may include pjmedia.
   core/include/comlink/core.h must stay pjproject-free; test_abi_purity
   enforces that.

   Time-stretches audio between a source port and the conference bridge
   so a recording can be played back at 0.5x or 2x without shifting its
   pitch. One implementation, used from playback.c for both the WAV
   player's own port and the OggOpus reader port (oggopus.h): a wrapper
   is the only place that covers both, since pjsua_player_create owns
   the WAV port and never hands out anything upstream of it to modify.

   pjmedia_wsola (pjmedia/wsola.h) does the actual stretching -- the
   same primitive pjmedia_delay_buf (pjmedia/src/pjmedia/delaybuf.c)
   is built on, adapted here for a caller-chosen ratio instead of an
   automatically learned one. A naive alternative -- resampling, or
   dropping/repeating samples -- changes pitch along with tempo; WSOLA
   does not, which is the entire reason this file exists instead of a
   five-line decimator. */
#ifndef SP_SPEEDPORT_H
#define SP_SPEEDPORT_H

#include "comlink/core.h"

#include <pj/pool.h>
#include <pjmedia/port.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wraps `source`, whose get_frame() must already produce audio at
   exactly `clock_rate`/`channels`/`samples_per_frame` -- the wrapper
   declares its own port info to match those numbers verbatim (16-bit
   PCM, the only format anything in this codebase produces) and never
   resamples, so a mismatched source plays back wrong from the very
   first frame. The returned port is what the caller adds to the
   conference bridge; `source` itself must NOT also be added -- the
   wrapper pulls from it directly, on whatever thread calls its own
   get_frame(), which is the bridge's media thread once the wrapper is
   connected.

   Starts at 1.0 (plain passthrough, no WSOLA touched at all) until
   sp_speedport_set_speed says otherwise.

   get_frame() does only pool-backed, pre-allocated work: no core
   lock, no per-frame allocation, no call back into the core. Safe to
   run on the media thread, which is the only thread it ever runs on.

   Teardown order matters and is the caller's job, not this port's:
   remove the wrapper from the bridge and destroy it (pjmedia_port_
   destroy) before destroying `source`. on_destroy here never touches
   `source` -- only the wrapper's own pool -- precisely so that
   ordering is the only thing that has to be gotten right, not a
   reference count. Note that pjmedia_conf_add_port installs a group
   lock on a port it is given, so this wrapper's own on_destroy may
   run later, on the bridge's own thread, rather than synchronously
   inside pjmedia_port_destroy -- see oggopus_write.c's writer_on_
   destroy comment for the same point made about the Opus recorder. */
sp_status sp_speedport_create(pj_pool_t *pool, pjmedia_port *source,
                              unsigned clock_rate, unsigned channels,
                              unsigned samples_per_frame,
                              pjmedia_port **out);

/* Changes the active speed. Only 0.5, 1.0 and 2.0 are meaningful --
   anything else is treated as 1.0 -- but this function itself never
   refuses a value; sp_playback_set_speed (comlink/core.h) is what
   validates and returns SP_ERR_INVALID_ARG, before a bad value ever
   reaches here.

   Safe to call from any thread. The change is only actually applied
   at the top of the next get_frame() call, on the media thread, which
   is also where pjmedia_wsola_reset happens -- so a switch never
   splices whatever WSOLA had queued for the old speed onto audio
   meant for the new one, and this function itself never touches
   WSOLA's buffers from the wrong thread. */
void sp_speedport_set_speed(pjmedia_port *port, float speed);

/* Drops whatever WSOLA history and queued/pending output this wrapper
   is holding, without changing the active speed -- the seek
   counterpart to sp_speedport_set_speed's own reset. A seek moves the
   SOURCE's read position directly (pjsua_player_set_pos), which this
   wrapper never hears about on its own: without this call, whatever
   WSOLA had synthesised or queued for the position the source was AT
   before the seek plays out first, splicing a stale or discontinuous
   few tens of milliseconds onto the start of the new position.

   Safe to call from any thread, same rule and same reason as
   sp_speedport_set_speed: applied at the top of the next get_frame()
   call, on the media thread, never touching WSOLA's buffers from the
   wrong thread. Harmless at 1.0x, where get_frame_one never touches
   WSOLA in the first place -- callers need not check the active speed
   before calling this. */
void sp_speedport_reset(pjmedia_port *port);

#ifdef __cplusplus
}
#endif
#endif /* SP_SPEEDPORT_H */
