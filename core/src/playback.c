/*
 * Playing a recording back through the core.
 *
 * Through pjmedia rather than a second audio path: the player's conf
 * port is connected to slot 0, the same slot the sound device already
 * occupies, so the volume and device the operator picked in Settings
 * apply to a played-back recording exactly as they do to a ringtone.
 *
 * Playing while a call is up is refused, not mixed in. A recording
 * out of the speaker reaches the far end through the microphone, so
 * an operator who starts one by accident during a call has played a
 * previous caller's voice down a live line. That refusal is checked
 * before anything else in sp_playback_start, including before the
 * arguments: a bad path while a call is up must still say "call".
 *
 * Two file formats, two different players under the hood. WAV goes
 * through pjsua's own pjsua_player_create, as it always has. OggOpus
 * has no such pjsua-level facility -- it is our own reader port
 * (oggopus.h), added to the conference bridge and torn down by hand,
 * the read-side mirror of how call.c's Opus recorder writes one.
 * Which file is which is decided by its first bytes, not its name:
 * see detect_format below.
 */
#include "core_internal.h"
#include "oggopus.h"
#include "playback.h"
#include "speedport.h"

#include <pjmedia/wav_port.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* pjsua_media_config_default's own defaults, which core.c never
   overrides (see core.c's pjsua_media_config_default call): the
   conference bridge runs at this rate regardless of any call's own
   codec, and an OggOpus reader handed to it has to match or pjmedia
   would need to resample -- matches call.c's own SP_REC_BRIDGE_*,
   duplicated here rather than shared, the same way each of
   oggopus_write.c and oggopus_read.c stand alone from the bridge's
   point of view. */
#define SP_PB_BRIDGE_CLOCK_RATE 16000u
#define SP_PB_BRIDGE_CHANNELS   1u
#define SP_PB_BRIDGE_PTIME_MS   20u
#define SP_PB_BRIDGE_SAMPLES_PER_FRAME \
    (SP_PB_BRIDGE_CLOCK_RATE * SP_PB_BRIDGE_PTIME_MS / 1000u)

void sp_playback_state_init(sp_playback_state *ps) {
    ps->kind = SP_PLAYBACK_NONE;
    ps->id = PJSUA_INVALID_ID;
    ps->ogg_port = NULL;
    ps->ogg_pool = NULL;
    ps->ogg_duration_ms = 0;
    ps->speed_port = NULL;
    ps->speed_pool = NULL;
    ps->speed_slot = PJSUA_INVALID_ID;
    ps->speed = 1.0f;
    ps->path[0] = '\0';
    ps->paused = false;
    atomic_store(&ps->eof, false);
}

/* Bytes per second of payload, for turning a byte offset into
   milliseconds and back: the port's own clock rate, channel count
   and bits per sample, never an assumed 16-bit/16kHz mono layout.
   WAV only -- OggOpus has no byte-addressable payload to convert. */
static uint64_t bytes_per_second(const pjmedia_port *port) {
    uint64_t clock_rate = PJMEDIA_PIA_SRATE(&port->info);
    uint64_t channels = PJMEDIA_PIA_CCNT(&port->info);
    uint64_t bits = PJMEDIA_PIA_BITS(&port->info);
    return clock_rate * channels * (bits / 8);
}

/* pjsua_player_set_pos and pjsua_player_get_pos both convert between
   bytes and the "samples" they take/return using only
   bits_per_sample/8 (see pjsua_aud.c), never multiplying by the
   channel count. Harmless here since pjsua_player_create refuses
   anything but mono, but matched exactly rather than assumed, in
   case that restriction ever loosens. Never zero: a port pjsua
   handed back always has a real audio format. */
static unsigned pjsua_pos_unit_bytes(const pjmedia_port *port) {
    unsigned bits = PJMEDIA_PIA_BITS(&port->info);
    unsigned b = bits / 8;
    return b ? b : 2;
}

/* Runs on a pjmedia thread when the WAV file reaches its end. Set the
   flag and return -- nothing else. Destroying the player from inside
   its own callback, or blocking here on core->lock while some other
   thread holds it and calls into pjmedia, is how this deadlocks.
   sp_playback_get reaps the flag and tears the player down on the
   caller's own thread. OggOpus has no equivalent callback; its eof is
   polled instead, from sp_playback_get itself, using the same flag. */
static void playback_eof_cb(pjmedia_port *port, void *user_data) {
    (void)port;
    sp_playback_state *ps = user_data;
    if (ps)
        atomic_store(&ps->eof, true);
}

/* Called with core->lock already released: pjsua_player_destroy and
   pjsua_conf_remove_port may take pjsua's own internal lock, and
   holding ours across that risks the same kind of inversion the eof
   callback avoids.

   Order matters beyond that, now that a speed wrapper (speedport.h)
   sits between the source and the bridge: the wrapper is removed from
   the bridge and destroyed FIRST, in every case, before the source it
   pulls frames from is touched at all. get_frame() on a still-live
   wrapper calls straight into the source; destroying the source first
   would leave that call running against freed memory. This mirrors
   call.c's recorder_stop ordering (remove from the bridge, then
   destroy, then release the pool that add_port needed kept alive) --
   applied here to the wrapper's own slot/pool rather than the
   source's, since the source itself is no longer in the bridge at
   all (see sp_playback_start). pjmedia_conf_add_port installs a group
   lock on speed_port, so this pjmedia_port_destroy call may only drop
   a reference -- on_destroy can run later, on the bridge's own
   thread, rather than synchronously here (see oggopus_write.c's
   writer_on_destroy comment for the same point). That is still safe:
   speedport.c's on_destroy touches only the wrapper's own pool, never
   the source, so by the time the source is destroyed below there is
   nothing left for the two to race on. */
static void destroy_loaded(sp_playback_kind kind, pjsua_player_id id,
                           pjmedia_port *ogg_port, pj_pool_t *ogg_pool,
                           pjmedia_port *speed_port, pj_pool_t *speed_pool,
                           pjsua_conf_port_id speed_slot) {
    if (speed_port) {
        pjsua_conf_remove_port(speed_slot);
        pjmedia_port_destroy(speed_port);
    }
    if (speed_pool)
        pj_pool_release(speed_pool);

    if (kind == SP_PLAYBACK_WAV) {
        if (id != PJSUA_INVALID_ID)
            pjsua_player_destroy(id);
    } else if (kind == SP_PLAYBACK_OGGOPUS) {
        if (ogg_port) {
            /* Not pjsua_conf_remove_port here: ogg_port itself is
               never added to the bridge any more -- only speed_port,
               removed above, is. */
            pjmedia_port_destroy(ogg_port);
            pj_pool_release(ogg_pool);
        }
    }
}

/* Detection is by contents, not extension: an extension check would
   accept a mislabelled file and fail confusingly deeper in, in
   whichever of pjsua_player_create or sp_oggopus_reader_create it was
   wrongly routed to. "OggS" is Ogg's own page magic (RFC 3533 S6);
   "RIFF" is WAV's. Neither is read past the four bytes needed to
   tell them apart -- the real parse happens in whichever path is
   chosen next, where a truncated or otherwise malformed file of the
   right family is still reported as such rather than as "unknown". */
typedef enum { SP_PB_FMT_WAV, SP_PB_FMT_OGGOPUS, SP_PB_FMT_UNKNOWN } sp_pb_fmt;

static sp_pb_fmt detect_format(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return SP_PB_FMT_UNKNOWN;
    unsigned char magic[4] = {0};
    size_t got = fread(magic, 1, sizeof magic, f);
    fclose(f);
    if (got == sizeof magic && memcmp(magic, "OggS", 4) == 0)
        return SP_PB_FMT_OGGOPUS;
    if (got == sizeof magic && memcmp(magic, "RIFF", 4) == 0)
        return SP_PB_FMT_WAV;
    return SP_PB_FMT_UNKNOWN;
}

sp_status sp_playback_start(sp_core *core, const char *path) {
    SP_ENTRY(core);

    /* Refused before anything else is even looked at: the safety rule
       this feature exists for. An operator who gets told after
       hearing a click has been told twice. */
    size_t n_calls = 0;
    sp_call_list(core, NULL, 0, &n_calls);
    if (n_calls > 0) {
        sp_core_detail(core,
            "a call is active; playing a recording now would reach the "
            "far end through the microphone");
        return SP_ERR_STATE;
    }

    if (!path || !*path)
        return SP_ERR_INVALID_ARG;
    if (strlen(path) >= sizeof core->playback.path) {
        sp_core_detail(core, "path is too long to play: %s", path);
        return SP_ERR_INVALID_ARG;
    }

    if (!core->pj_ready) {
        sp_core_detail(core, "the core is not running");
        return SP_ERR_STATE;
    }

    /* R_OK, not F_OK: F_OK only asks whether the path exists, and
       never fails with EACCES for an unreadable-but-present file --
       the distinction the caller needs below would never trigger. */
    if (access(path, R_OK) != 0) {
        /* ENOENT is genuinely missing; anything else (typically
           EACCES) is a permissions or disk problem on a path that is
           otherwise fine -- distinct per SP_ERR_IO's own doc comment
           in comlink/core.h, and a different afternoon for whoever
           has to act on it than a renamed or deleted file. */
        int err = errno;
        sp_core_detail(core, "%s: %s", path, strerror(err));
        return err == ENOENT ? SP_ERR_NOT_FOUND : SP_ERR_IO;
    }

    sp_pb_fmt fmt = detect_format(path);
    if (fmt == SP_PB_FMT_UNKNOWN) {
        sp_core_detail(core,
            "%s: not a recognised recording (starts with neither an Ogg "
            "nor a WAV header)", path);
        return SP_ERR_MEDIA;
    }

    sp_playback_kind new_kind = SP_PLAYBACK_NONE;
    pjsua_player_id new_id = PJSUA_INVALID_ID;
    pjmedia_port *new_ogg_port = NULL;
    pj_pool_t *new_ogg_pool = NULL;
    uint32_t new_ogg_duration_ms = 0;
    pjmedia_port *new_speed_port = NULL;
    pj_pool_t *new_speed_pool = NULL;
    pjsua_conf_port_id new_speed_slot = PJSUA_INVALID_ID;

    if (fmt == SP_PB_FMT_WAV) {
        pj_str_t f = pj_str((char *)path);
        /* Once, not looped: pjsua_player_create loops by default, and
           a recording that looped back to the start on its own would
           make both the eof callback and "stopped at the end"
           meaningless. */
        pj_status_t st = pjsua_player_create(&f, PJMEDIA_FILE_NO_LOOP, &new_id);
        if (st != PJ_SUCCESS) {
            char pjmsg[PJ_ERR_MSG_SIZE] = {0};
            pj_strerror(st, pjmsg, sizeof pjmsg);
            sp_core_detail(core,
                "cannot play %s: this build only reads WAV and OggOpus "
                "files (%s)", path, pjmsg);
            return SP_ERR_MEDIA;
        }

        /* The player's own conf slot (registered internally by
           pjsua_player_create) is deliberately left unconnected: the
           speed wrapper below pulls from its raw port directly, and
           chaining the wrapper's own slot to slot 0 is what carries
           that audio onward instead. An unconnected slot is never
           pulled by the bridge -- see sp_playback_pause's own comment
           on that -- so leaving it be is harmless. */
        pjmedia_port *port = NULL;
        if (pjsua_player_get_port(new_id, &port) != PJ_SUCCESS || !port) {
            pjsua_player_destroy(new_id);
            sp_core_detail(core, "cannot get the player's own port for %s",
                          path);
            return SP_ERR_MEDIA;
        }
        pjmedia_wav_player_set_eof_cb2(port, &core->playback, playback_eof_cb);

        pj_pool_t *pool = pjsua_pool_create("sp_playback_speed", 256, 256);
        if (!pool) {
            pjsua_player_destroy(new_id);
            return SP_ERR_INTERNAL;
        }
        pjmedia_port *speed_port = NULL;
        sp_status sst = sp_speedport_create(
            pool, port, PJMEDIA_PIA_SRATE(&port->info),
            PJMEDIA_PIA_CCNT(&port->info), PJMEDIA_PIA_SPF(&port->info),
            &speed_port);
        if (sst != SP_OK) {
            pj_pool_release(pool);
            pjsua_player_destroy(new_id);
            sp_core_detail(core, "cannot play %s: %s", path,
                          sp_strerror(sst));
            return SP_ERR_MEDIA;
        }
        pjsua_conf_port_id slot = PJSUA_INVALID_ID;
        if (pjsua_conf_add_port(pool, speed_port, &slot) != PJ_SUCCESS) {
            pjmedia_port_destroy(speed_port);
            pj_pool_release(pool);
            pjsua_player_destroy(new_id);
            sp_core_detail(core, "cannot add %s to the conference bridge",
                          path);
            return SP_ERR_MEDIA;
        }
        if (pjsua_conf_connect(slot, 0) != PJ_SUCCESS) {
            pjsua_conf_remove_port(slot);
            pjmedia_port_destroy(speed_port);
            pj_pool_release(pool);
            pjsua_player_destroy(new_id);
            sp_core_detail(core, "cannot connect %s to the sound device",
                          path);
            return SP_ERR_MEDIA;
        }

        new_kind = SP_PLAYBACK_WAV;
        new_speed_port = speed_port;
        new_speed_pool = pool;
        new_speed_slot = slot;
    } else {
        pj_pool_t *pool = pjsua_pool_create("sp_playback_opus", 512, 512);
        if (!pool)
            return SP_ERR_INTERNAL;

        pjmedia_port *port = NULL;
        sp_status ost = sp_oggopus_reader_create(
            pool, path, SP_PB_BRIDGE_CLOCK_RATE, SP_PB_BRIDGE_CHANNELS,
            SP_PB_BRIDGE_SAMPLES_PER_FRAME, &port);
        if (ost != SP_OK) {
            pj_pool_release(pool);
            sp_core_detail(core, "cannot play %s: %s", path,
                          sp_strerror(ost));
            return SP_ERR_MEDIA;
        }

        /* The raw reader is never itself added to the bridge -- the
           speed wrapper below is, and pulls from this port directly.
           The wrapper gets its own separate pool for its bridge slot
           (speed_pool, parallel to ogg_pool for the reader) rather
           than sharing `pool`: destroy_loaded releases the two
           independently, in a fixed order, and a shared pool would
           make that a double release instead. */
        pjmedia_port *speed_port = NULL;
        sp_status sst = sp_speedport_create(
            pool, port, SP_PB_BRIDGE_CLOCK_RATE, SP_PB_BRIDGE_CHANNELS,
            SP_PB_BRIDGE_SAMPLES_PER_FRAME, &speed_port);
        if (sst != SP_OK) {
            pjmedia_port_destroy(port);
            pj_pool_release(pool);
            sp_core_detail(core, "cannot play %s: %s", path,
                          sp_strerror(sst));
            return SP_ERR_MEDIA;
        }

        pj_pool_t *speed_pool = pjsua_pool_create("sp_playback_speed", 256, 256);
        if (!speed_pool) {
            pjmedia_port_destroy(speed_port);
            pjmedia_port_destroy(port);
            pj_pool_release(pool);
            return SP_ERR_INTERNAL;
        }
        pjsua_conf_port_id slot = PJSUA_INVALID_ID;
        if (pjsua_conf_add_port(speed_pool, speed_port, &slot) != PJ_SUCCESS) {
            pj_pool_release(speed_pool);
            pjmedia_port_destroy(speed_port);
            pjmedia_port_destroy(port);
            pj_pool_release(pool);
            sp_core_detail(core, "cannot add %s to the conference bridge",
                          path);
            return SP_ERR_MEDIA;
        }
        if (pjsua_conf_connect(slot, 0) != PJ_SUCCESS) {
            pjsua_conf_remove_port(slot);
            pj_pool_release(speed_pool);
            pjmedia_port_destroy(speed_port);
            pjmedia_port_destroy(port);
            pj_pool_release(pool);
            sp_core_detail(core, "cannot connect %s to the sound device",
                          path);
            return SP_ERR_MEDIA;
        }

        /* Best-effort: a duration that fails to read still leaves a
           playable recording, just one whose position bar cannot show
           a total. sp_playback_start does not fail on it -- only on
           the reader itself refusing the file above. */
        uint32_t dur = 0;
        sp_oggopus_duration_ms(path, &dur);

        new_kind = SP_PLAYBACK_OGGOPUS;
        new_ogg_port = port;
        new_ogg_pool = pool;
        new_ogg_duration_ms = dur;
        new_speed_port = speed_port;
        new_speed_pool = speed_pool;
        new_speed_slot = slot;
    }

    /* Only now, with the new player up and connected, does the old
       one -- if any -- go. A start that fails above must leave
       whatever was already playing untouched. */
    pthread_mutex_lock(&core->lock);
    sp_playback_kind old_kind = core->playback.kind;
    pjsua_player_id old_id = core->playback.id;
    pjmedia_port *old_ogg_port = core->playback.ogg_port;
    pj_pool_t *old_ogg_pool = core->playback.ogg_pool;
    pjmedia_port *old_speed_port = core->playback.speed_port;
    pj_pool_t *old_speed_pool = core->playback.speed_pool;
    pjsua_conf_port_id old_speed_slot = core->playback.speed_slot;

    core->playback.kind = new_kind;
    core->playback.id = new_id;
    core->playback.ogg_port = new_ogg_port;
    core->playback.ogg_pool = new_ogg_pool;
    core->playback.ogg_duration_ms = new_ogg_duration_ms;
    core->playback.speed_port = new_speed_port;
    core->playback.speed_pool = new_speed_pool;
    core->playback.speed_slot = new_speed_slot;
    core->playback.speed = 1.0f;
    snprintf(core->playback.path, sizeof core->playback.path, "%s", path);
    core->playback.paused = false;
    atomic_store(&core->playback.eof, false);
    pthread_mutex_unlock(&core->lock);

    destroy_loaded(old_kind, old_id, old_ogg_port, old_ogg_pool,
                   old_speed_port, old_speed_pool, old_speed_slot);

    return SP_OK;
}

sp_status sp_playback_pause(sp_core *core) {
    SP_ENTRY(core);
    pthread_mutex_lock(&core->lock);
    if (core->playback.kind == SP_PLAYBACK_NONE) {
        pthread_mutex_unlock(&core->lock);
        sp_core_detail(core, "nothing is loaded to pause");
        return SP_ERR_STATE;
    }
    bool already = core->playback.paused;
    pjsua_conf_port_id speed_slot = core->playback.speed_slot;
    pthread_mutex_unlock(&core->lock);

    if (already)
        return SP_OK;

    /* Disconnecting from slot 0 is what holds the position: pjmedia's
       conference bridge only calls get_frame() on a port that has a
       listener, so a disconnected port's file position simply stops
       moving rather than needing to be saved and restored. True for
       both kinds -- the WAV port and our own OggOpus reader alike --
       and, since the speed wrapper (speedport.h) went in, it is the
       wrapper's own slot that is disconnected/reconnected here, not
       the source's: the source is no longer connected to anything by
       itself (see sp_playback_start), and disconnecting the wrapper
       stops the bridge from calling its get_frame(), which is what
       stops it pulling from the source in turn -- position freezes on
       both sides of the wrapper together, not just one. */
    pjsua_conf_disconnect(speed_slot, 0);

    pthread_mutex_lock(&core->lock);
    core->playback.paused = true;
    pthread_mutex_unlock(&core->lock);
    return SP_OK;
}

sp_status sp_playback_resume(sp_core *core) {
    SP_ENTRY(core);
    pthread_mutex_lock(&core->lock);
    if (core->playback.kind == SP_PLAYBACK_NONE) {
        pthread_mutex_unlock(&core->lock);
        sp_core_detail(core, "nothing is loaded to resume");
        return SP_ERR_STATE;
    }
    bool was_paused = core->playback.paused;
    pjsua_conf_port_id speed_slot = core->playback.speed_slot;
    pthread_mutex_unlock(&core->lock);

    if (!was_paused)
        return SP_OK;

    if (pjsua_conf_connect(speed_slot, 0) != PJ_SUCCESS) {
        sp_core_detail(core, "could not resume playback");
        return SP_ERR_MEDIA;
    }

    pthread_mutex_lock(&core->lock);
    core->playback.paused = false;
    pthread_mutex_unlock(&core->lock);
    return SP_OK;
}

sp_status sp_playback_stop(sp_core *core) {
    SP_ENTRY(core);
    pthread_mutex_lock(&core->lock);
    sp_playback_kind kind = core->playback.kind;
    pjsua_player_id id = core->playback.id;
    pjmedia_port *ogg_port = core->playback.ogg_port;
    pj_pool_t *ogg_pool = core->playback.ogg_pool;
    pjmedia_port *speed_port = core->playback.speed_port;
    pj_pool_t *speed_pool = core->playback.speed_pool;
    pjsua_conf_port_id speed_slot = core->playback.speed_slot;

    core->playback.kind = SP_PLAYBACK_NONE;
    core->playback.id = PJSUA_INVALID_ID;
    core->playback.ogg_port = NULL;
    core->playback.ogg_pool = NULL;
    core->playback.ogg_duration_ms = 0;
    core->playback.speed_port = NULL;
    core->playback.speed_pool = NULL;
    core->playback.speed_slot = PJSUA_INVALID_ID;
    core->playback.speed = 1.0f;
    core->playback.path[0] = '\0';
    core->playback.paused = false;
    atomic_store(&core->playback.eof, false);
    pthread_mutex_unlock(&core->lock);

    destroy_loaded(kind, id, ogg_port, ogg_pool, speed_port, speed_pool,
                   speed_slot);
    return SP_OK;
}

/* Time-stretching without a pitch shift: see comlink/core.h's own
   comment for the contract, and speedport.h for why a wrapper port is
   what implements it. */
sp_status sp_playback_set_speed(sp_core *core, float speed) {
    SP_ENTRY(core);
    if (speed != 0.5f && speed != 1.0f && speed != 2.0f)
        return SP_ERR_INVALID_ARG;

    pthread_mutex_lock(&core->lock);
    if (core->playback.kind == SP_PLAYBACK_NONE) {
        pthread_mutex_unlock(&core->lock);
        sp_core_detail(core, "nothing is loaded to change the speed of");
        return SP_ERR_STATE;
    }
    pjmedia_port *speed_port = core->playback.speed_port;
    core->playback.speed = speed;
    pthread_mutex_unlock(&core->lock);

    /* sp_speedport_set_speed applies this on the media thread, at the
       top of its next get_frame() call -- not here, and not under
       core->lock, on the same rule sp_playback_pause/resume follow
       for pjsua_conf_connect/disconnect: no call into pjmedia while
       holding a core lock a callback on another thread might also be
       waiting on. */
    if (speed_port)
        sp_speedport_set_speed(speed_port, speed);
    return SP_OK;
}

sp_status sp_playback_seek(sp_core *core, uint32_t position_ms) {
    SP_ENTRY(core);
    pthread_mutex_lock(&core->lock);
    if (core->playback.kind == SP_PLAYBACK_NONE) {
        pthread_mutex_unlock(&core->lock);
        sp_core_detail(core, "nothing is loaded to seek");
        return SP_ERR_STATE;
    }
    sp_playback_kind kind = core->playback.kind;
    pjsua_player_id id = core->playback.id;
    pjmedia_port *speed_port = core->playback.speed_port;
    pthread_mutex_unlock(&core->lock);

    if (kind == SP_PLAYBACK_OGGOPUS) {
        /* Sample-accurate seeking inside compressed Opus needs a
           page-level search for the target granule and a decoder
           reset at that point -- real work, deliberately left undone
           here rather than rushed: an operator scrubbing to a wrong
           position on a 911 recording is worse than a control that
           plainly does not move yet. Play/pause/stop and the position
           display are unaffected. */
        sp_core_detail(core,
            "seeking is not yet supported for Opus recordings");
        return SP_ERR_MEDIA;
    }

    pjmedia_port *port = NULL;
    if (pjsua_player_get_port(id, &port) != PJ_SUCCESS || !port)
        return sp_core_fail(core, PJ_EUNKNOWN, SP_ERR_MEDIA,
                            "pjsua_player_get_port");

    /* Both the clamp and the position come from a 64-bit byte count:
       milliseconds times bytes-per-second overflows a 32-bit int on a
       file of only a couple of minutes at 16-bit/16kHz. */
    uint64_t bps = bytes_per_second(port);
    unsigned unit = pjsua_pos_unit_bytes(port);
    pj_ssize_t len = pjmedia_wav_player_get_len(port);
    uint64_t len_bytes = len > 0 ? (uint64_t)len : 0;

    /* pjmedia_wav_player_port_set_pos asserts the byte offset is
       strictly LESS than the payload length, not <=: a seek clamped
       to exactly the end aborts the process rather than returning an
       error. The last whole sample unit is as far as a seek can go. */
    uint64_t total_units = len_bytes / unit;
    uint64_t max_units = total_units > 0 ? total_units - 1 : 0;

    uint64_t want_bytes = bps ? ((uint64_t)position_ms * bps) / 1000ULL : 0;
    uint64_t samples = want_bytes / unit;
    if (samples > max_units)
        samples = max_units;

    /* pjsua_player_set_pos takes SAMPLES, not bytes and not
       milliseconds: want_bytes was divided by the unit pjsua itself
       uses (see pjsua_pos_unit_bytes) to get there. */
    pj_status_t st = pjsua_player_set_pos(id, (pj_uint32_t)samples);
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_MEDIA, "pjsua_player_set_pos");

    /* The seek above moves the SOURCE's own read position directly;
       the speed wrapper sitting downstream of it (speedport.h) has no
       way to know that happened on its own. Without this, whatever
       WSOLA had queued or synthesised for the position the source was
       AT before the seek still plays out first -- a stale or
       discontinuous few tens of milliseconds spliced onto the start
       of the new position, worse the faster or slower playback is
       running. Unconditional and harmless at 1.0x: see
       sp_speedport_reset's own comment. */
    if (speed_port)
        sp_speedport_reset(speed_port);
    return SP_OK;
}

