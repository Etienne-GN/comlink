/* Drives the OggOpus writer port directly: synthesised PCM frames in,
   an .opus file on disk out. No core, no call, no sound device -- see
   oggopus.h for why the port is built to allow exactly this. */
#include "sp_test.h"
#include "oggopus.h"

#include <ogg/ogg.h>

#include <pj/pool.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static pj_caching_pool g_cp;

static pj_pool_t *new_pool(void) {
    return pj_pool_create(&g_cp.factory, "test_oggopus", 4096, 4096, NULL);
}

static const char *test_dir(void) {
    static char dir[512];
    snprintf(dir, sizeof dir, "%s/oggopus_test", getenv("SP_TEST_TMP"));
    mkdir(dir, 0777);
    return dir;
}

static const char *test_path(const char *name) {
    static char path[600];
    snprintf(path, sizeof path, "%s/%s", test_dir(), name);
    return path;
}

/* A tone, so a round trip (task 4) can be checked for more than "it
   decoded" -- silence would pass against a decoder that returns
   zeroes. Not load-bearing for the container-level assertions here,
   but real content is a more honest input than a buffer of zeros. */
static void fill_tone(pj_int16_t *buf, unsigned n, unsigned rate) {
    const double freq = 440.0;
    for (unsigned i = 0; i < n; i++) {
        double t = (double)i / (double)rate;
        buf[i] = (pj_int16_t)(8000.0 * sin(2.0 * M_PI * freq * t));
    }
}

static pj_status_t feed(pjmedia_port *port, unsigned rate, unsigned samples) {
    pj_int16_t buf[16000];
    SP_ASSERT(samples <= sizeof buf / sizeof buf[0]);
    fill_tone(buf, samples, rate);

    pjmedia_frame frame;
    frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
    frame.buf = buf;
    frame.size = (pj_size_t)samples * sizeof(pj_int16_t);
    frame.timestamp.u64 = 0;
    frame.bit_info = 0;
    return pjmedia_port_put_frame(port, &frame);
}

/* Reads every packet out of an OggOpus file with libogg's own decode
   path -- the same machinery a real player uses -- rather than
   trusting our own writer's bookkeeping. Also records the last page's
   granule position and its end-of-stream flag, since those are
   per-page, not per-packet. */
#define MAX_PACKETS 64
#define MAX_PACKET_BYTES 4000

typedef struct {
    unsigned char data[MAX_PACKET_BYTES];
    long len;
} captured_packet;

typedef struct {
    captured_packet packets[MAX_PACKETS];
    int n_packets;
    ogg_int64_t last_granulepos;
    int last_eos;
    int n_pages;
    int first4_is_oggs;
} stream_dump;

static int dump_stream(const char *path, stream_dump *out) {
    memset(out, 0, sizeof *out);

    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;

    unsigned char magic[4] = {0};
    size_t got = fread(magic, 1, sizeof magic, f);
    out->first4_is_oggs =
        (got == sizeof magic && memcmp(magic, "OggS", 4) == 0);
    rewind(f);

    ogg_sync_state oy;
    ogg_sync_init(&oy);
    ogg_stream_state os;
    int os_inited = 0;

    for (;;) {
        char *buf = ogg_sync_buffer(&oy, 4096);
        size_t n = fread(buf, 1, 4096, f);
        if (ogg_sync_wrote(&oy, (long)n) != 0)
            break;
        if (n == 0)
            break;

        ogg_page og;
        while (ogg_sync_pageout(&oy, &og) == 1) {
            if (!os_inited) {
                ogg_stream_init(&os, ogg_page_serialno(&og));
                os_inited = 1;
            }
            out->n_pages++;
            out->last_granulepos = ogg_page_granulepos(&og);
            out->last_eos = ogg_page_eos(&og);

            ogg_stream_pagein(&os, &og);
            ogg_packet op;
            while (ogg_stream_packetout(&os, &op) == 1) {
                if (out->n_packets < MAX_PACKETS) {
                    long copy = op.bytes;
                    if (copy > MAX_PACKET_BYTES)
                        copy = MAX_PACKET_BYTES;
                    memcpy(out->packets[out->n_packets].data, op.packet,
                          (size_t)copy);
                    out->packets[out->n_packets].len = op.bytes;
                    out->n_packets++;
                }
            }
        }
    }

    if (os_inited)
        ogg_stream_clear(&os);
    ogg_sync_clear(&oy);
    fclose(f);
    return 1;
}

