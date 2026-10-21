/*
 * A conference-bridge sink that appends what it hears to a file as raw
 * mono s16le: the transcript's capture, one per side of a call.
 *
 * Raw rather than WAV because nothing reads these but the transcriber,
 * and a header would need rewriting at close for no reader's benefit.
 *
 * The bridge finalises a port on its own clock, after the caller's
 * pjsua_conf_remove_port and pjmedia_port_destroy have returned (see
 * test_recording.c). on_closed is how the owner learns the file is
 * complete: it runs exactly once, from on_destroy, after fclose.
 */
#ifndef SP_PCMSINK_H
#define SP_PCMSINK_H

#include <stdbool.h>

#include <pjmedia/port.h>

#include "comlink/core.h"

/* ok is false if any write or the close failed. */
typedef void (*sp_pcmsink_closed_cb)(void *user, bool ok);

/* The port takes its own pool from pool's factory and releases it in
   on_destroy, as the OggOpus writer does. The file is created 0600,
   and a symlink at path is refused rather than followed. SP_ERR_IO
   when it cannot be created. */
sp_status sp_pcmsink_create(pj_pool_t *pool, const char *path,
                            unsigned clock_rate, unsigned samples_per_frame,
                            sp_pcmsink_closed_cb on_closed, void *user,
                            pjmedia_port **out);

#endif /* SP_PCMSINK_H */
