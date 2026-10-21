/* See speedport.h for the shape of the problem. This file is the
   get_frame() state machine for each of the three speeds:

   - 1.0: pjmedia_port_get_frame(source, frame) directly. No WSOLA
     call at all -- not even pjmedia_wsola_save -- so the common case
     (most playback is at normal speed) pays nothing for a feature
     nobody asked for, and cannot itself introduce an artefact.

   - 2.0: pull a source frame, pjmedia_wsola_save it, and once two or
     more frames are queued, pjmedia_wsola_discard roughly half of
     them back out. erase_cnt is in/out -- WSOLA reports what it
     actually managed, which this port's fixed frame size makes more
     likely to be MORE than asked than less (see get_frame_double's
     own comment on why). Rather than trust any one call's own result,
     a running credit tracks the cumulative discarded-vs-pulled
     balance and only attempts a discard while the stream is behind on
     the 50% target overall, so a run of over-shoots earns a run of
     skips afterward instead of compounding into emitting far fewer
     frames than 2x should.

   - 0.5: pull one source frame, save it, emit it as-is, and on the
     next call emit pjmedia_wsola_generate's synthetic frame instead
     of pulling again -- alternating real/generated frames one for
     one, which is what makes half speed take exactly twice as many
     output frames to consume the same source.

   All three stop the instant the source returns
   PJMEDIA_FRAME_TYPE_NONE: `ended` latches permanently once seen (a
   speed change afterward does not revive it), and any already-queued
   backlog still drains out normally before the wrapper itself starts
   reporting PJMEDIA_FRAME_TYPE_NONE -- end of file must still end,
   not run forever regenerating from the last real audio. */
#include "speedport.h"

#include <pjmedia/errno.h>
#include <pjmedia/signatures.h>
#include <pjmedia/wsola.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* How many source frames' worth of scratch space get_frame_double may
   need queued at once. Sized generously -- several times the
   steady-state need of about two frames -- as a margin against a run
   of skipped discards (see discard_credit) or of short discards
   letting the backlog build up further than usual before it next gets
   trimmed; combine_append's own defensive drop (see below) is the
   backstop if even that margin is exceeded. */
#define SP_SPEEDPORT_COMBINE_FRAMES 8u

/* Caps how many source frames get_frame_double will pull within a
   single call -- see its own comment on discard_credit for why a
   run of pulls can happen at all. Bounds a single get_frame() call to
   a small, constant amount of work regardless of what WSOLA's search
   does or how long a discard-skip streak runs; a call that is still
   short of a full output frame after the cap falls through to
   pjmedia_wsola_generate instead of pulling further. */
#define SP_SPEEDPORT_MAX_PULL_ROUNDS 6u

typedef enum {
    SP_SPEED_HALF,
    SP_SPEED_ONE,
    SP_SPEED_DOUBLE
} sp_speed_mode;

static sp_speed_mode mode_of(float speed) {
    if (speed == 0.5f)
        return SP_SPEED_HALF;
    if (speed == 2.0f)
        return SP_SPEED_DOUBLE;
    /* Includes 1.0 and, defensively, anything sp_playback_set_speed
       should already have refused -- see speedport.h. */
    return SP_SPEED_ONE;
}