/* OpusHead field readers, straight off the bytes the writer emitted --
   not the writer's own idea of what it wrote. Layout: RFC 7845 S5.1. */
static unsigned head_pre_skip(const stream_dump *d) {
    SP_ASSERT(d->n_packets >= 1);
    SP_ASSERT(d->packets[0].len >= 19);
    return (unsigned)d->packets[0].data[10] |
          ((unsigned)d->packets[0].data[11] << 8);
}

static unsigned head_input_rate(const stream_dump *d) {
    SP_ASSERT(d->n_packets >= 1);
    SP_ASSERT(d->packets[0].len >= 19);
    return (unsigned)d->packets[0].data[12] |
          ((unsigned)d->packets[0].data[13] << 8) |
          ((unsigned)d->packets[0].data[14] << 16) |
          ((unsigned)d->packets[0].data[15] << 24);
}

SP_TEST(a_written_file_starts_with_an_ogg_page) {
    pj_pool_t *pool = new_pool();
    const char *path = test_path("starts_with_page.opus");
    pjmedia_port *port = NULL;
    SP_ASSERT(sp_oggopus_writer_create(pool, path, 16000, 1, 320, &port) ==
             SP_OK);
    SP_ASSERT(port != NULL);
    pjmedia_port_destroy(port);

    stream_dump d;
    SP_ASSERT(dump_stream(path, &d));
    SP_ASSERT(d.first4_is_oggs);

    pj_pool_release(pool);
}

SP_TEST(the_first_packet_is_an_opus_head) {
    pj_pool_t *pool = new_pool();
    const char *path = test_path("opus_head.opus");
    pjmedia_port *port = NULL;
    SP_ASSERT(sp_oggopus_writer_create(pool, path, 16000, 1, 320, &port) ==
             SP_OK);
    pjmedia_port_destroy(port);

    stream_dump d;
    SP_ASSERT(dump_stream(path, &d));
    SP_ASSERT(d.n_packets >= 1);
    if (d.n_packets >= 1) {
        SP_ASSERT(d.packets[0].len >= 19);
        SP_ASSERT(memcmp(d.packets[0].data, "OpusHead", 8) == 0);
        SP_ASSERT(d.packets[0].data[8] == 1);   /* version */
        SP_ASSERT(d.packets[0].data[9] == 1);   /* channel count: mono */
    }

    pj_pool_release(pool);
}

SP_TEST(the_second_packet_is_opus_tags) {
    pj_pool_t *pool = new_pool();
    const char *path = test_path("opus_tags.opus");
    pjmedia_port *port = NULL;
    SP_ASSERT(sp_oggopus_writer_create(pool, path, 16000, 1, 320, &port) ==
             SP_OK);
    pjmedia_port_destroy(port);

    stream_dump d;
    SP_ASSERT(dump_stream(path, &d));
    SP_ASSERT(d.n_packets >= 2);
    if (d.n_packets >= 2) {
        SP_ASSERT(d.packets[1].len >= 8);
        SP_ASSERT(memcmp(d.packets[1].data, "OpusTags", 8) == 0);
    }

    pj_pool_release(pool);
}

/* Nothing tested the header's own content directly: a wrong
   sample-rate or pre-skip field would only be caught indirectly, and
   only once Task 4's reader exists. Reading both back off the bytes
   the writer emitted catches a wrong value independent of that, and a
   pre_skip of exactly 0 specifically catches OPUS_GET_LOOKAHEAD
   silently failing and leaving it at its initialised value -- a real
   encoder's lookahead is never 0. */
SP_TEST(the_opus_head_reports_rate_and_a_real_pre_skip) {
    pj_pool_t *pool = new_pool();
    const char *path = test_path("head_fields.opus");
    pjmedia_port *port = NULL;
    SP_ASSERT(sp_oggopus_writer_create(pool, path, 24000, 2, 480, &port) ==
             SP_OK);
    pjmedia_port_destroy(port);

    stream_dump d;
    SP_ASSERT(dump_stream(path, &d));
    SP_ASSERT(d.n_packets >= 1);
    if (d.n_packets >= 1) {
        SP_ASSERT(d.packets[0].data[9] == 2); /* channel count: stereo */
        SP_ASSERT(head_input_rate(&d) == 24000);
        SP_ASSERT(head_pre_skip(&d) > 0);
    }

    pj_pool_release(pool);
}

