/*
 * sp_playback_*: playing a recording back through the core.
 *
 * Every core here is started with "audio_device: none", exactly as
 * the recording tests are, because there is no sound card in CI.
 * That configuration does NOT stop pjsua_conf_connect from wiring
 * the player's port into slot 0 -- pjsua's null device still accepts
 * the connection, it is simply inaudible -- so these tests do reach
 * pjsua_player_create, pjsua_conf_connect, pjsua_player_get_port,
 * pjsua_player_set_pos and the byte/sample arithmetic in
 * sp_playback_get and sp_playback_seek.
 *
 * What they do NOT reach:
 *   - Anything actually audible: there is no card to check against.
 *   - The master clock genuinely pulling frames over real time. The
 *     null device's conference bridge is not driven by a wall clock
 *     in a way these tests wait on, so a file's position advancing
 *     on its own during PLAYING is not exercised; only that a seek
 *     changes it and a pause holds it still.
 *   - The end-of-file callback actually firing from a file playing
 *     to completion. That needs the clock above. Only the plumbing
 *     (sp_playback_get's flag check and teardown) is exercised, by
 *     the seek-to-end and stop paths -- not the callback itself.
 */
#include "sp_test.h"
#include "comlink/core.h"
#include "oggopus.h"

#include <pjsua-lib/pjsua.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SP_TEST_WAV_FRAMES 1600  /* 100 ms at 16 kHz, 16-bit mono */

static const char *cfg_path(void) {
    static char p[512];
    snprintf(p, sizeof p, "%s/playback_test/comlink.yaml", getenv("SP_TEST_TMP"));
    return p;
}

static sp_core *make_core(void) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/playback_test", getenv("SP_TEST_TMP"));
    mkdir(dir, 0777);

    FILE *f = fopen(cfg_path(), "w");
    if (!f)
        return NULL;
    fputs("log_level: 0\nsip_port: 0\naudio_device: none\n"
          "accounts:\n  - id: a\n    user: 1000\n    domain: 127.0.0.1\n"
          "    register: false\n", f);
    fclose(f);

    sp_core *c = NULL;
    if (sp_core_create(cfg_path(), &c) != SP_OK)
        return NULL;
    if (sp_core_start(c) != SP_OK) {
        sp_core_destroy(c);
        return NULL;
    }
    return c;
}

static void shutdown_core(sp_core *c) {
    sp_core_stop(c);
    sp_core_destroy(c);
}

/* Caller-owned buffer, not a static one: two paths live at once in
   several tests below (e.g. switching_from_opus_to_wav_tears_down_
   the_reader_port), and a shared static buffer made the second call
   silently overwrite the first -- both pointers then named the same
   file, and whichever write happened last is the only one a test
   ever actually exercised. */
static void tmp_path(char *buf, size_t bufsz, const char *name) {
    snprintf(buf, bufsz, "%s/%s", getenv("SP_TEST_TMP"), name);
}

/* A real, minimal WAV: 16-bit PCM, mono, 16 kHz, SP_TEST_WAV_FRAMES
 * frames of silence. pjsua_player_create documents that it only
 * accepts 16-bit PCM mono, so this is not an arbitrary choice.
 *
 * Field by field, because a wrong header produces a confusing
 * pjmedia error rather than a clear one:
 *   [0..3]   "RIFF"
 *   [4..7]   RIFF chunk size = 36 + data size
 *   [8..11]  "WAVE"
 *   [12..15] "fmt "
 *   [16..19] fmt chunk size = 16 (PCM, no extension)
 *   [20..21] format tag = 1 (PCM)
 *   [22..23] channel count = 1
 *   [24..27] sample rate = 16000
 *   [28..31] byte rate = sample_rate * block_align = 32000
 *   [32..33] block align = channels * bits_per_sample/8 = 2
 *   [34..35] bits per sample = 16
 *   [36..39] "data"
 *   [40..43] data chunk size, in bytes
 */
