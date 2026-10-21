/* Internal to core/src -- this header may include pjmedia.
   core/include/comlink/core.h must stay pjproject-free; test_abi_purity
   enforces that.

   Shared contract between the OggOpus writer and the OggOpus reader
   (the reader lands in a later task): the granule rate. Per RFC 7845
   S4, an OggOpus stream's granule positions are always counted in
   48 kHz samples, regardless of the stream's own clock rate. A file
   written with granules in its capture rate plays at the correct
   pitch and seeks to the wrong place -- the single most common
   OggOpus bug, and the reason it is a named constant rather than a
   literal repeated at each call site. */
#ifndef SP_OGGOPUS_H
#define SP_OGGOPUS_H

#include "comlink/core.h"

#include <pjmedia/port.h>
#include <pj/pool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SP_OGGOPUS_GRANULE_RATE 48000u

/* Creates a pjmedia_port whose put_frame() Opus-encodes and Ogg-muxes
   its PCM into the file at `path`. `clock_rate` must be one Opus
   itself accepts (8000/12000/16000/24000/48000) and `channels` is 1
   or 2; `samples_per_frame` only sizes the port's declared info --
   put_frame() buffers internally, so it need not be called with that
   many samples each time (the bridge's frame size and Opus's legal
   frame sizes are different things).

   put_frame() runs off pool-backed memory, buffered stdio, and one
   bounded 4000-byte stack buffer per frame for the encoded packet
   (oggopus_write.c's SP_OGGOPUS_MAX_PACKET): no core lock, no
   callback into the core, safe to call from the media thread.
   on_destroy() encodes any buffered remainder, flags
   the final page end-of-stream, and closes the file -- a file closed
   any other way has no EOS page and a wrong final granule. */
sp_status sp_oggopus_writer_create(pj_pool_t *pool, const char *path,
                                   unsigned clock_rate, unsigned channels,
                                   unsigned samples_per_frame,
                                   pjmedia_port **out);

/* Creates a pjmedia_port whose get_frame() reads Ogg pages from `path`,
   Opus-decodes them, and hands the bridge PCM at `clock_rate`/`channels`.
   `clock_rate` must be one Opus itself accepts, exactly like the
   writer -- it is what the decoder is created with, never OpusHead's
   own input_sample_rate field, which is documentation of the original
   capture and not an instruction to a decoder (see oggopus_read.c).
   `channels` must match OpusHead's channel count exactly: this reader
   only understands channel mapping family 0 (RFC 7845 S5.1.1.1), the
   only family the writer ever produces, so a mismatch is reported
   rather than guessed at. `samples_per_frame` only sizes the port's
   declared info, matching the writer's own contract: get_frame() is
   handed whatever size the bridge asks for, and serves it out of an
   internal buffer regardless.

   get_frame() runs entirely off pool-backed memory and buffered stdio:
   no core lock, no callback into the core, safe to call from the
   media thread. Once the file's audio is exhausted, get_frame() keeps
   returning silence (PJMEDIA_FRAME_TYPE_NONE, a zeroed buffer) rather
   than garbage or a repeated last frame; sp_oggopus_reader_is_eof
   reports that state without touching the port's own machinery. */
sp_status sp_oggopus_reader_create(pj_pool_t *pool, const char *path,
                                   unsigned clock_rate, unsigned channels,
                                   unsigned samples_per_frame,
                                   pjmedia_port **out);

/* True once the reader has served every real sample the file has and
   will only produce silence from here on. Safe to poll from a thread
   other than the one calling get_frame -- backed by an atomic, the
   same shape as sp_playback_state's own eof flag, for the same
   reason: get_frame runs on the media thread and must not be made to
   wait on whoever is polling this. */
sp_status sp_oggopus_reader_is_eof(pjmedia_port *port, pj_bool_t *out);

/* Playback position, in milliseconds of real (post-pre-skip) audio
   already served to the bridge. Like the eof flag above, read without
   synchronising with get_frame; a caller polling this for a position
   bar tolerates the same staleness pjmedia_wav_player_port_get_pos
   already does for the WAV path. */
sp_status sp_oggopus_reader_get_pos_ms(pjmedia_port *port, uint32_t *out);

/* The file's playback duration in milliseconds: the last page's
   granule position minus OpusHead's pre-skip (RFC 7845 S4.3), not a
   header field -- OggOpus has no duration field, and the granule
   itself is not the duration because it is seeded at the pre-skip
   (see sp_oggopus_writer_create's own comment on why). Opens and
   scans the file's pages directly rather than going through a reader
   port, so it works standalone (playback needs a duration before it
   has decided whether to create one). SP_ERR_IO if the file cannot be
   opened, SP_ERR_MEDIA if it has no page carrying an OpusHead. */
sp_status sp_oggopus_duration_ms(const char *path, uint32_t *out);

#ifdef __cplusplus
}
#endif
#endif