/* RFC 7845 S4.3: a granule position counts decoded samples from the
   start of the logical stream, and that count includes the pre-skip
   samples themselves -- playback duration is granule minus pre-skip,
   not granule alone. A stream with no real audio at all must
   therefore still end with a granule of exactly pre_skip, giving a
   valid zero duration. Starting the running granule at 0 instead
   makes an empty recording's duration negative -- confirmed with
   opusinfo, which reports exactly that as "created by a buggy
   encoder". */
SP_TEST(an_empty_stream_ends_with_the_pre_skip_granule) {
    pj_pool_t *pool = new_pool();
    const char *path = test_path("empty.opus");
    pjmedia_port *port = NULL;
    SP_ASSERT(sp_oggopus_writer_create(pool, path, 16000, 1, 320, &port) ==
             SP_OK);
    pjmedia_port_destroy(port); /* no put_frame call at all */

    stream_dump d;
    SP_ASSERT(dump_stream(path, &d));
    SP_ASSERT(d.last_eos != 0);
    SP_ASSERT(d.last_granulepos == (ogg_int64_t)head_pre_skip(&d));
    SP_ASSERT(d.last_granulepos > 0);

    pj_pool_release(pool);
}

/* The bug this test exists to prevent: granules are in 48kHz samples
   whatever the capture rate. One second of 16kHz audio is exactly 50
   frames of 20ms (16000 / 320), so nothing is left buffered at
   destroy and the final granule lands exactly on 48000 + pre_skip --
   not near 16000, which is what a writer that advanced the granule by
   the capture rate would produce instead. The exact figure (not a
   tolerance band) is what makes this test able to tell the correct
   accounting apart from one that forgets the pre-skip offset: one
   Opus frame is 960 samples and the pre-skip is a few hundred, so a
   tolerance wide enough to allow for the frame would also have hidden
   a missing pre-skip. */
SP_TEST(granule_positions_are_in_48k_samples) {
    pj_pool_t *pool = new_pool();
    const char *path = test_path("granule_48k.opus");
    pjmedia_port *port = NULL;
    SP_ASSERT(sp_oggopus_writer_create(pool, path, 16000, 1, 320, &port) ==
             SP_OK);

    SP_ASSERT(feed(port, 16000, 16000) == PJ_SUCCESS); /* one second */
    pjmedia_port_destroy(port);

    stream_dump d;
    SP_ASSERT(dump_stream(path, &d));
    unsigned pre_skip = head_pre_skip(&d);
    SP_ASSERT(d.last_granulepos == (ogg_int64_t)(48000 + pre_skip));
    SP_ASSERT(llabs((long long)d.last_granulepos - 16000) > 1000);

    pj_pool_release(pool);
}

SP_TEST(the_last_page_is_flagged_end_of_stream) {
    pj_pool_t *pool = new_pool();
    const char *path = test_path("eos_flag.opus");
    pjmedia_port *port = NULL;
    SP_ASSERT(sp_oggopus_writer_create(pool, path, 16000, 1, 320, &port) ==
             SP_OK);

    SP_ASSERT(feed(port, 16000, 320) == PJ_SUCCESS);
    pjmedia_port_destroy(port);

    stream_dump d;
    SP_ASSERT(dump_stream(path, &d));
    SP_ASSERT(d.last_eos != 0);

    pj_pool_release(pool);
}

/* The bridge's frame size and Opus's legal frame sizes are different
   things: put_frame is fed 100 samples at a time into a writer whose
   Opus frame is 320 samples (20ms at 16kHz). 10 calls (1000 samples)
   cross three frame boundaries with 40 samples left over, which
   destroy() must pad, trim and flag end-of-stream -- exercising both
   the buffering and the finalisation math in one shot. */