typedef struct sp_speedport {
    pjmedia_port base;
    pj_pool_t *pool;      /* this wrapper's own; released in on_destroy */
    pjmedia_port *source; /* never destroyed here -- see speedport.h */

    unsigned channels;
    unsigned spf;           /* samples per frame, per channel */
    unsigned frame_samples; /* spf * channels -- one frame, interleaved */

    pjmedia_wsola *wsola;

    /* Requested from any thread via sp_speedport_set_speed; applied
       at the top of the next get_frame() call, which is the only
       place `mode`, `active_speed` and everything below are ever
       touched -- so those need no lock, only this one atomic word
       crosses threads. */
    _Atomic float requested_speed;
    float active_speed;
    sp_speed_mode mode;

    /* Set by sp_speedport_reset, from any thread; reaped and cleared
       at the top of the next get_frame() call, same rule as
       requested_speed above. Independent of requested_speed/
       active_speed: a reset can land with the speed unchanged (the
       seek case) as well as alongside a speed change. */
    _Atomic pj_bool_t reset_pending;

    pj_int16_t *tmp;        /* one pulled source frame, frame_samples long */
    pj_int16_t *combine;    /* SP_SPEEDPORT_COMBINE_FRAMES * frame_samples */
    unsigned combine_cap;   /* capacity of combine, in samples */
    unsigned combine_fill;  /* valid samples currently in combine, from [0) */

    /* 2x only: running balance of "samples actually discarded, times
       two" minus "samples pulled", i.e. twice how far ahead (positive)
       or behind (negative) of the 50%-discarded target the stream is
       so far. See get_frame_double's own comment on why a discard is
       attempted only while this is negative. */
    pj_int64_t discard_credit;
    /* How many times WSOLA declined a discard. Reported once,
       then counted: a guard that no-ops without saying so is
       indistinguishable from one that never fires. */
    unsigned   discard_failures;

    pj_bool_t half_pending_generate; /* 0.5x: next call synthesises */
    pj_bool_t ended; /* source has returned PJMEDIA_FRAME_TYPE_NONE */
} sp_speedport;

/* Pulls exactly one source frame into `dst` (frame_samples samples).
   *ended is set when the source is exhausted (PJMEDIA_FRAME_TYPE_NONE
   or an outright error, treated the same way sp_oggopus_reader_create
   and the WAV path already treat "nothing more to play"); `dst` is
   zeroed in that case so a caller that copies it anyway never reads
   uninitialised pool memory. */
static void pull_source_frame(sp_speedport *sp, pj_int16_t *dst,
                              pj_bool_t *ended) {
    pjmedia_frame f;
    f.type = PJMEDIA_FRAME_TYPE_AUDIO;
    f.buf = dst;
    f.size = (pj_size_t)sp->frame_samples * sizeof(pj_int16_t);
    f.timestamp.u64 = 0;
    f.bit_info = 0;

    pj_status_t st = pjmedia_port_get_frame(sp->source, &f);
    if (st != PJ_SUCCESS || f.type != PJMEDIA_FRAME_TYPE_AUDIO) {
        memset(dst, 0, (size_t)sp->frame_samples * sizeof(pj_int16_t));
        *ended = PJ_TRUE;
    } else {
        *ended = PJ_FALSE;
    }
}

/* Appends one frame to the combine buffer, dropping the oldest frame
   first if it would not otherwise fit. That drop is a last resort,
   never expected to trigger at this codebase's frame size (see
   SP_SPEEDPORT_COMBINE_FRAMES's own comment) -- it exists so a WSOLA
   that somehow never manages to erase anything loses at most a frame
   of audio instead of writing past combine's pool-allocated length. */
static void combine_append(sp_speedport *sp, const pj_int16_t *src) {
    unsigned fs = sp->frame_samples;
    if (sp->combine_fill + fs > sp->combine_cap) {
        memmove(sp->combine, sp->combine + fs,
               (size_t)(sp->combine_fill - fs) * sizeof(pj_int16_t));
        sp->combine_fill -= fs;
    }
    memcpy(sp->combine + sp->combine_fill, src,
          (size_t)fs * sizeof(pj_int16_t));
    sp->combine_fill += fs;
}

/* Shared by a speed switch and an explicit sp_speedport_reset: drops
   whatever either speed's own state machine had queued rather than
   trying to carry it over. The discarded backlog is at most a couple
   of frames of audio (a few tens of milliseconds), and playing that
   out -- at the OLD speed's compression while a NEW speed's WSOLA
   history starts cold, or from the position the source was AT before
   a seek moved it elsewhere -- is exactly the spliced-together click
   or stale audio this reset is here to avoid.

   `ended` is deliberately NOT touched here: once the source is
   exhausted it stays exhausted no matter what the speed knob does or
   how many times a reset lands -- there is nothing left to switch to
   or seek back into from this wrapper's point of view. A seek that
   lands the source somewhere before its own end is the frontend's job
   to notice (sp_playback_get's own eof flag), not this wrapper's. */