static bool write_wav(const char *path) {
    const uint32_t data_bytes = SP_TEST_WAV_FRAMES * 2;
    const uint32_t riff_size = 36 + data_bytes;
    unsigned char h[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 0x10,0,0,0, 1,0, 1,0,
        0x80,0x3e,0,0, 0,0x7d,0,0, 2,0, 16,0,
        'd','a','t','a', 0,0,0,0
    };
    memcpy(h + 4, &riff_size, sizeof riff_size);
    memcpy(h + 40, &data_bytes, sizeof data_bytes);

    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    unsigned char silence[SP_TEST_WAV_FRAMES * 2] = { 0 };
    bool ok = fwrite(h, 1, sizeof h, f) == sizeof h &&
              fwrite(silence, 1, sizeof silence, f) == sizeof silence;
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
        remove(path);
    return ok;
}

/* Not a WAV at all: no RIFF header, just bytes. pjsua_player_create
   parses the header regardless of the extension, so this is what
   actually exercises "a format this build cannot read", not the
   .opus name by itself. */
static bool write_junk(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    const char junk[] = "not a wav file, just some bytes\n";
    bool ok = fwrite(junk, 1, sizeof junk, f) == sizeof junk;
    if (fclose(f) != 0)
        ok = false;
    return ok;
}

#define SP_TEST_OGG_RATE 16000u
#define SP_TEST_OGG_FRAME 320u /* 20ms @ 16kHz, the recording bridge's own */

/* A real OggOpus file, built the same way call.c's recorder does
   (pjsua_pool_create + sp_oggopus_writer_create + put_frame), not a
   hand-rolled one: exercising sp_playback_start against a file this
   build's own writer would actually produce is a more honest fixture
   than bytes assembled to merely look like one. Needs an already-
   started core (pjsua_pool_create asserts one exists), so callers
   always build this after make_core(). */
static bool write_opus_tone(const char *path, unsigned seconds) {
    pj_pool_t *pool = pjsua_pool_create("test_playback_opus", 4096, 4096);
    if (!pool)
        return false;

    pjmedia_port *port = NULL;
    if (sp_oggopus_writer_create(pool, path, SP_TEST_OGG_RATE, 1,
                                 SP_TEST_OGG_FRAME, &port) != SP_OK) {
        pj_pool_release(pool);
        return false;
    }

    unsigned total = SP_TEST_OGG_RATE * seconds;
    unsigned done = 0;
    bool ok = true;
    while (ok && done < total) {
        pj_int16_t buf[SP_TEST_OGG_RATE];
        unsigned chunk = total - done;
        if (chunk > SP_TEST_OGG_RATE)
            chunk = SP_TEST_OGG_RATE;
        for (unsigned i = 0; i < chunk; i++) {
            double t = (double)(done + i) / (double)SP_TEST_OGG_RATE;
            buf[i] = (pj_int16_t)(8000.0 * sin(2.0 * M_PI * 440.0 * t));
        }
        pjmedia_frame frame;
        frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
        frame.buf = buf;
        frame.size = (pj_size_t)chunk * sizeof(pj_int16_t);
        frame.timestamp.u64 = 0;
        frame.bit_info = 0;
        ok = pjmedia_port_put_frame(port, &frame) == PJ_SUCCESS;
        done += chunk;
    }

    pjmedia_port_destroy(port); /* finalises: EOS page, close */
    pj_pool_release(pool);
    return ok;
}

/* Nothing loaded is a defined state, not an error. A frontend polls
   this on a timer and must not have to special-case a fresh core. */
SP_TEST(a_fresh_core_reports_stopped) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    sp_playback p = { SP_PLAY_PLAYING, 1, 1, "x", true, 2.0f };
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.state == SP_PLAY_STOPPED);
    SP_ASSERT(p.position_ms == 0 && p.duration_ms == 0);
    SP_ASSERT(p.path == NULL);
    SP_ASSERT(p.speed == 1.0f);

    shutdown_core(core);
}

SP_TEST(a_missing_file_is_not_found) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    SP_ASSERT(sp_playback_start(core, "/nonexistent/nope.wav")
              == SP_ERR_NOT_FOUND);

    shutdown_core(core);
}

/* A file that exists but cannot be read is a different problem from
   a missing one -- a permissions or disk issue on an otherwise-fine
   path, per SP_ERR_IO's own doc comment -- and must not be reported
   as SP_ERR_NOT_FOUND, which sends whoever reads it looking for a
   renamed or deleted file instead of a permissions escalation.
   Skipped under root: access() ignores the permission bits for the
   superuser, so chmod 000 would not reproduce the condition. */
