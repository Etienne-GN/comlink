/* sp_speedport_*: time-stretching a source port for playback speed
 * control, with no core, no call and no sound device -- exactly the
 * standalone shape test_oggopus.c already uses for its own reader
 * port, since a pjmedia_port can be built and driven on its own.
 *
 * A synthetic tone_port stands in for whatever real source the
 * wrapper is given in production (the WAV player's own port, or
 * oggopus.h's reader): it produces a fixed number of frames of a pure
 * sine wave, then PJMEDIA_FRAME_TYPE_NONE, which is all get_frame()
 * on the wrapper ever looks at from a source.
 */
#include "sp_test.h"
#include "speedport.h"

#include <pj/pool.h>
#include <pjmedia/errno.h>
#include <pjmedia/port.h>
#include <pjmedia/signatures.h>

#include <math.h>
#include <string.h>

#define CLOCK_RATE 16000u
#define CHANNELS   1u
#define SPF        320u /* 20ms at 16kHz, this codebase's bridge ptime */
#define TONE_FREQ  440.0

static pj_caching_pool g_cp;

static pj_pool_t *new_pool(void) {
    return pj_pool_create(&g_cp.factory, "test_speedport", 8192, 8192, NULL);
}

/* A pure sine source, with a continuous phase across frames (not
 * reset per call): a discontinuity at every frame boundary would add
 * spurious zero crossings of its own and confuse speed_two_preserves_
 * pitch's frequency estimate. Stack-allocated by the tests, not pool-
 * backed -- fine, since tone_on_destroy does nothing and the tests
 * never ask a pool to free it.
 *
 * A perfectly pure, unvarying sine is technically a poor input for
 * WSOLA's own pitch-synchronised search: every offset that is a whole
 * number of periods away from any other is an equally perfect match,
 * which real speech (what this wrapper actually sees in production --
 * either the WAV player's own port or oggopus.h's reader, both of
 * which decode a recorded call) never presents. A small amount of
 * amplitude drift and deterministic broadband dither, well below the
 * tone's own amplitude, is added so find_pitch has a genuine nearby
 * best match to converge on instead of many equally-good distant
 * ones -- exactly the property real audio has for free. It is far
 * too small to add or remove a single zero crossing of its own (see
 * estimate_freq_hz's own comment on why that still suffices).
 */
typedef struct {
    pjmedia_port base;
    unsigned clock_rate;
    unsigned frame_samples;
    unsigned frames_total;
    unsigned frames_served;
    double freq;
    double phase_samples;
    pj_uint32_t noise_state; /* xorshift32, seeded fixed for reproducibility */
} tone_port;

static pj_int16_t tone_dither(tone_port *t) {
    /* xorshift32: a few integer ops, deterministic from a fixed seed,
       no libc RNG (whose sequence is not this test's to depend on). */
    pj_uint32_t x = t->noise_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t->noise_state = x;
    return (pj_int16_t)((int)(x % 121) - 60); /* roughly +/-60 */
}

static pj_status_t tone_get_frame(pjmedia_port *this_port,
                                  pjmedia_frame *frame) {
    tone_port *t = (tone_port *)this_port;
    if (t->frames_served >= t->frames_total) {
        frame->type = PJMEDIA_FRAME_TYPE_NONE;
        return PJ_SUCCESS;
    }
    pj_int16_t *dst = (pj_int16_t *)frame->buf;
    for (unsigned i = 0; i < t->frame_samples; i++) {
        double tt = t->phase_samples / (double)t->clock_rate;
        double amp = 8000.0 * (1.0 + 0.2 * sin(2.0 * M_PI * 2.0 * tt));
        double v = amp * sin(2.0 * M_PI * t->freq * tt);
        dst[i] = (pj_int16_t)v + tone_dither(t);
        t->phase_samples += 1.0;
    }
    t->frames_served++;
    frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
    return PJ_SUCCESS;
}

static pj_status_t tone_on_destroy(pjmedia_port *this_port) {
    (void)this_port;
    return PJ_SUCCESS;
}