SP_TEST(a_frame_size_opus_cannot_encode_is_buffered) {
    pj_pool_t *pool = new_pool();
    const char *path = test_path("ragged_frames.opus");
    pjmedia_port *port = NULL;
    SP_ASSERT(sp_oggopus_writer_create(pool, path, 16000, 1, 320, &port) ==
             SP_OK);

    for (int i = 0; i < 10; i++)
        SP_ASSERT(feed(port, 16000, 100) == PJ_SUCCESS);
    pjmedia_port_destroy(port);

    stream_dump d;
    SP_ASSERT(dump_stream(path, &d));
    /* 2 header packets + 3 full 320-sample frames + 1 trimmed
       remainder frame carrying end-of-stream. */
    SP_ASSERT(d.n_packets == 6);
    SP_ASSERT(d.last_eos != 0);
    /* pre_skip (the running granule's starting point) + 3 full frames
       (960 granule each) + the 40-sample remainder scaled to 48kHz
       and rounded: (40*48000 + 8000) / 16000 = 120. */
    unsigned pre_skip = head_pre_skip(&d);
    SP_ASSERT(d.last_granulepos == (ogg_int64_t)(pre_skip + 3 * 960 + 120));

    pj_pool_release(pool);
}

/* Destroying the writer -- not merely writing frames to it -- is what
   finalises the file. Nothing is fed beyond a single partial buffer,
   so without on_destroy doing the padding/trim/EOS work this file
   would have no EOS page and a granule stuck at 0. */
SP_TEST(destroying_the_writer_finalises_the_file) {
    pj_pool_t *pool = new_pool();
    const char *path = test_path("finalise.opus");
    pjmedia_port *port = NULL;
    SP_ASSERT(sp_oggopus_writer_create(pool, path, 8000, 1, 160, &port) ==
             SP_OK);

    SP_ASSERT(feed(port, 8000, 100) == PJ_SUCCESS); /* < 160: no full frame */
    pjmedia_port_destroy(port);

    stream_dump d;
    SP_ASSERT(dump_stream(path, &d));
    SP_ASSERT(d.n_packets == 3); /* 2 headers + 1 finalised remainder */
    SP_ASSERT(d.last_eos != 0);
    /* pre_skip + (100*48000 + 4000) / 8000 = pre_skip + 600. */
    unsigned pre_skip = head_pre_skip(&d);
    SP_ASSERT(d.last_granulepos == (ogg_int64_t)(pre_skip + 600));

    pj_pool_release(pool);
}

/* --- Task 4: the reader, and a round trip that is not circular ------
 *
 * dump_stream above proves the container's bytes are right by reading
 * them with libogg directly, never trusting the writer's own idea of
 * what it wrote. The tests below do the equivalent for the DECODED
 * AUDIO: they read the file back through sp_oggopus_reader_create --
 * the same port the phone plays a recording through -- and check the
 * samples that come out against the samples that went in, rather than
 * merely that some samples came out. A decoder that silently returns
 * zeroes would pass any test that only counts bytes; it fails these,
 * because a silent decode correlates with nothing.
 *
 * That still leaves the circularity the plan warns about: this only
 * proves our reader accepts our writer, which share every assumption
 * by construction. Two independent tools close that gap and are run
 * by hand against files this suite writes, not from C:
 *   - opusdec (opus-tools), decoding the same file outside this code
 *     entirely, compared against what the reader produces;
 *   - opusinfo (opus-tools), which parses the container looking for
 *     exactly the kind of page/granule mistake a player tolerates and
 *     a spec does not -- it is what caught the writer's own pre-skip
 *     bug that review and testing both missed. See the task report
 *     for both tools' output on these files.
 */

/* Same 440Hz tone fill_tone above produces, reused rather than
 * reimplemented so the reference and the round trip agree on exactly
 * what "the source tone" means bit for bit. */
static double pearson_correlation(const pj_int16_t *a, const pj_int16_t *b,
                                  unsigned n) {
    if (n == 0)
        return 0.0;
    double ma = 0.0, mb = 0.0;
    for (unsigned i = 0; i < n; i++) {
        ma += a[i];
        mb += b[i];
    }
    ma /= n;
    mb /= n;
    double num = 0.0, da = 0.0, db = 0.0;
    for (unsigned i = 0; i < n; i++) {
        double xa = (double)a[i] - ma, xb = (double)b[i] - mb;
        num += xa * xb;
        da += xa * xa;
        db += xb * xb;
    }
    if (da <= 0.0 || db <= 0.0)
        return 0.0;
    return num / sqrt(da * db);
}