SP_TEST(an_unreadable_file_is_an_io_error_not_not_found) {
    if (geteuid() == 0) {
        printf("  SKIP (running as root; access() bypasses the mode bits)\n");
        return;
    }

    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    char path[512];
    tmp_path(path, sizeof path, "unreadable.wav");
    SP_ASSERT(write_wav(path));
    SP_ASSERT(chmod(path, 0000) == 0);

    SP_ASSERT(sp_playback_start(core, path) == SP_ERR_IO);

    chmod(path, 0644);   /* so cleanup of SP_TEST_TMP can remove it */
    shutdown_core(core);
}

SP_TEST(an_empty_path_is_invalid) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    SP_ASSERT(sp_playback_start(core, "") == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_playback_start(core, NULL) == SP_ERR_INVALID_ARG);

    shutdown_core(core);
}

/* The rule this feature is built around. A recording played while a
   call is up reaches the far end through the microphone, so an
   operator who does it by accident has played a previous caller's
   voice down a live line. */
SP_TEST(playing_during_a_call_is_refused) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;
    const sp_account_id acc = 1;   /* first handle issued by sp_core_start */

    char wavpath[512];
    tmp_path(wavpath, sizeof wavpath, "playback_during_call.wav");
    SP_ASSERT(write_wav(wavpath));

    sp_call_id call = 0;
    SP_ASSERT(sp_call_make(core, acc, "sip:1@127.0.0.1:1", NULL, 0, &call)
              == SP_OK);
    SP_ASSERT(sp_playback_start(core, wavpath) == SP_ERR_STATE);
    /* And the reason is one an operator can act on, worded so that
       only this refusal -- not a stale detail left over from some
       earlier call -- could have produced it. */
    SP_ASSERT(strstr(sp_last_error_detail(core), "microphone") != NULL);
    sp_call_hangup(core, call, 0);

    shutdown_core(core);
}

/* The other half of the same safety rule: sp_playback_start refuses
   to begin while a call exists, but that does nothing for playback
   that was already running before the call showed up. on_call_state
   (and on_incoming_call, for an inbound ring) must stop it. */
SP_TEST(a_call_arriving_stops_playback_already_running) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;
    const sp_account_id acc = 1;   /* first handle issued by sp_core_start */

    char wavpath[512];
    tmp_path(wavpath, sizeof wavpath, "playback_then_call.wav");
    SP_ASSERT(write_wav(wavpath));

    SP_ASSERT(sp_playback_start(core, wavpath) == SP_OK);
    sp_playback before;
    SP_ASSERT(sp_playback_get(core, &before) == SP_OK);
    SP_ASSERT(before.state == SP_PLAY_PLAYING);

    /* on_call_state fires synchronously inside sp_call_make -- see
       its own comment in call.c -- so the stop has already happened
       by the time this returns; no wait needed. */
    sp_call_id call = 0;
    SP_ASSERT(sp_call_make(core, acc, "sip:1@127.0.0.1:1", NULL, 0, &call)
              == SP_OK);

    sp_playback after;
    SP_ASSERT(sp_playback_get(core, &after) == SP_OK);
    SP_ASSERT(after.state == SP_PLAY_STOPPED);
    SP_ASSERT(after.path == NULL);

    sp_call_hangup(core, call, 0);
    shutdown_core(core);
}

SP_TEST(pause_and_resume_need_something_loaded) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    SP_ASSERT(sp_playback_pause(core) == SP_ERR_STATE);
    SP_ASSERT(sp_playback_resume(core) == SP_ERR_STATE);

    shutdown_core(core);
}

/* Stopping nothing is not an error: a frontend calls it on close and
   should not have to ask first. */
SP_TEST(stopping_nothing_is_ok) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    SP_ASSERT(sp_playback_stop(core) == SP_OK);

    shutdown_core(core);
}