static void tone_port_init(tone_port *t, unsigned clock_rate, unsigned spf,
                           unsigned frames_total, double freq) {
    memset(t, 0, sizeof *t);
    t->clock_rate = clock_rate;
    t->frame_samples = spf;
    t->frames_total = frames_total;
    t->freq = freq;
    t->noise_state = 0x9e3779b9u; /* fixed, non-zero: xorshift needs one */
    t->base.get_frame = &tone_get_frame;
    t->base.on_destroy = &tone_on_destroy;
    pj_str_t name = pj_str((char *)"tone");
    pjmedia_port_info_init(&t->base.info, &name,
                           PJMEDIA_SIG_CLASS_PORT_AUD('T', 'O'), clock_rate,
                           1, 16, spf);
}

static pjmedia_frame make_frame(pj_int16_t *buf) {
    pjmedia_frame f;
    f.type = PJMEDIA_FRAME_TYPE_AUDIO;
    f.buf = buf;
    f.size = (pj_size_t)SPF * sizeof(pj_int16_t);
    f.timestamp.u64 = 0;
    f.bit_info = 0;
    return f;
}

/* Zero-crossing counting over a windowed tone is enough to estimate
 * its dominant frequency here, and needs no FFT: the signal under
 * test is a single sinusoid with nothing else in it, so every rising
 * (negative-to-non-negative) crossing marks one full cycle, and
 * counting them over a known time window gives the frequency
 * directly. A resampling implementation played back at 2x halves the
 * period of every cycle, which doubles the crossing rate -- exactly
 * the distinction this test exists to catch. An FFT's extra
 * resolution and leakage handling would only matter for a multi-tone
 * or noisy signal, and this one is neither.
 */
static double estimate_freq_hz(const pj_int16_t *samples, unsigned n,
                               unsigned clock_rate) {
    unsigned crossings = 0;
    for (unsigned i = 1; i < n; i++) {
        if (samples[i - 1] < 0 && samples[i] >= 0)
            crossings++;
    }
    double seconds = (double)n / (double)clock_rate;
    return seconds > 0 ? (double)crossings / seconds : 0.0;
}

SP_TEST(speed_one_passes_audio_through_unchanged) {
    pj_pool_t *pool = new_pool();

    tone_port src, ref;
    tone_port_init(&src, CLOCK_RATE, SPF, 5, TONE_FREQ);
    tone_port_init(&ref, CLOCK_RATE, SPF, 5, TONE_FREQ); /* same phase */

    pjmedia_port *wrap = NULL;
    SP_ASSERT(sp_speedport_create(pool, &src.base, CLOCK_RATE, CHANNELS, SPF,
                                  &wrap) == SP_OK);

    pj_int16_t direct[SPF], through[SPF];
    pjmedia_frame fd = make_frame(direct);
    SP_ASSERT(pjmedia_port_get_frame(&ref.base, &fd) == PJ_SUCCESS);
    SP_ASSERT(fd.type == PJMEDIA_FRAME_TYPE_AUDIO);

    pjmedia_frame fw = make_frame(through);
    SP_ASSERT(pjmedia_port_get_frame(wrap, &fw) == PJ_SUCCESS);
    SP_ASSERT(fw.type == PJMEDIA_FRAME_TYPE_AUDIO);

    /* At 1.0 no WSOLA call is made at all -- get_frame_one forwards
       straight to the source -- so the two must be bit-identical, not
       merely close. */
    SP_ASSERT(memcmp(direct, through, sizeof direct) == 0);

    pjmedia_port_destroy(wrap);
    pj_pool_release(pool);
}