sp_status sp_playback_get(sp_core *core, sp_playback *out) {
    SP_ENTRY(core);
    if (!out)
        return SP_ERR_INVALID_ARG;

    /* OggOpus has no eof callback of its own (see playback_eof_cb's
       comment): reaped here into the same flag the WAV path's
       callback sets, so the check just below treats both kinds
       identically. Read without core->lock, same as the position and
       duration queries further down -- sp_oggopus_reader_is_eof is
       backed by an atomic for exactly this. */
    pthread_mutex_lock(&core->lock);
    sp_playback_kind peek_kind = core->playback.kind;
    pjmedia_port *peek_ogg_port = core->playback.ogg_port;
    pthread_mutex_unlock(&core->lock);
    if (peek_kind == SP_PLAYBACK_OGGOPUS && peek_ogg_port) {
        pj_bool_t oeof = PJ_FALSE;
        if (sp_oggopus_reader_is_eof(peek_ogg_port, &oeof) == SP_OK && oeof)
            atomic_store(&core->playback.eof, true);
    }

    /* Reaped here, on the caller's thread, rather than from inside
       the eof callback itself. */
    if (atomic_load(&core->playback.eof))
        sp_playback_stop(core);

    pthread_mutex_lock(&core->lock);
    if (core->playback.kind == SP_PLAYBACK_NONE) {
        pthread_mutex_unlock(&core->lock);
        out->state = SP_PLAY_STOPPED;
        out->position_ms = 0;
        out->duration_ms = 0;
        out->path = NULL;
        out->seekable = false;
        out->speed = 1.0f;
        return SP_OK;
    }
    sp_playback_kind kind = core->playback.kind;
    pjsua_player_id id = core->playback.id;
    pjmedia_port *ogg_port = core->playback.ogg_port;
    uint32_t ogg_duration_ms = core->playback.ogg_duration_ms;
    bool paused = core->playback.paused;
    float speed = core->playback.speed;
    out->path = core->playback.path;
    pthread_mutex_unlock(&core->lock);

    /* Queried without core->lock held: pjsua has its own internal
       lock, and calling into it while holding ours risks the two
       inverting against the pjmedia callback thread.

       Both queries below read the SOURCE's own position/length --
       the WAV port pjsua_player_get_port hands back, or the raw
       OggOpus reader's own sample counter -- never the speed
       wrapper's. The wrapper has no position of its own to report: it
       only reshapes how fast the source's audio is handed to the
       bridge, not the source's own byte/sample offset, so duration_ms
       and position_ms come out identical at every speed -- a 60-
       second recording is duration_ms 60000 whether played at 1x or
       2x. See test_playback.c's speed_two_still_reports_the_same_
       duration test. */
    uint64_t position_ms = 0, duration_ms = 0;
    if (kind == SP_PLAYBACK_WAV) {
        pjmedia_port *port = NULL;
        if (pjsua_player_get_port(id, &port) == PJ_SUCCESS && port) {
            uint64_t bps = bytes_per_second(port);
            pj_ssize_t len = pjmedia_wav_player_get_len(port);
            pj_ssize_t pos = pjmedia_wav_player_port_get_pos(port);
            uint64_t len_bytes = len > 0 ? (uint64_t)len : 0;
            uint64_t pos_bytes = pos > 0 ? (uint64_t)pos : 0;
            if (bps) {
                duration_ms = (len_bytes * 1000ULL) / bps;
                position_ms = (pos_bytes * 1000ULL) / bps;
            }
        }
    } else {
        duration_ms = ogg_duration_ms;
        uint32_t pos = 0;
        if (ogg_port && sp_oggopus_reader_get_pos_ms(ogg_port, &pos) == SP_OK)
            position_ms = pos;
    }

    out->state = paused ? SP_PLAY_PAUSED : SP_PLAY_PLAYING;
    out->position_ms = (uint32_t)position_ms;
    out->duration_ms = (uint32_t)duration_ms;
    /* Matches sp_playback_seek's own refusal: WAV seeks, OggOpus does
       not yet. Decided by kind, never by the path's extension. */
    out->seekable = (kind == SP_PLAYBACK_WAV);
    out->speed = speed;
    return SP_OK;
}