static void do_reset(sp_speedport *sp) {
    pjmedia_wsola_reset(sp->wsola, 0);
    sp->combine_fill = 0;
    sp->discard_credit = 0;
    sp->half_pending_generate = PJ_FALSE;
}

static void apply_pending_speed(sp_speedport *sp) {
    float req = atomic_load(&sp->requested_speed);
    if (req == sp->active_speed)
        return;
    sp->active_speed = req;
    sp->mode = mode_of(req);
    do_reset(sp);
}

static void apply_pending_reset(sp_speedport *sp) {
    if (atomic_exchange(&sp->reset_pending, PJ_FALSE))
        do_reset(sp);
}

static pj_status_t get_frame_one(sp_speedport *sp, pjmedia_frame *frame) {
    return pjmedia_port_get_frame(sp->source, frame);
}

static pj_status_t get_frame_half(sp_speedport *sp, pjmedia_frame *frame) {
    pj_int16_t *dst = (pj_int16_t *)frame->buf;

    if (sp->half_pending_generate) {
        pjmedia_wsola_generate(sp->wsola, dst);
        sp->half_pending_generate = PJ_FALSE;
        frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
        return PJ_SUCCESS;
    }

    if (sp->ended) {
        frame->type = PJMEDIA_FRAME_TYPE_NONE;
        return PJ_SUCCESS;
    }

    pj_bool_t src_ended = PJ_FALSE;
    pull_source_frame(sp, dst, &src_ended);
    if (src_ended) {
        sp->ended = PJ_TRUE;
        frame->type = PJMEDIA_FRAME_TYPE_NONE;
        return PJ_SUCCESS;
    }

    /* Never a loss-concealment save (prev_lost = PJ_FALSE): the
       previous generated frame was deliberate stretching, not a
       dropped packet, so WSOLA should just append this one rather
       than blend it in as a recovery. */
    pjmedia_wsola_save(sp->wsola, dst, PJ_FALSE);
    sp->half_pending_generate = PJ_TRUE;
    frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
    return PJ_SUCCESS;
}