SP_TEST(speed_two_consumes_the_source_in_half_the_output_frames) {
    pj_pool_t *pool = new_pool();
    unsigned n_src_frames = 40;
    tone_port src;
    tone_port_init(&src, CLOCK_RATE, SPF, n_src_frames, TONE_FREQ);

    pjmedia_port *wrap = NULL;
    SP_ASSERT(sp_speedport_create(pool, &src.base, CLOCK_RATE, CHANNELS, SPF,
                                  &wrap) == SP_OK);
    sp_speedport_set_speed(wrap, 2.0f);

    unsigned out_frames = 0;
    for (;;) {
        pj_int16_t buf[SPF];
        pjmedia_frame f = make_frame(buf);
        SP_ASSERT(pjmedia_port_get_frame(wrap, &f) == PJ_SUCCESS);
        if (f.type == PJMEDIA_FRAME_TYPE_NONE)
            break;
        out_frames++;
        if (out_frames >= 1000) { /* runaway guard: fail, don't hang */
            SP_ASSERT(out_frames < 1000);
            break;
        }
    }

    unsigned expected = n_src_frames / 2;
    unsigned diff = out_frames > expected ? out_frames - expected
                                          : expected - out_frames;
    SP_ASSERT(diff <= 1); /* a frame either way for WSOLA's window */

    pjmedia_port_destroy(wrap);
    pj_pool_release(pool);
}

SP_TEST(speed_half_consumes_the_source_in_twice_the_output_frames) {
    pj_pool_t *pool = new_pool();
    unsigned n_src_frames = 20;
    tone_port src;
    tone_port_init(&src, CLOCK_RATE, SPF, n_src_frames, TONE_FREQ);

    pjmedia_port *wrap = NULL;
    SP_ASSERT(sp_speedport_create(pool, &src.base, CLOCK_RATE, CHANNELS, SPF,
                                  &wrap) == SP_OK);
    sp_speedport_set_speed(wrap, 0.5f);

    unsigned out_frames = 0;
    for (;;) {
        pj_int16_t buf[SPF];
        pjmedia_frame f = make_frame(buf);
        SP_ASSERT(pjmedia_port_get_frame(wrap, &f) == PJ_SUCCESS);
        if (f.type == PJMEDIA_FRAME_TYPE_NONE)
            break;
        out_frames++;
        if (out_frames >= 1000) {
            SP_ASSERT(out_frames < 1000);
            break;
        }
    }

    unsigned expected = n_src_frames * 2;
    unsigned diff = out_frames > expected ? out_frames - expected
                                          : expected - out_frames;
    SP_ASSERT(diff <= 1);

    pjmedia_port_destroy(wrap);
    pj_pool_release(pool);
}

/* The load-bearing test: everything above also passes against a
 * plain resampler (decimate for 2x, interpolate for 0.5x), which is
 * exactly the naive shortcut WSOLA exists to avoid. Only the pitch
 * measured here tells the two apart. See speedport.c's Step 5 proof
 * in the task report for what happens to this specific test when
 * WSOLA is swapped for decimation.
 */
SP_TEST(speed_two_preserves_pitch) {
    pj_pool_t *pool = new_pool();
    unsigned n_src_frames = 100; /* 2s of tone: plenty of cycles at 440Hz */
    tone_port src;
    tone_port_init(&src, CLOCK_RATE, SPF, n_src_frames, TONE_FREQ);

    pjmedia_port *wrap = NULL;
    SP_ASSERT(sp_speedport_create(pool, &src.base, CLOCK_RATE, CHANNELS, SPF,
                                  &wrap) == SP_OK);
    sp_speedport_set_speed(wrap, 2.0f);

    pj_int16_t out[SPF * 200];
    unsigned out_n = 0;
    for (unsigned calls = 0; calls < 1000; calls++) {
        pj_int16_t buf[SPF];
        pjmedia_frame f = make_frame(buf);
        SP_ASSERT(pjmedia_port_get_frame(wrap, &f) == PJ_SUCCESS);
        if (f.type == PJMEDIA_FRAME_TYPE_NONE)
            break;
        if (out_n + SPF <= sizeof(out) / sizeof(out[0])) {
            memcpy(out + out_n, buf, (size_t)SPF * sizeof(pj_int16_t));
            out_n += SPF;
        }
    }
    SP_ASSERT(out_n >= CLOCK_RATE); /* at least a second, for a clean count */

    double measured = estimate_freq_hz(out, out_n, CLOCK_RATE);
    double err = fabs(measured - TONE_FREQ) / TONE_FREQ;
    /* Within a few percent of the source's own frequency. A
       resampling implementation would land near double (880Hz, an
       octave up), which is nowhere near this tolerance. */
    SP_ASSERT(err < 0.05);

    pjmedia_port_destroy(wrap);
    pj_pool_release(pool);
}