/* A seek past the end is a seek to the end, not a failure. */
SP_TEST(a_seek_beyond_the_end_clamps) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    char wavpath[512];
    tmp_path(wavpath, sizeof wavpath, "seek_clamp.wav");
    SP_ASSERT(write_wav(wavpath));

    SP_ASSERT(sp_playback_start(core, wavpath) == SP_OK);
    SP_ASSERT(sp_playback_seek(core, 9999999) == SP_OK);
    sp_playback p;
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.position_ms <= p.duration_ms);
    /* Not vacuous in the way it looks: before this fix, clamping to
       exactly the end (rather than one sample short of it) tripped
       an assertion inside pjmedia_wav_player_port_set_pos and aborted
       the whole test process -- see the comment in sp_playback_seek.
       Reaching this line at all is most of what this test checks.
       position_ms itself is not asserted close to duration_ms: on a
       file this short, the whole remaining payload lands in
       pjmedia's read-ahead buffer in one fill, and
       pjmedia_wav_player_port_get_pos reports 0 until a frame is
       actually pulled through get_frame() -- which needs the
       conference bridge's clock running, not exercised here under
       audio_device: none. */
    SP_ASSERT(p.duration_ms > 0);
    sp_playback_stop(core);

    shutdown_core(core);
}

/* A format this build cannot read is refused with a reason naming
   WAV, rather than attempted and failing somewhere deeper. */
SP_TEST(a_non_wav_file_is_refused_naming_wav) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    char opuspath[512];
    tmp_path(opuspath, sizeof opuspath, "junk.opus");
    SP_ASSERT(write_junk(opuspath));

    SP_ASSERT(sp_playback_start(core, opuspath) == SP_ERR_MEDIA);
    SP_ASSERT(strstr(sp_last_error_detail(core), "WAV") != NULL);

    shutdown_core(core);
}

/* The full life cycle on a file that actually opens: PLAYING with a
   real duration and the path handed back, PAUSED, back to PLAYING on
   resume, then STOPPED. "Without losing the position" is not proven
   here the way it reads: under audio_device: none there is no clock
   pulling frames (see the file header), so position_ms is 0 the
   whole way through and paused_pos below is 0 compared to 0. What
   this does prove is that pausing twice is idempotent rather than a
   second error, and that resume and stop leave the state machine in
   the right state. */
SP_TEST(a_loaded_recording_plays_pauses_and_stops) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    char wavpath[512];
    tmp_path(wavpath, sizeof wavpath, "lifecycle.wav");
    SP_ASSERT(write_wav(wavpath));

    SP_ASSERT(sp_playback_start(core, wavpath) == SP_OK);

    sp_playback p;
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.state == SP_PLAY_PLAYING);
    SP_ASSERT(p.duration_ms > 0);
    SP_ASSERT(p.path != NULL && strcmp(p.path, wavpath) == 0);

    SP_ASSERT(sp_playback_seek(core, 50) == SP_OK);
    SP_ASSERT(sp_playback_pause(core) == SP_OK);
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.state == SP_PLAY_PAUSED);
    uint32_t paused_pos = p.position_ms;

    /* Pausing twice is idempotent, not a second error. */
    SP_ASSERT(sp_playback_pause(core) == SP_OK);
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.position_ms == paused_pos);

    SP_ASSERT(sp_playback_resume(core) == SP_OK);
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.state == SP_PLAY_PLAYING);

    SP_ASSERT(sp_playback_stop(core) == SP_OK);
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.state == SP_PLAY_STOPPED);
    SP_ASSERT(p.path == NULL);

    shutdown_core(core);
}

/* Starting a second file stops the first rather than erroring or
   mixing the two. */
SP_TEST(starting_again_stops_the_previous_one) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    char first[512], second[512];
    tmp_path(first, sizeof first, "first.wav");
    tmp_path(second, sizeof second, "second.wav");
    SP_ASSERT(write_wav(first));
    SP_ASSERT(write_wav(second));

    SP_ASSERT(sp_playback_start(core, first) == SP_OK);
    SP_ASSERT(sp_playback_start(core, second) == SP_OK);

    sp_playback p;
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.state == SP_PLAY_PLAYING);
    SP_ASSERT(p.path != NULL && strcmp(p.path, second) == 0);

    sp_playback_stop(core);
    shutdown_core(core);
}

/* --- OggOpus: detection by contents, and the reader's own lifecycle -- */

/* The other half of "detection is by contents, not extension": a real
   Opus file named .wav must still be routed to the Opus reader, not
   rejected or handed to pjsua_player_create where its OggS header
   would simply fail to parse as RIFF. Content wins over name in both
   directions -- the WAV-named-.opus case is covered by junk.opus
   above and by a_loaded_recording_plays_pauses_and_stops's own
   lifecycle test using an actual .wav path. */