static double rms(const pj_int16_t *a, unsigned n) {
    if (n == 0)
        return 0.0;
    double s = 0.0;
    for (unsigned i = 0; i < n; i++)
        s += (double)a[i] * (double)a[i];
    return sqrt(s / n);
}

#define SP_RT_RATE 16000u
#define SP_RT_FRAME 320u /* 20ms @ 16kHz, matching the recording bridge */
#define SP_RT_SECONDS 1u
#define SP_RT_SAMPLES (SP_RT_RATE * SP_RT_SECONDS) /* 50 whole frames: no
                                                       trailing padding, so
                                                       the decoded count
                                                       below is exact, not
                                                       a tolerance band */
#define SP_RT_DECODE_CAP 20000u

/* Pulls every frame the reader has, at exactly the size the recording
 * bridge asks in production (SP_RT_FRAME), until sp_oggopus_reader_is_eof
 * reports true. The iteration cap is generous slack against a bug that
 * never sets eof, not a real expected count -- true completion is
 * driven by the eof flag, the same one playback.c polls. */
static unsigned decode_all(pjmedia_port *port, pj_int16_t *out, unsigned cap) {
    pj_int16_t fbuf[SP_RT_FRAME];
    pjmedia_frame frame;
    frame.buf = fbuf;
    frame.size = sizeof fbuf;
    frame.timestamp.u64 = 0;
    frame.bit_info = 0;

    unsigned n = 0;
    for (int iter = 0; iter < 300; iter++) {
        pj_bool_t eof = PJ_FALSE;
        SP_ASSERT(sp_oggopus_reader_is_eof(port, &eof) == SP_OK);
        if (eof)
            break;
        SP_ASSERT(pjmedia_port_get_frame(port, &frame) == PJ_SUCCESS);
        if (frame.type == PJMEDIA_FRAME_TYPE_AUDIO) {
            SP_ASSERT(n + SP_RT_FRAME <= cap);
            memcpy(out + n, fbuf, sizeof fbuf);
            n += SP_RT_FRAME;
        }
    }
    return n;
}

/* The assertion that matters most, per the plan: decoded audio must
 * correlate with the source tone, not merely exist. Threshold and
 * rationale: 24kbps Opus on a single clean sine is close to lossless
 * perceptually, so a correct decode should land near 1.0; 0.9 is set
 * well below that but far above what a broken decode would produce.
 * A wrong pre-skip in particular shifts the decoded signal by roughly
 * a lookahead's worth of samples against a wave whose period is only
 * ~36 samples at 16kHz -- many cycles of phase error, which collapses
 * zero-lag correlation toward 0, not down to 0.9. Lossy coding alone
 * cannot produce a failure this test would catch; a wrong pre-skip or
 * a decoder returning silence both would. */
#define SP_RT_MIN_CORRELATION 0.9

SP_TEST(round_trip_decodes_the_tone_and_correlates_with_the_source) {
    pj_pool_t *pool = new_pool();
    const char *path = test_path("round_trip.opus");

    pj_int16_t source[SP_RT_SAMPLES];
    fill_tone(source, SP_RT_SAMPLES, SP_RT_RATE);

    pjmedia_port *wport = NULL;
    SP_ASSERT(sp_oggopus_writer_create(pool, path, SP_RT_RATE, 1, SP_RT_FRAME,
                                       &wport) == SP_OK);
    SP_ASSERT(feed(wport, SP_RT_RATE, SP_RT_SAMPLES) == PJ_SUCCESS);
    pjmedia_port_destroy(wport);

    pjmedia_port *rport = NULL;
    SP_ASSERT(sp_oggopus_reader_create(pool, path, SP_RT_RATE, 1, SP_RT_FRAME,
                                       &rport) == SP_OK);

    pj_int16_t decoded[SP_RT_DECODE_CAP];
    unsigned n = decode_all(rport, decoded, SP_RT_DECODE_CAP);

    /* Exact, not approximate: SP_RT_SAMPLES is a whole number of Opus
       frames, so there is no partial final frame for end-trim to
       round, and pre-skip is a fixed discard from the front -- the
       real duration this file represents is exactly SP_RT_SAMPLES. */
    SP_ASSERT(n == SP_RT_SAMPLES);

    double corr = pearson_correlation(source, decoded, SP_RT_SAMPLES);
    SP_ASSERT(corr > SP_RT_MIN_CORRELATION);

    pjmedia_port_destroy(rport);
    pj_pool_release(pool);
}