SP_TEST(an_unsupported_speed_is_refused) {
    /* sp_speedport_set_speed itself is void and never refuses -- see
       speedport.h -- because the refusal that matters to a caller
       (SP_ERR_INVALID_ARG) belongs to sp_playback_set_speed
       (comlink/core.h), which validates before this layer is ever
       reached. What this port-level test checks is the documented
       fallback: an unrecognised value behaves as 1.0 (plain
       passthrough) rather than crashing or silently misbehaving. */
    pj_pool_t *pool = new_pool();
    tone_port src, ref;
    tone_port_init(&src, CLOCK_RATE, SPF, 3, TONE_FREQ);
    tone_port_init(&ref, CLOCK_RATE, SPF, 3, TONE_FREQ);

    pjmedia_port *wrap = NULL;
    SP_ASSERT(sp_speedport_create(pool, &src.base, CLOCK_RATE, CHANNELS, SPF,
                                  &wrap) == SP_OK);
    sp_speedport_set_speed(wrap, 1.7f);

    pj_int16_t direct[SPF], through[SPF];
    pjmedia_frame fd = make_frame(direct);
    SP_ASSERT(pjmedia_port_get_frame(&ref.base, &fd) == PJ_SUCCESS);
    pjmedia_frame fw = make_frame(through);
    SP_ASSERT(pjmedia_port_get_frame(wrap, &fw) == PJ_SUCCESS);
    SP_ASSERT(fw.type == PJMEDIA_FRAME_TYPE_AUDIO);
    SP_ASSERT(memcmp(direct, through, sizeof direct) == 0);

    pjmedia_port_destroy(wrap);
    pj_pool_release(pool);
}

SP_TEST(speed_can_change_mid_playback) {
    pj_pool_t *pool = new_pool();
    unsigned n_src_frames = 60;
    tone_port src;
    tone_port_init(&src, CLOCK_RATE, SPF, n_src_frames, TONE_FREQ);

    pjmedia_port *wrap = NULL;
    SP_ASSERT(sp_speedport_create(pool, &src.base, CLOCK_RATE, CHANNELS, SPF,
                                  &wrap) == SP_OK);

    unsigned out_frames = 0;
    for (unsigned i = 0; i < 10; i++) {
        pj_int16_t buf[SPF];
        pjmedia_frame f = make_frame(buf);
        SP_ASSERT(pjmedia_port_get_frame(wrap, &f) == PJ_SUCCESS);
        SP_ASSERT(f.type == PJMEDIA_FRAME_TYPE_AUDIO);
        out_frames++;
    }

    sp_speedport_set_speed(wrap, 2.0f);

    pj_bool_t saw_none = PJ_FALSE;
    for (;;) {
        pj_int16_t buf[SPF];
        pjmedia_frame f = make_frame(buf);
        SP_ASSERT(pjmedia_port_get_frame(wrap, &f) == PJ_SUCCESS);
        if (f.type == PJMEDIA_FRAME_TYPE_NONE) {
            saw_none = PJ_TRUE;
            break;
        }
        out_frames++;
        /* Stays bounded: no click, no stall, no runaway generation
           past the source's own end. */
        if (out_frames >= 1000) {
            SP_ASSERT(out_frames < 1000);
            break;
        }
    }
    SP_ASSERT(saw_none);
    SP_ASSERT(out_frames > 10); /* kept producing audio after the switch */

    pjmedia_port_destroy(wrap);
    pj_pool_release(pool);
}