SP_TEST(an_opus_file_named_wav_is_still_played_as_opus) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    char path[512];
    tmp_path(path, sizeof path, "mislabeled.wav");
    SP_ASSERT(write_opus_tone(path, 1));

    SP_ASSERT(sp_playback_start(core, path) == SP_OK);
    sp_playback p;
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.state == SP_PLAY_PLAYING);
    /* Duration comes from sp_oggopus_duration_ms's page scan, not the
       WAV byte-rate arithmetic sp_playback_get would use for the
       other kind -- a wrong branch here reports 0 or garbage instead
       of close to 1000ms. */
    SP_ASSERT(p.duration_ms > 900 && p.duration_ms < 1100);
    SP_ASSERT(p.path != NULL && strcmp(p.path, path) == 0);

    sp_playback_stop(core);
    shutdown_core(core);
}

/* The full life cycle for an OggOpus recording, the same shape as
   a_loaded_recording_plays_pauses_and_stops above but through the
   reader port and its own conference-bridge slot instead of pjsua's
   WAV player -- proving destroy_loaded's OggOpus branch (remove from
   the bridge, destroy the port, release the pool) runs cleanly on
   every exit: pause, resume, and finally stop. */
SP_TEST(an_opus_recording_plays_pauses_and_stops) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    char path[512];
    tmp_path(path, sizeof path, "opus_lifecycle.opus");
    SP_ASSERT(write_opus_tone(path, 1));

    SP_ASSERT(sp_playback_start(core, path) == SP_OK);

    sp_playback p;
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.state == SP_PLAY_PLAYING);
    SP_ASSERT(p.duration_ms > 900 && p.duration_ms < 1100);
    SP_ASSERT(p.path != NULL && strcmp(p.path, path) == 0);

    SP_ASSERT(sp_playback_pause(core) == SP_OK);
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.state == SP_PLAY_PAUSED);

    /* Pausing twice is idempotent, not a second error -- same rule as
       the WAV path. */
    SP_ASSERT(sp_playback_pause(core) == SP_OK);

    SP_ASSERT(sp_playback_resume(core) == SP_OK);
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.state == SP_PLAY_PLAYING);

    SP_ASSERT(sp_playback_stop(core) == SP_OK);
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.state == SP_PLAY_STOPPED);
    SP_ASSERT(p.path == NULL);

    shutdown_core(core);
}

/* Sample-accurate seeking inside compressed Opus is deliberately not
   implemented yet (see sp_playback_seek's own comment) -- refused
   cleanly rather than silently mis-seeking or corrupting the decode
   state. Play/pause/stop already proved unaffected above. */
SP_TEST(seeking_an_opus_recording_is_refused_cleanly) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    char path[512];
    tmp_path(path, sizeof path, "no_seek.opus");
    SP_ASSERT(write_opus_tone(path, 1));

    SP_ASSERT(sp_playback_start(core, path) == SP_OK);
    SP_ASSERT(sp_playback_seek(core, 500) == SP_ERR_MEDIA);
    SP_ASSERT(strstr(sp_last_error_detail(core), "Opus") != NULL);

    /* Refused, not broken: still playing, unaffected. */
    sp_playback p;
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.state == SP_PLAY_PLAYING);

    sp_playback_stop(core);
    shutdown_core(core);
}

/* sp_playback.seekable is what a frontend ANDs into its slider's
   setEnabled, precisely because sp_playback_seek's own SP_ERR_MEDIA
   refusal above is not by itself enough: a live, enabled slider that
   snaps back on every drag teaches an operator to ignore it (and the
   Qt frontend's activity log is not always visible to say why). False
   for OggOpus, true for WAV -- decided by kind, never by extension. */
SP_TEST(seekable_is_false_for_opus_and_true_for_wav) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    char opuspath[512];
    tmp_path(opuspath, sizeof opuspath, "seekable_check.opus");
    SP_ASSERT(write_opus_tone(opuspath, 1));

    SP_ASSERT(sp_playback_start(core, opuspath) == SP_OK);
    sp_playback p;
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.seekable == false);

    char wavpath[512];
    tmp_path(wavpath, sizeof wavpath, "seekable_check.wav");
    SP_ASSERT(write_wav(wavpath));

    SP_ASSERT(sp_playback_start(core, wavpath) == SP_OK);
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.seekable == true);

    sp_playback_stop(core);
    shutdown_core(core);
}