/* Hazard 1, tested directly rather than only implied by the
 * whole-file correlation above: a reader that forgets to discard
 * OpusHead's pre-skip does not fail to decode -- it decodes fine, just
 * shifted, so the recording's first syllable (or, here, the first
 * cycle of the tone) is missing or replaced with the encoder's
 * priming artifact. Checked two ways on just the first 20ms: energy
 * (a shifted/silent start has much lower RMS than a full-amplitude
 * tone) and correlation against the source's own opening window,
 * which a shift by even a fraction of the ~36-sample period at 16kHz
 * would badly weaken. */
SP_TEST(the_reader_discards_pre_skip_so_playback_starts_at_time_zero) {
    pj_pool_t *pool = new_pool();
    const char *path = test_path("pre_skip_start.opus");

    pj_int16_t source[SP_RT_SAMPLES];
    fill_tone(source, SP_RT_SAMPLES, SP_RT_RATE);

    pjmedia_port *wport = NULL;
    SP_ASSERT(sp_oggopus_writer_create(pool, path, SP_RT_RATE, 1, SP_RT_FRAME,
                                       &wport) == SP_OK);
    SP_ASSERT(feed(wport, SP_RT_RATE, SP_RT_SAMPLES) == PJ_SUCCESS);
    pjmedia_port_destroy(wport);

    pjmedia_port *rport = NULL;
    SP_ASSERT(sp_oggopus_reader_create(pool, path, SP_RT_RATE, 1, SP_RT_FRAME,
                                       &rport) == SP_OK);

    pj_int16_t decoded[SP_RT_DECODE_CAP];
    unsigned n = decode_all(rport, decoded, SP_RT_DECODE_CAP);
    SP_ASSERT(n >= SP_RT_FRAME);

    /* Expected RMS of a full-scale 8000-amplitude sine is 8000/sqrt(2)
       ~= 5657; a wide floor of 4000 tolerates real quantisation noise
       while easily excluding a near-silent or badly attenuated start. */
    SP_ASSERT(rms(decoded, SP_RT_FRAME) > 4000.0);

    double corr = pearson_correlation(source, decoded, SP_RT_FRAME);
    SP_ASSERT(corr > 0.85);

    pjmedia_port_destroy(rport);
    pj_pool_release(pool);
}

/* sp_oggopus_duration_ms reads the last page's granule, not a header
 * field -- OggOpus carries no duration field at all. For a whole
 * number of frames the answer is exact: SP_RT_SAMPLES samples at
 * SP_RT_RATE is exactly 1000ms. */
SP_TEST(duration_ms_matches_the_real_audio_fed_in) {
    pj_pool_t *pool = new_pool();
    const char *path = test_path("duration.opus");

    pjmedia_port *wport = NULL;
    SP_ASSERT(sp_oggopus_writer_create(pool, path, SP_RT_RATE, 1, SP_RT_FRAME,
                                       &wport) == SP_OK);
    SP_ASSERT(feed(wport, SP_RT_RATE, SP_RT_SAMPLES) == PJ_SUCCESS);
    pjmedia_port_destroy(wport);

    uint32_t ms = 0;
    SP_ASSERT(sp_oggopus_duration_ms(path, &ms) == SP_OK);
    SP_ASSERT(ms == 1000);

    pj_pool_release(pool);
}

SP_TEST(duration_ms_of_a_missing_file_is_io_error) {
    uint32_t ms = 0;
    SP_ASSERT(sp_oggopus_duration_ms("/nonexistent/nope.opus", &ms) ==
             SP_ERR_IO);
}

/* The bridge's frame size and Opus's legal frame sizes are different
 * things on the read side too, mirroring the writer's own ragged-frame
 * test: get_frame is asked for 100-sample chunks here, none of which
 * is a legal Opus frame size, while the file underneath decodes in
 * 320-sample (20ms) packets. Nothing about serving smaller slices must
 * lose or duplicate samples. */