static pj_status_t get_frame_double(sp_speedport *sp, pjmedia_frame *frame) {
    unsigned fs = sp->frame_samples;
    pj_int16_t *dst = (pj_int16_t *)frame->buf;

    /* Pull source frames and compress them until at least one full
       output frame is sitting in combine, or the source runs out.

       erase_cnt is in/out, and WSOLA reports what it actually
       managed -- which this codebase's own fixed frame size makes
       more likely to be MORE than asked than less: its pitch-matched
       search window is sized off frame_samples, not off the request,
       so a single discard call can remove well over what was asked.
       Treating each call's own request as the unit to get right would
       mean a run of over-shoots compounds into emitting far fewer
       frames than 2x should (each one quietly doing more than 2x)
       with no way to notice. discard_credit tracks the CUMULATIVE
       balance instead -- twice the total ever actually discarded
       minus the total ever pulled -- and a discard is only attempted
       while that balance says the stream is behind on the 50% target,
       skipped otherwise. A streak of over-shoots earns a streak of
       skips immediately after, which is what keeps the running total
       converging on 2x rather than drifting away from it over a long
       recording. The credit's positive side is clamped so one large
       early over-shoot cannot bank enough "ahead" credit to disable
       compression for the rest of the file. */
    unsigned rounds = 0;
    while (!sp->ended && rounds < SP_SPEEDPORT_MAX_PULL_ROUNDS &&
          (sp->combine_fill < fs || sp->discard_credit < 0)) {
        rounds++;
        pj_bool_t src_ended = PJ_FALSE;
        pull_source_frame(sp, sp->tmp, &src_ended);
        if (src_ended) {
            sp->ended = PJ_TRUE;
            break;
        }
        pjmedia_wsola_save(sp->wsola, sp->tmp, PJ_FALSE);
        combine_append(sp, sp->tmp);
        sp->discard_credit -= (pj_int64_t)fs;
        /* Floored as well as capped, and the two bounds are the same
           size on purpose.
         *
           The cap stops an early over-shoot switching compression off
           for the rest of the file. The floor stops the opposite: if
           pjmedia_wsola_discard keeps failing or under-shooting, the
           credit would otherwise run negative without limit, and the
           only thing left to relieve the pressure is combine_append's
           defensive drop of the oldest frame -- which throws away
           speech rather than slowing down.
         *
           Bounded, the same failure degrades toward 1x playback
           instead. A recording that plays slower than asked is
           recoverable; one with words missing is not, and nothing
           downstream could tell the difference. WSOLA on this
           codebase has only ever been observed to over-shoot, so this
           is the branch nobody has seen -- which is the argument for
           bounding it rather than against. */
        if (sp->discard_credit < -2 * (pj_int64_t)fs)
            sp->discard_credit = -2 * (pj_int64_t)fs;

        if (sp->combine_fill > fs && sp->discard_credit < 0) {
            unsigned erase_cnt = fs;
            pj_status_t st = pjmedia_wsola_discard(
                sp->wsola, sp->combine, sp->combine_fill, NULL, 0, &erase_cnt);
            if (st == PJ_SUCCESS && erase_cnt > 0 &&
               erase_cnt <= sp->combine_fill) {
                memmove(sp->combine, sp->combine + erase_cnt,
                       (size_t)(sp->combine_fill - erase_cnt) *
                           sizeof(pj_int16_t));
                sp->combine_fill -= erase_cnt;
                sp->discard_credit += 2 * (pj_int64_t)erase_cnt;
                if (sp->discard_credit > 2 * (pj_int64_t)fs)
                    sp->discard_credit = 2 * (pj_int64_t)fs;
            } else {
                /* Counted rather than swallowed. This runs on the
                   media thread, so the report is one line to stderr
                   the first time and a count thereafter -- enough for
                   somebody to find out it happened at all, which a
                   bare no-op would never allow. */
                sp->discard_failures++;
                if (sp->discard_failures == 1)
                    fprintf(stderr, "comlink: speed: WSOLA declined to "
                                    "discard (status %d, erased %u of %u); "
                                    "playback will drift toward 1x\n",
                            (int)st, erase_cnt, sp->combine_fill);
            }
        }
    }

    if (sp->combine_fill >= fs) {
        memcpy(dst, sp->combine, (size_t)fs * sizeof(pj_int16_t));
        memmove(sp->combine, sp->combine + fs,
               (size_t)(sp->combine_fill - fs) * sizeof(pj_int16_t));
        sp->combine_fill -= fs;
        frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
    } else if (sp->ended && sp->combine_fill > 0) {
        /* The file's genuine tail, shorter than a full frame, padded
           rather than held back. Reachable exactly once per file: the
           next call has combine_fill == 0 and ended still true, which
           falls to the plain PJMEDIA_FRAME_TYPE_NONE case below --
           end of file must still end, not pad forever. */
        memcpy(dst, sp->combine, (size_t)sp->combine_fill * sizeof(pj_int16_t));
        memset(dst + sp->combine_fill, 0,
              (size_t)(fs - sp->combine_fill) * sizeof(pj_int16_t));
        sp->combine_fill = 0;
        frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
    } else if (!sp->ended &&
              pjmedia_wsola_generate(sp->wsola, dst) == PJ_SUCCESS) {
        /* The retry cap above was hit with the source still live and
           still short of a full frame: WSOLA has already been fed
           everything pulled so far (every pjmedia_wsola_save call
           above), so it can extend from that history instead of this
           call pulling more. combine_fill (whatever is left of it) is
           deliberately left untouched, not folded into this generated
           frame -- it keeps accumulating toward a future real frame
           on subsequent calls rather than being spent here. */
        frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
    } else {
        frame->type = PJMEDIA_FRAME_TYPE_NONE;
    }
    return PJ_SUCCESS;
}