/* Crossing formats exercises the part starting_again_stops_the_
   previous_one does not: destroy_loaded switching on the OLD kind
   while the new one being installed is a different kind entirely.
   A bug that assumed both were always the same kind (e.g. reusing
   the WAV teardown path on an OggOpus port, or vice versa) would
   leak the port and its pool, or call pjsua_player_destroy on an
   ID that was never a pjsua player. */
SP_TEST(switching_from_opus_to_wav_tears_down_the_reader_port) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    char opuspath[512], wavpath[512];
    tmp_path(opuspath, sizeof opuspath, "switch_from.opus");
    tmp_path(wavpath, sizeof wavpath, "switch_to.wav");
    SP_ASSERT(write_opus_tone(opuspath, 1));
    SP_ASSERT(write_wav(wavpath));

    SP_ASSERT(sp_playback_start(core, opuspath) == SP_OK);
    SP_ASSERT(sp_playback_start(core, wavpath) == SP_OK);

    sp_playback p;
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.state == SP_PLAY_PLAYING);
    SP_ASSERT(p.path != NULL && strcmp(p.path, wavpath) == 0);

    sp_playback_stop(core);
    shutdown_core(core);
}

/* The other direction: the WAV-vs-call test above already proves an
   arriving call stops a WAV player mid-play; this is the same rule
   reached through the OggOpus branch of destroy_loaded instead. */
SP_TEST(a_call_arriving_stops_an_opus_recording_already_running) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;
    const sp_account_id acc = 1;   /* first handle issued by sp_core_start */

    char path[512];
    tmp_path(path, sizeof path, "opus_then_call.opus");
    SP_ASSERT(write_opus_tone(path, 1));

    SP_ASSERT(sp_playback_start(core, path) == SP_OK);
    sp_playback before;
    SP_ASSERT(sp_playback_get(core, &before) == SP_OK);
    SP_ASSERT(before.state == SP_PLAY_PLAYING);

    sp_call_id call = 0;
    SP_ASSERT(sp_call_make(core, acc, "sip:1@127.0.0.1:1", NULL, 0, &call)
              == SP_OK);

    sp_playback after;
    SP_ASSERT(sp_playback_get(core, &after) == SP_OK);
    SP_ASSERT(after.state == SP_PLAY_STOPPED);
    SP_ASSERT(after.path == NULL);

    sp_call_hangup(core, call, 0);
    shutdown_core(core);
}

/* --- speed: the wrapper (speedport.c) is unit-tested on its own in
 * test_speedport.c against a synthetic source with no core involved.
 * What belongs here instead is the core-level contract:
 * sp_playback_set_speed's own validation, and task 2's own point 6 --
 * that the numbers sp_playback_get reports describe the recording's
 * own timeline, unaffected by the speed it is played back at. */

SP_TEST(set_speed_validates_input_and_needs_something_loaded) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    SP_ASSERT(sp_playback_set_speed(core, 2.0f) == SP_ERR_STATE);

    char wavpath[512];
    tmp_path(wavpath, sizeof wavpath, "speed_validate.wav");
    SP_ASSERT(write_wav(wavpath));
    SP_ASSERT(sp_playback_start(core, wavpath) == SP_OK);

    SP_ASSERT(sp_playback_set_speed(core, 1.5f) == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_playback_set_speed(core, 0.0f) == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_playback_set_speed(core, 0.5f) == SP_OK);

    sp_playback p;
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.speed == 0.5f);

    sp_playback_stop(core);
    shutdown_core(core);
}

/* Duration comes from the WAV port's own byte length (see
 * sp_playback_get's own comment on reading the source, not the speed
 * wrapper, for position and duration alike), so this is a plain
 * equality -- not one that depends on any frame actually having been
 * pulled through the wrapper, which nothing in this no-sound-card test
 * environment does on its own (see this file's own header comment). */