SP_TEST(get_frame_serves_whatever_size_the_bridge_asks_for) {
    pj_pool_t *pool = new_pool();
    const char *path = test_path("ragged_read.opus");

    pj_int16_t source[SP_RT_SAMPLES];
    fill_tone(source, SP_RT_SAMPLES, SP_RT_RATE);

    pjmedia_port *wport = NULL;
    SP_ASSERT(sp_oggopus_writer_create(pool, path, SP_RT_RATE, 1, SP_RT_FRAME,
                                       &wport) == SP_OK);
    SP_ASSERT(feed(wport, SP_RT_RATE, SP_RT_SAMPLES) == PJ_SUCCESS);
    pjmedia_port_destroy(wport);

    pjmedia_port *rport = NULL;
    SP_ASSERT(sp_oggopus_reader_create(pool, path, SP_RT_RATE, 1, SP_RT_FRAME,
                                       &rport) == SP_OK);

    pj_int16_t decoded[SP_RT_DECODE_CAP];
    unsigned n = 0;
    pj_int16_t fbuf[100];
    pjmedia_frame frame;
    frame.buf = fbuf;
    frame.size = sizeof fbuf;
    frame.timestamp.u64 = 0;
    frame.bit_info = 0;
    for (int iter = 0; iter < 500; iter++) {
        pj_bool_t eof = PJ_FALSE;
        SP_ASSERT(sp_oggopus_reader_is_eof(rport, &eof) == SP_OK);
        if (eof)
            break;
        SP_ASSERT(pjmedia_port_get_frame(rport, &frame) == PJ_SUCCESS);
        if (frame.type == PJMEDIA_FRAME_TYPE_AUDIO) {
            SP_ASSERT(n + 100 <= SP_RT_DECODE_CAP);
            memcpy(decoded + n, fbuf, sizeof fbuf);
            n += 100;
        }
    }

    SP_ASSERT(n == SP_RT_SAMPLES);
    double corr = pearson_correlation(source, decoded, SP_RT_SAMPLES);
    SP_ASSERT(corr > SP_RT_MIN_CORRELATION);

    pjmedia_port_destroy(rport);
    pj_pool_release(pool);
}

/* A file that is not Ogg at all (the container-level twin of
 * playback.c's own detect_format check) is refused rather than read
 * as if it were empty. */
SP_TEST(the_reader_refuses_a_non_ogg_file) {
    pj_pool_t *pool = new_pool();
    const char *path = test_path("not_ogg.opus");

    FILE *f = fopen(path, "wb");
    SP_ASSERT(f != NULL);
    const char junk[] = "not an ogg file at all\n";
    SP_ASSERT(fwrite(junk, 1, sizeof junk, f) == sizeof junk);
    fclose(f);

    pjmedia_port *rport = NULL;
    SP_ASSERT(sp_oggopus_reader_create(pool, path, SP_RT_RATE, 1, SP_RT_FRAME,
                                       &rport) == SP_ERR_MEDIA);
    SP_ASSERT(rport == NULL);

    pj_pool_release(pool);
}

int main(void) {
    pj_init();
    pj_caching_pool_init(&g_cp, NULL, 0);

    SP_RUN(a_written_file_starts_with_an_ogg_page);
    SP_RUN(the_first_packet_is_an_opus_head);
    SP_RUN(the_second_packet_is_opus_tags);
    SP_RUN(the_opus_head_reports_rate_and_a_real_pre_skip);
    SP_RUN(an_empty_stream_ends_with_the_pre_skip_granule);
    SP_RUN(granule_positions_are_in_48k_samples);
    SP_RUN(the_last_page_is_flagged_end_of_stream);
    SP_RUN(a_frame_size_opus_cannot_encode_is_buffered);
    SP_RUN(destroying_the_writer_finalises_the_file);

    SP_RUN(round_trip_decodes_the_tone_and_correlates_with_the_source);
    SP_RUN(the_reader_discards_pre_skip_so_playback_starts_at_time_zero);
    SP_RUN(duration_ms_matches_the_real_audio_fed_in);
    SP_RUN(duration_ms_of_a_missing_file_is_io_error);
    SP_RUN(get_frame_serves_whatever_size_the_bridge_asks_for);
    SP_RUN(the_reader_refuses_a_non_ogg_file);

    pj_caching_pool_destroy(&g_cp);
    return SP_TEST_RESULT();
}