static pj_status_t speedport_get_frame(pjmedia_port *this_port,
                                       pjmedia_frame *frame) {
    sp_speedport *sp = (sp_speedport *)this_port;
    apply_pending_speed(sp);
    apply_pending_reset(sp);

    if (sp->mode == SP_SPEED_ONE)
        return get_frame_one(sp, frame);
    if (sp->mode == SP_SPEED_HALF)
        return get_frame_half(sp, frame);
    return get_frame_double(sp, frame);
}

static pj_status_t speedport_on_destroy(pjmedia_port *this_port) {
    sp_speedport *sp = (sp_speedport *)this_port;
    if (sp->wsola)
        pjmedia_wsola_destroy(sp->wsola);
    pj_pool_release(sp->pool);
    return PJ_SUCCESS;
}

sp_status sp_speedport_create(pj_pool_t *pool_, pjmedia_port *source,
                              unsigned clock_rate, unsigned channels,
                              unsigned samples_per_frame,
                              pjmedia_port **out) {
    if (!pool_ || !source || !out || !clock_rate || !channels ||
       !samples_per_frame)
        return SP_ERR_INVALID_ARG;
    *out = NULL;

    pj_pool_t *pool =
        pj_pool_create(pool_->factory, "sp_speedport", 1000, 1000, NULL);
    if (!pool)
        return SP_ERR_INTERNAL;

    sp_speedport *sp = PJ_POOL_ZALLOC_T(pool, sp_speedport);
    if (!sp) {
        pj_pool_release(pool);
        return SP_ERR_INTERNAL;
    }
    sp->pool = pool;
    sp->source = source;
    sp->channels = channels;
    sp->spf = samples_per_frame;
    sp->frame_samples = samples_per_frame * channels;
    sp->combine_cap = SP_SPEEDPORT_COMBINE_FRAMES * sp->frame_samples;
    atomic_init(&sp->requested_speed, 1.0f);
    sp->active_speed = 1.0f;
    sp->mode = SP_SPEED_ONE;
    atomic_init(&sp->reset_pending, PJ_FALSE);

    sp->tmp = pj_pool_alloc(pool,
                            (pj_size_t)sp->frame_samples * sizeof(pj_int16_t));
    sp->combine = pj_pool_alloc(
        pool, (pj_size_t)sp->combine_cap * sizeof(pj_int16_t));
    if (!sp->tmp || !sp->combine) {
        pj_pool_release(pool);
        return SP_ERR_INTERNAL;
    }

    /* NO_FADING: without it, WSOLA caps how many synthetic frames it
       will generate in a row before fading to silence -- a limit
       meant for packet-loss concealment, where a long stretch of
       generated audio means the network is in trouble and silence is
       the honest answer. Half-speed playback generates on purpose,
       indefinitely, for as long as the operator leaves it there; the
       same option pjmedia_delay_buf itself passes for the same
       reason (delaybuf.c's own pjmedia_wsola_create call). */
    pj_status_t st =
        pjmedia_wsola_create(pool, clock_rate, samples_per_frame, channels,
                             PJMEDIA_WSOLA_NO_FADING, &sp->wsola);
    if (st != PJ_SUCCESS) {
        pj_pool_release(pool);
        return SP_ERR_INTERNAL;
    }

    sp->base.get_frame = &speedport_get_frame;
    sp->base.on_destroy = &speedport_on_destroy;
    pj_str_t name = pj_str((char *)"sp_speedport");
    pjmedia_port_info_init(&sp->base.info, &name,
                           PJMEDIA_SIG_CLASS_PORT_AUD('S', 'P'), clock_rate,
                           channels, 16, samples_per_frame);

    *out = &sp->base;
    return SP_OK;
}

void sp_speedport_set_speed(pjmedia_port *port, float speed) {
    if (!port)
        return;
    sp_speedport *sp = (sp_speedport *)port;
    atomic_store(&sp->requested_speed, speed);
}

void sp_speedport_reset(pjmedia_port *port) {
    if (!port)
        return;
    sp_speedport *sp = (sp_speedport *)port;
    atomic_store(&sp->reset_pending, PJ_TRUE);
}