SP_TEST(a_2x_playback_still_reports_the_same_duration) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    char wavpath[512];
    tmp_path(wavpath, sizeof wavpath, "speed_duration.wav");
    SP_ASSERT(write_wav(wavpath));
    SP_ASSERT(sp_playback_start(core, wavpath) == SP_OK);

    sp_playback p;
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.duration_ms > 0);
    SP_ASSERT(p.speed == 1.0f);
    uint32_t duration_at_1x = p.duration_ms;

    SP_ASSERT(sp_playback_set_speed(core, 2.0f) == SP_OK);
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.duration_ms == duration_at_1x);
    SP_ASSERT(p.speed == 2.0f);

    sp_playback_stop(core);
    shutdown_core(core);
}

/* sp_playback_seek resets the speed wrapper's own WSOLA state (see
 * its own comment and speedport.h's sp_speedport_reset) so a seek
 * while sped up does not splice stale audio onto the new position.
 * That reset itself is exercised for real by this test -- speed_port
 * is non-NULL here, so sp_playback_seek's call into
 * sp_speedport_reset is on the live code path, not skipped -- but
 * what it actually does to the audio is not observable under
 * audio_device: none (see this file's own header comment): nothing
 * pulls frames through the wrapper's get_frame() on its own here, and
 * sp_speedport_reset's own effect (dropping queued/synthesised WSOLA
 * state) is unit-tested directly, with no core involved, in
 * test_speedport.c's reset_drops_the_pending_synthesised_half_speed_
 * frame. What this test proves at the core level is the wiring: a
 * seek while sped up still succeeds, on both the WAV and Opus-refusal
 * paths, and leaves the speed exactly as it was -- the reset touches
 * the wrapper's internal WSOLA state only, never core->playback.speed
 * or the speed wrapper's active_speed. */
SP_TEST(seeking_while_sped_up_still_succeeds_and_keeps_the_speed) {
    sp_core *core = make_core();
    SP_ASSERT(core != NULL); if (!core) return;

    char wavpath[512];
    tmp_path(wavpath, sizeof wavpath, "seek_while_sped_up.wav");
    SP_ASSERT(write_wav(wavpath));
    SP_ASSERT(sp_playback_start(core, wavpath) == SP_OK);
    SP_ASSERT(sp_playback_set_speed(core, 2.0f) == SP_OK);

    SP_ASSERT(sp_playback_seek(core, 20) == SP_OK);

    sp_playback p;
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.speed == 2.0f);

    /* 0.5x too, same file, same expectation. */
    SP_ASSERT(sp_playback_set_speed(core, 0.5f) == SP_OK);
    SP_ASSERT(sp_playback_seek(core, 10) == SP_OK);
    SP_ASSERT(sp_playback_get(core, &p) == SP_OK);
    SP_ASSERT(p.speed == 0.5f);

    sp_playback_stop(core);
    shutdown_core(core);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(a_fresh_core_reports_stopped);
    SP_RUN(a_missing_file_is_not_found);
    SP_RUN(an_unreadable_file_is_an_io_error_not_not_found);
    SP_RUN(an_empty_path_is_invalid);
    SP_RUN(playing_during_a_call_is_refused);
    SP_RUN(a_call_arriving_stops_playback_already_running);
    SP_RUN(pause_and_resume_need_something_loaded);
    SP_RUN(stopping_nothing_is_ok);
    SP_RUN(a_seek_beyond_the_end_clamps);
    SP_RUN(a_non_wav_file_is_refused_naming_wav);
    SP_RUN(a_loaded_recording_plays_pauses_and_stops);
    SP_RUN(starting_again_stops_the_previous_one);
    SP_RUN(an_opus_file_named_wav_is_still_played_as_opus);
    SP_RUN(an_opus_recording_plays_pauses_and_stops);
    SP_RUN(seeking_an_opus_recording_is_refused_cleanly);
    SP_RUN(seekable_is_false_for_opus_and_true_for_wav);
    SP_RUN(switching_from_opus_to_wav_tears_down_the_reader_port);
    SP_RUN(a_call_arriving_stops_an_opus_recording_already_running);
    SP_RUN(set_speed_validates_input_and_needs_something_loaded);
    SP_RUN(a_2x_playback_still_reports_the_same_duration);
    SP_RUN(seeking_while_sped_up_still_succeeds_and_keeps_the_speed);
    return SP_TEST_RESULT();
}