/* sp_speedport_reset is the seek counterpart to a speed switch: same
 * do_reset() in speedport.c, called with the speed left alone. At
 * 0.5x, get_frame_half alternates one real source frame with one
 * WSOLA-synthesised one (half_pending_generate) -- without a reset,
 * the frame right after a real pull is always the synthesised one.
 * Resetting in between forces the next call back to a real pull
 * instead, which is directly observable here as frames_served
 * jumping by one on THAT call rather than the one after -- exactly
 * what dropping the queued "generate next" state should do, and
 * exactly what a seek needs so the frame right after it comes from
 * the new (post-seek) source position rather than a leftover
 * synthesised one for the old position. Reachable with no core, no
 * call and no sound device, same as every other test in this file. */
SP_TEST(reset_drops_the_pending_synthesised_half_speed_frame) {
    pj_pool_t *pool = new_pool();
    tone_port src;
    tone_port_init(&src, CLOCK_RATE, SPF, 10, TONE_FREQ);

    pjmedia_port *wrap = NULL;
    SP_ASSERT(sp_speedport_create(pool, &src.base, CLOCK_RATE, CHANNELS, SPF,
                                  &wrap) == SP_OK);
    sp_speedport_set_speed(wrap, 0.5f);

    pj_int16_t buf[SPF];
    pjmedia_frame f = make_frame(buf);
    SP_ASSERT(pjmedia_port_get_frame(wrap, &f) == PJ_SUCCESS);
    SP_ASSERT(f.type == PJMEDIA_FRAME_TYPE_AUDIO);
    SP_ASSERT(src.frames_served == 1); /* the real pull */

    /* Without a reset, this next call would emit the synthesised
       frame queued above and leave frames_served at 1. */
    sp_speedport_reset(wrap);

    f = make_frame(buf);
    SP_ASSERT(pjmedia_port_get_frame(wrap, &f) == PJ_SUCCESS);
    SP_ASSERT(f.type == PJMEDIA_FRAME_TYPE_AUDIO);
    SP_ASSERT(src.frames_served == 2); /* a real pull, not the synth one */

    pjmedia_port_destroy(wrap);
    pj_pool_release(pool);
}

/* Harmless at 1.0x, per sp_speedport_reset's own comment: get_frame_one
 * never touches WSOLA, so nothing here should change or crash. */
SP_TEST(reset_at_default_speed_does_not_disturb_playback) {
    pj_pool_t *pool = new_pool();
    tone_port src, ref;
    tone_port_init(&src, CLOCK_RATE, SPF, 3, TONE_FREQ);
    tone_port_init(&ref, CLOCK_RATE, SPF, 3, TONE_FREQ);

    pjmedia_port *wrap = NULL;
    SP_ASSERT(sp_speedport_create(pool, &src.base, CLOCK_RATE, CHANNELS, SPF,
                                  &wrap) == SP_OK);
    sp_speedport_reset(wrap);

    pj_int16_t direct[SPF], through[SPF];
    pjmedia_frame fd = make_frame(direct);
    SP_ASSERT(pjmedia_port_get_frame(&ref.base, &fd) == PJ_SUCCESS);
    pjmedia_frame fw = make_frame(through);
    SP_ASSERT(pjmedia_port_get_frame(wrap, &fw) == PJ_SUCCESS);
    SP_ASSERT(fw.type == PJMEDIA_FRAME_TYPE_AUDIO);
    SP_ASSERT(memcmp(direct, through, sizeof direct) == 0);

    pjmedia_port_destroy(wrap);
    pj_pool_release(pool);
}

int main(void) {
    pj_init();
    pj_caching_pool_init(&g_cp, NULL, 0);

    SP_RUN(speed_one_passes_audio_through_unchanged);
    SP_RUN(speed_two_consumes_the_source_in_half_the_output_frames);
    SP_RUN(speed_half_consumes_the_source_in_twice_the_output_frames);
    SP_RUN(speed_two_preserves_pitch);
    SP_RUN(an_unsupported_speed_is_refused);
    SP_RUN(speed_can_change_mid_playback);
    SP_RUN(reset_drops_the_pending_synthesised_half_speed_frame);
    SP_RUN(reset_at_default_speed_does_not_disturb_playback);

    pj_caching_pool_destroy(&g_cp);
    return SP_TEST_RESULT();
}
