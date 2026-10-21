/*
 * The transcription worker against a real core, with a fake engine:
 * what is stored, what is reported, and that no audio outlives its
 * job whatever happens to it.
 */
#include "sp_test.h"
#include "core_internal.h"
#include "stt.h"
#include "transcriber.h"
#include "transcript.h"

#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static _Atomic int g_calls;
static _Atomic int g_events;
static _Atomic int g_last_state;
static char g_last_detail[256];
static pthread_mutex_t g_detail_lock = PTHREAD_MUTEX_INITIALIZER;

static int fake_transcribe(void *self, const char *model, const float *pcm,
                           size_t n, int threads, const atomic_bool *abort_,
                           sp_stt_segment **out, size_t *n_out,
                           char *err, size_t errlen) {
    (void)self; (void)model; (void)pcm; (void)threads; (void)abort_;
    (void)err; (void)errlen;
    atomic_fetch_add(&g_calls, 1);
    *out = calloc(1, sizeof **out);
    (*out)[0].start_ms = 0;
    (*out)[0].end_ms = (uint32_t)(n / 16);
    (*out)[0].text = strdup("words");
    *n_out = 1;
    return 0;
}
static const sp_stt_engine FAKE = { fake_transcribe, NULL };

/* Holds the worker until told to stop, the way a long call would. */
static int slow_transcribe(void *self, const char *model, const float *pcm,
                           size_t n, int threads, const atomic_bool *abort_,
                           sp_stt_segment **out, size_t *n_out,
                           char *err, size_t errlen) {
    (void)self; (void)model; (void)pcm; (void)n; (void)threads;
    (void)out; (void)n_out;
    atomic_fetch_add(&g_calls, 1);
    for (int i = 0; i < 1000 && !atomic_load(abort_); i++)
        usleep(10000);
    snprintf(err, errlen, "stopped");
    return -1;
}
static const sp_stt_engine SLOW = { slow_transcribe, NULL };

static void on_event(const sp_event *ev, void *user) {
    (void)user;
    if (ev->type != SP_EV_TRANSCRIPT)
        return;
    pthread_mutex_lock(&g_detail_lock);
    snprintf(g_last_detail, sizeof g_last_detail, "%s",
             ev->u.transcript.detail ? ev->u.transcript.detail : "");
    pthread_mutex_unlock(&g_detail_lock);
    atomic_store(&g_last_state, (int)ev->u.transcript.state);
    atomic_fetch_add(&g_events, 1);
}

static char g_dir[512];

static const char *cfg_path(void) {
    static char p[600];
    snprintf(p, sizeof p, "%s/comlink.yaml", g_dir);
    return p;
}

/* Each test in its own directory: the scratch directory sits beside
   the configuration, and startup empties it. */
static void fresh_dir(const char *name) {
    snprintf(g_dir, sizeof g_dir, "%s/tr_%s", getenv("SP_TEST_TMP"), name);
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    SP_ASSERT(system(cmd) == 0);
    mkdir(g_dir, 0700);
}

static void write_cfg(const char *extra) {
    FILE *f = fopen(cfg_path(), "w");
    if (!f)
        return;
    fprintf(f, "log_level: 0\nsip_port: 0\naudio_device: none\n"
               "accounts: []\nmodules:\n  transcript: true\n%s",
            extra ? extra : "");
    fclose(f);
}

static sp_core *boot(void) {
    atomic_store(&g_calls, 0);
    atomic_store(&g_events, 0);
    atomic_store(&g_last_state, -1);
    sp_core *c = NULL;
    if (sp_core_create(cfg_path(), &c) != SP_OK)
        return NULL;
    sp_core_set_event_cb(c, on_event, NULL);
    if (sp_core_start(c) != SP_OK) {
        sp_core_destroy(c);
        return NULL;
    }
    return c;
}

/* One second of something a speech engine would take for a voice. */
static char *track(sp_core *c, const char *leaf, bool silent) {
    const char *dir = NULL;
    if (sp_transcript_get_scratch_dir(c, &dir) != SP_OK || !dir)
        return NULL;
    char *p = malloc(strlen(dir) + strlen(leaf) + 2);
    sprintf(p, "%s/%s", dir, leaf);
    FILE *f = fopen(p, "wb");
    for (int i = 0; f && i < 16000; i++) {
        int16_t s = silent ? 0 : (int16_t)(6000 * sinf((float)i * 0.05f));
        fwrite(&s, 2, 1, f);
    }
    if (f)
        fclose(f);
    return p;
}

static bool gone(const char *p) {
    return access(p, F_OK) != 0;
}

static size_t files_in(const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n;
}

static sp_transcript_state state_of(sp_core *c, int64_t id) {
    const sp_call_record *r = NULL;
    size_t n = 0;
    if (sp_history_read(c, 100, &r, &n) != SP_OK)
        return (sp_transcript_state)-1;
    for (size_t i = 0; i < n; i++)
        if (r[i].id == id)
            return r[i].transcript;
    return (sp_transcript_state)-1;
}

/* The stored reason, as the Calls list reads it. */
static const char *detail_of(sp_core *c, int64_t id) {
    const sp_call_record *r = NULL;
    size_t n = 0;
    if (sp_history_read(c, 100, &r, &n) != SP_OK)
        return NULL;
    for (size_t i = 0; i < n; i++)
        if (r[i].id == id)
            return r[i].transcript_detail;
    return NULL;
}

static bool wait_events(int want, int ms) {
    for (int i = 0; i < ms && atomic_load(&g_events) < want; i += 10)
        usleep(10000);
    return atomic_load(&g_events) >= want;
}

SP_TEST(a_job_stores_both_sides_and_deletes_the_audio) {
    fresh_dir("ok");
    char extra[700];
    snprintf(extra, sizeof extra, "transcript:\n  model: %s\n", cfg_path());
    write_cfg(extra);
    sp_core *c = boot();
    if (!c) { SP_ASSERT(0); return; }
    sp_transcriber_set_engine(c->transcriber, &FAKE);

    int64_t id = sp_history_begin(c->history, true, "a", "sip:1@x",
                                  "sip:2@x", "cid-ok");
    char *far = track(c, "far.pcm", false), *mic = track(c, "mic.pcm", false);
    sp_transcriber_submit(c->transcriber, id, "Alice", far, mic);
    SP_ASSERT(sp_transcriber_wait_idle(c->transcriber, 5000));
    SP_ASSERT(wait_events(1, 2000));

    const sp_transcript_line *l = NULL;
    size_t n = 0;
    SP_ASSERT(sp_transcript_read(c, id, &l, &n) == SP_OK);
    SP_ASSERT(n == 2);
    if (n == 2) {
        SP_ASSERT(l[0].local == false);
        SP_ASSERT_STR(l[0].speaker, "Alice");
        SP_ASSERT(l[1].local == true);
    }
    SP_ASSERT(state_of(c, id) == SP_TRANSCRIPT_READY);
    SP_ASSERT(atomic_load(&g_last_state) == SP_TRANSCRIPT_READY);
    SP_ASSERT(gone(far) && gone(mic));
    free(far); free(mic);
    sp_core_destroy(c);
}

SP_TEST(a_silent_side_is_not_sent_to_the_engine) {
    fresh_dir("silent");
    char extra[700];
    snprintf(extra, sizeof extra, "transcript:\n  model: %s\n", cfg_path());
    write_cfg(extra);
    sp_core *c = boot();
    if (!c) { SP_ASSERT(0); return; }
    sp_transcriber_set_engine(c->transcriber, &FAKE);

    int64_t id = sp_history_begin(c->history, true, "a", "sip:1@x",
                                  "sip:2@x", "cid-s");
    char *far = track(c, "far.pcm", false), *mic = track(c, "mic.pcm", true);
    sp_transcriber_submit(c->transcriber, id, "Alice", far, mic);
    SP_ASSERT(sp_transcriber_wait_idle(c->transcriber, 5000));
    SP_ASSERT(atomic_load(&g_calls) == 1);
    const sp_transcript_line *l = NULL;
    size_t n = 0;
    SP_ASSERT(sp_transcript_read(c, id, &l, &n) == SP_OK);
    SP_ASSERT(n == 1 && !l[0].local);
    free(far); free(mic);
    sp_core_destroy(c);
}

SP_TEST(no_model_fails_with_a_reason) {
    fresh_dir("nomodel");
    write_cfg(NULL);
    sp_core *c = boot();
    if (!c) { SP_ASSERT(0); return; }
    sp_transcriber_set_engine(c->transcriber, &FAKE);

    int64_t id = sp_history_begin(c->history, true, "a", "sip:1@x",
                                  "sip:2@x", "cid-nm");
    char *far = track(c, "far.pcm", false), *mic = track(c, "mic.pcm", false);
    sp_transcriber_submit(c->transcriber, id, "Alice", far, mic);
    SP_ASSERT(sp_transcriber_wait_idle(c->transcriber, 5000));
    SP_ASSERT(wait_events(1, 2000));
    SP_ASSERT(atomic_load(&g_calls) == 0);
    SP_ASSERT(state_of(c, id) == SP_TRANSCRIPT_FAILED);
    SP_ASSERT(atomic_load(&g_last_state) == SP_TRANSCRIPT_FAILED);
    pthread_mutex_lock(&g_detail_lock);
    SP_ASSERT(strstr(g_last_detail, "model") != NULL);
    pthread_mutex_unlock(&g_detail_lock);
    SP_ASSERT(detail_of(c, id) && strstr(detail_of(c, id), "model"));
    SP_ASSERT(gone(far) && gone(mic));
    free(far); free(mic);
    sp_core_destroy(c);
}

SP_TEST(a_module_switched_off_discards_the_job) {
    fresh_dir("off");
    char extra[700];
    snprintf(extra, sizeof extra, "transcript:\n  model: %s\n", cfg_path());
    write_cfg(extra);
    sp_core *c = boot();
    if (!c) { SP_ASSERT(0); return; }
    sp_transcriber_set_engine(c->transcriber, &FAKE);
    SP_ASSERT(sp_module_set(c, "transcript", false) == SP_OK);

    int64_t id = sp_history_begin(c->history, true, "a", "sip:1@x",
                                  "sip:2@x", "cid-off");
    sp_history_transcript_state(c->history, id, SP_TRANSCRIPT_PENDING);
    char *far = track(c, "far.pcm", false), *mic = track(c, "mic.pcm", false);
    sp_transcriber_submit(c->transcriber, id, "Alice", far, mic);
    SP_ASSERT(sp_transcriber_wait_idle(c->transcriber, 5000));
    SP_ASSERT(atomic_load(&g_calls) == 0);
    SP_ASSERT(state_of(c, id) == SP_TRANSCRIPT_NONE);
    SP_ASSERT(gone(far) && gone(mic));
    free(far); free(mic);
    sp_core_destroy(c);
}

/* Named as the capture names them, which is what a restart looks for. */
static char *track_for(sp_core *c, int64_t id, const char *side) {
    char leaf[64];
    snprintf(leaf, sizeof leaf, "%lld-%s.pcm", (long long)id, side);
    return track(c, leaf, false);
}

SP_TEST(a_queue_cut_short_by_quitting_resumes_at_the_next_start) {
    fresh_dir("resume");
    char extra[700];
    snprintf(extra, sizeof extra, "transcript:\n  model: %s\n", cfg_path());
    write_cfg(extra);
    sp_core *c = boot();
    if (!c) { SP_ASSERT(0); return; }
    sp_transcriber_set_engine(c->transcriber, &SLOW);

    int64_t a = sp_history_begin(c->history, true, "a", "sip:1@x",
                                 "sip:2@x", "cid-r1");
    int64_t b = sp_history_begin(c->history, true, "a", "sip:1@x",
                                 "sip:2@x", "cid-r2");
    sp_history_transcript_state(c->history, a, SP_TRANSCRIPT_PENDING);
    sp_history_transcript_state(c->history, b, SP_TRANSCRIPT_PENDING);
    char *fa = track_for(c, a, "far"), *ma = track_for(c, a, "mic");
    char *fb = track_for(c, b, "far"), *mb = track_for(c, b, "mic");
    sp_transcriber_submit(c->transcriber, a, "A", fa, ma);
    sp_transcriber_submit(c->transcriber, b, "B", fb, mb);
    /* a is running, b is waiting, when the operator quits. */
    for (int i = 0; i < 200 && atomic_load(&g_calls) == 0; i++)
        usleep(10000);
    sp_core_destroy(c);

    SP_ASSERT(!gone(fa) && !gone(ma) && !gone(fb) && !gone(mb));

    sp_transcriber_default_engine_for_tests(&FAKE);
    c = boot();
    if (!c) { SP_ASSERT(0); return; }
    SP_ASSERT(wait_events(2, 5000));
    SP_ASSERT(sp_transcriber_wait_idle(c->transcriber, 5000));
    SP_ASSERT(state_of(c, a) == SP_TRANSCRIPT_READY);
    SP_ASSERT(state_of(c, b) == SP_TRANSCRIPT_READY);
    SP_ASSERT(gone(fa) && gone(ma) && gone(fb) && gone(mb));
    sp_core_destroy(c);
    sp_transcriber_default_engine_for_tests(NULL);
    free(fa); free(ma); free(fb); free(mb);
}

SP_TEST(startup_clears_scratch_and_pending) {
    fresh_dir("startup");
    write_cfg(NULL);
    sp_core *c = boot();
    if (!c) { SP_ASSERT(0); return; }
    int64_t id = sp_history_begin(c->history, true, "a", "sip:1@x",
                                  "sip:2@x", "cid-st");
    sp_history_transcript_state(c->history, id, SP_TRANSCRIPT_PENDING);
    const char *dir = NULL;
    SP_ASSERT(sp_transcript_get_scratch_dir(c, &dir) == SP_OK);
    char scratch[600];
    snprintf(scratch, sizeof scratch, "%s", dir ? dir : "");
    sp_core_destroy(c);

    char stray[700];
    snprintf(stray, sizeof stray, "%s/stray.pcm", scratch);
    FILE *f = fopen(stray, "wb");
    if (f) { fputs("x", f); fclose(f); }
    SP_ASSERT(!gone(stray));

    c = boot();
    if (!c) { SP_ASSERT(0); return; }
    SP_ASSERT(gone(stray));
    SP_ASSERT(files_in(scratch) == 0);
    struct stat st;
    SP_ASSERT(stat(scratch, &st) == 0 && (st.st_mode & 077) == 0);
    SP_ASSERT(state_of(c, id) == SP_TRANSCRIPT_FAILED);
    SP_ASSERT(detail_of(c, id) && strstr(detail_of(c, id), "lost"));
    sp_core_destroy(c);
}

SP_TEST(a_second_instance_leaves_the_first_one_s_queue_alone) {
    fresh_dir("second");
    write_cfg(NULL);
    sp_core *a = NULL;
    SP_ASSERT(sp_core_create(cfg_path(), &a) == SP_OK);
    if (!a) return;
    int64_t id = sp_history_begin(a->history, true, "a", "sip:1@x",
                                  "sip:2@x", "cid-2nd");
    sp_history_transcript_state(a->history, id, SP_TRANSCRIPT_PENDING);
    char queued[700];
    snprintf(queued, sizeof queued, "%s/%lld-far.pcm",
             a->transcript_scratch, (long long)id);
    FILE *f = fopen(queued, "wb");
    if (f) { fputs("x", f); fclose(f); }

    /* Same configuration, so the same scratch directory and the same
       database: everything in both is the first instance's work. */
    sp_core *b = NULL;
    SP_ASSERT(sp_core_create(cfg_path(), &b) == SP_OK);
    SP_ASSERT(!gone(queued));
    SP_ASSERT(b && b->transcriber == NULL);
    sp_core_destroy(b);

    const sp_call_record *r = NULL;
    size_t n = 0;
    sp_transcript_state st = (sp_transcript_state)-1;
    if (sp_history_read(a, 100, &r, &n) == SP_OK)
        for (size_t i = 0; i < n; i++)
            if (r[i].id == id)
                st = r[i].transcript;
    SP_ASSERT(st == SP_TRANSCRIPT_PENDING);
    sp_core_destroy(a);
}

SP_TEST(a_state_handed_to_the_worker_is_stored) {
    fresh_dir("setstate");
    write_cfg(NULL);
    sp_core *c = boot();
    if (!c) { SP_ASSERT(0); return; }
    int64_t id = sp_history_begin(c->history, true, "a", "sip:1@x",
                                  "sip:2@x", "cid-ss");
    sp_history_transcript_state(c->history, id, SP_TRANSCRIPT_PENDING);
    sp_transcriber_set_state(c->transcriber, id, SP_TRANSCRIPT_NONE, NULL);
    sp_transcriber_set_state(c->transcriber, id, SP_TRANSCRIPT_FAILED,
                             "the call's audio could not be captured");
    SP_ASSERT(sp_transcriber_wait_idle(c->transcriber, 5000));
    SP_ASSERT(state_of(c, id) == SP_TRANSCRIPT_FAILED);
    SP_ASSERT(detail_of(c, id) && strstr(detail_of(c, id), "captured"));
    sp_core_destroy(c);
}

SP_TEST(the_model_path_survives_a_reload) {
    fresh_dir("model");
    write_cfg(NULL);
    sp_core *c = boot();
    if (!c) { SP_ASSERT(0); return; }
    char m[4096];
    SP_ASSERT(sp_transcript_get_model(c, m, sizeof m) == SP_OK && !*m);
    SP_ASSERT(sp_transcript_set_model(c, cfg_path()) == SP_OK);
    sp_core_destroy(c);

    c = boot();
    if (!c) { SP_ASSERT(0); return; }
    SP_ASSERT(sp_transcript_get_model(c, m, sizeof m) == SP_OK);
    SP_ASSERT_STR(m, cfg_path());
    char small[4];
    SP_ASSERT(sp_transcript_get_model(c, small, sizeof small)
              == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_transcript_set_model(c, "") == SP_OK);
    SP_ASSERT(sp_transcript_get_model(c, m, sizeof m) == SP_OK && !*m);
    sp_core_destroy(c);
}

SP_TEST(a_bad_model_path_is_refused) {
    fresh_dir("badmodel");
    write_cfg(NULL);
    sp_core *c = boot();
    if (!c) { SP_ASSERT(0); return; }
    SP_ASSERT(sp_transcript_set_model(c, "ggml-small.bin")
              == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_transcript_set_model(c, "/nonexistent/ggml-small.bin")
              == SP_ERR_NOT_FOUND);
    char m[4096];
    SP_ASSERT(sp_transcript_get_model(c, m, sizeof m) == SP_OK && !*m);
    sp_core_destroy(c);
}

SP_TEST(a_scratch_symlink_is_refused_not_followed) {
    /* The sweep deletes files. Pointed elsewhere by a symlink, it must
       not delete what is there; it refuses the directory instead and
       the phone runs without transcripts. */
    fresh_dir("symlink");
    write_cfg(NULL);
    char victim[600], keep[700], link[700];
    snprintf(victim, sizeof victim, "%s/elsewhere", g_dir);
    mkdir(victim, 0700);
    snprintf(keep, sizeof keep, "%s/precious.txt", victim);
    FILE *f = fopen(keep, "w");
    if (f) { fputs("x", f); fclose(f); }
    snprintf(link, sizeof link, "%s/transcript-scratch", g_dir);
    SP_ASSERT(symlink(victim, link) == 0);

    sp_core *c = boot();
    if (!c) { SP_ASSERT(0); return; }
    SP_ASSERT(!gone(keep));
    SP_ASSERT(c->transcriber == NULL);
    const char *dir = NULL;
    SP_ASSERT(sp_transcript_get_scratch_dir(c, &dir) != SP_OK);
    sp_core_destroy(c);
    SP_ASSERT(!gone(keep));
}

/* Two segments whatever it is given: one over the first five seconds,
   one from ten to twelve. Stands in for whisper inventing words over
   a noisy stretch and transcribing a real one. */
static int two_transcribe(void *self, const char *model, const float *pcm,
                          size_t n, int threads, const atomic_bool *abort_,
                          sp_stt_segment **out, size_t *n_out,
                          char *err, size_t errlen) {
    (void)self; (void)model; (void)pcm; (void)n; (void)threads;
    (void)abort_; (void)err; (void)errlen;
    atomic_fetch_add(&g_calls, 1);
    *out = calloc(2, sizeof **out);
    (*out)[0] = (sp_stt_segment){ 0, 5000, strdup("invented") };
    (*out)[1] = (sp_stt_segment){ 10000, 12000, strdup("real") };
    *n_out = 2;
    return 0;
}
static const sp_stt_engine TWO = { two_transcribe, NULL };

/* A track of the given level and length, then optionally a tone. */
static char *track_of(sp_core *c, const char *leaf, float noise, int noise_s,
                      int tone_s) {
    const char *dir = NULL;
    if (sp_transcript_get_scratch_dir(c, &dir) != SP_OK || !dir)
        return NULL;
    char *p = malloc(strlen(dir) + strlen(leaf) + 2);
    sprintf(p, "%s/%s", dir, leaf);
    FILE *f = fopen(p, "wb");
    for (int i = 0; f && i < 16000 * noise_s; i++) {
        int16_t v = (int16_t)(((i * 7919) % 2 ? 1 : -1) * noise * 32767);
        fwrite(&v, 2, 1, f);
    }
    for (int i = 0; f && i < 16000 * tone_s; i++) {
        int16_t v = (int16_t)(6000 * sinf((float)i * 0.05f));
        fwrite(&v, 2, 1, f);
    }
    if (f)
        fclose(f);
    return p;
}

SP_TEST(no_job_runs_while_a_call_is_up) {
    fresh_dir("busy");
    char extra[700];
    snprintf(extra, sizeof extra, "transcript:\n  model: %s\n", cfg_path());
    write_cfg(extra);
    sp_core *c = boot();
    if (!c) { SP_ASSERT(0); return; }
    sp_transcriber_set_engine(c->transcriber, &FAKE);
    /* A call, as far as the transcriber can tell. */
    const uint64_t fake = sp_ht_add(c->calls, -1, NULL);
    sp_transcriber_call_started(c->transcriber);

    int64_t id = sp_history_begin(c->history, true, "a", "sip:1@x",
                                  "sip:2@x", "cid-busy");
    char *far = track(c, "far.pcm", false), *mic = track(c, "mic.pcm", false);
    sp_transcriber_submit(c->transcriber, id, "Alice", far, mic);
    usleep(1500 * 1000);
    SP_ASSERT(atomic_load(&g_calls) == 0);
    SP_ASSERT(!gone(far));

    sp_ht_retire(c->calls, fake);
    SP_ASSERT(sp_transcriber_wait_idle(c->transcriber, 5000));
    SP_ASSERT(state_of(c, id) == SP_TRANSCRIPT_READY);
    free(far); free(mic);
    sp_core_destroy(c);
}

SP_TEST(a_call_starting_pauses_the_running_job) {
    fresh_dir("pause");
    char extra[700];
    snprintf(extra, sizeof extra, "transcript:\n  model: %s\n", cfg_path());
    write_cfg(extra);
    sp_core *c = boot();
    if (!c) { SP_ASSERT(0); return; }
    sp_transcriber_set_engine(c->transcriber, &SLOW);

    int64_t id = sp_history_begin(c->history, true, "a", "sip:1@x",
                                  "sip:2@x", "cid-pause");
    sp_history_transcript_state(c->history, id, SP_TRANSCRIPT_PENDING);
    char *far = track(c, "far.pcm", false), *mic = track(c, "mic.pcm", false);
    sp_transcriber_submit(c->transcriber, id, "Alice", far, mic);
    for (int i = 0; i < 300 && atomic_load(&g_calls) == 0; i++)
        usleep(10000);
    SP_ASSERT(atomic_load(&g_calls) == 1);

    const uint64_t fake = sp_ht_add(c->calls, -1, NULL);
    sp_transcriber_call_started(c->transcriber);
    usleep(500 * 1000);
    /* Put back, not thrown away: the audio waits for the call to end. */
    SP_ASSERT(!gone(far) && !gone(mic));
    SP_ASSERT(state_of(c, id) == SP_TRANSCRIPT_PENDING);

    sp_transcriber_set_engine(c->transcriber, &FAKE);
    sp_ht_retire(c->calls, fake);
    SP_ASSERT(wait_events(1, 5000));
    SP_ASSERT(sp_transcriber_wait_idle(c->transcriber, 5000));
    SP_ASSERT(state_of(c, id) == SP_TRANSCRIPT_READY);
    SP_ASSERT(gone(far) && gone(mic));
    free(far); free(mic);
    sp_core_destroy(c);
}

SP_TEST(switching_off_stops_the_running_job) {
    fresh_dir("offrun");
    char extra[700];
    snprintf(extra, sizeof extra, "transcript:\n  model: %s\n", cfg_path());
    write_cfg(extra);
    sp_core *c = boot();
    if (!c) { SP_ASSERT(0); return; }
    sp_transcriber_set_engine(c->transcriber, &SLOW);
    int64_t id = sp_history_begin(c->history, true, "a", "sip:1@x",
                                  "sip:2@x", "cid-offrun");
    sp_history_transcript_state(c->history, id, SP_TRANSCRIPT_PENDING);
    char *far = track(c, "far.pcm", false), *mic = track(c, "mic.pcm", false);
    sp_transcriber_submit(c->transcriber, id, "Alice", far, mic);
    for (int i = 0; i < 300 && atomic_load(&g_calls) == 0; i++)
        usleep(10000);
    SP_ASSERT(sp_module_set(c, "transcript", false) == SP_OK);
    SP_ASSERT(sp_transcriber_wait_idle(c->transcriber, 2000));
    SP_ASSERT(state_of(c, id) == SP_TRANSCRIPT_NONE);
    SP_ASSERT(gone(far) && gone(mic));
    free(far); free(mic);
    sp_core_destroy(c);
}

SP_TEST(a_long_side_is_transcribed_in_chunks) {
    fresh_dir("chunks");
    char extra[700];
    snprintf(extra, sizeof extra, "transcript:\n  model: %s\n", cfg_path());
    write_cfg(extra);
    sp_core *c = boot();
    if (!c) { SP_ASSERT(0); return; }
    sp_transcriber_set_engine(c->transcriber, &FAKE);
    sp_transcriber_set_chunk_samples(c->transcriber, 16000);
    int64_t id = sp_history_begin(c->history, true, "a", "sip:1@x",
                                  "sip:2@x", "cid-chunks");
    char *far = track_of(c, "far.pcm", 0.0f, 0, 3);
    char *mic = track(c, "mic.pcm", true);
    sp_transcriber_submit(c->transcriber, id, "Alice", far, mic);
    SP_ASSERT(sp_transcriber_wait_idle(c->transcriber, 5000));
    SP_ASSERT(atomic_load(&g_calls) == 3);
    const sp_transcript_line *l = NULL;
    size_t n = 0;
    SP_ASSERT(sp_transcript_read(c, id, &l, &n) == SP_OK);
    SP_ASSERT(n == 3);
    if (n == 3) {
        SP_ASSERT(l[0].start_ms == 0);
        SP_ASSERT(l[1].start_ms == 1000);
        SP_ASSERT(l[2].start_ms == 2000);
    }
    free(far); free(mic);
    sp_core_destroy(c);
}

SP_TEST(words_over_line_noise_are_dropped) {
    fresh_dir("noise");
    char extra[700];
    snprintf(extra, sizeof extra, "transcript:\n  model: %s\n", cfg_path());
    write_cfg(extra);
    sp_core *c = boot();
    if (!c) { SP_ASSERT(0); return; }
    sp_transcriber_set_engine(c->transcriber, &TWO);
    int64_t id = sp_history_begin(c->history, true, "a", "sip:1@x",
                                  "sip:2@x", "cid-noise");
    /* Ten seconds of -45 dBFS hiss, then two of something louder. */
    char *far = track_of(c, "far.pcm", 0.0056f, 10, 2);
    char *mic = track(c, "mic.pcm", true);
    sp_transcriber_submit(c->transcriber, id, "Alice", far, mic);
    SP_ASSERT(sp_transcriber_wait_idle(c->transcriber, 5000));
    const sp_transcript_line *l = NULL;
    size_t n = 0;
    SP_ASSERT(sp_transcript_read(c, id, &l, &n) == SP_OK);
    SP_ASSERT(n == 1);
    if (n == 1)
        SP_ASSERT_STR(l[0].text, "real");
    free(far); free(mic);
    sp_core_destroy(c);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(a_job_stores_both_sides_and_deletes_the_audio);
    SP_RUN(a_silent_side_is_not_sent_to_the_engine);
    SP_RUN(no_model_fails_with_a_reason);
    SP_RUN(a_module_switched_off_discards_the_job);
    SP_RUN(a_queue_cut_short_by_quitting_resumes_at_the_next_start);
    SP_RUN(startup_clears_scratch_and_pending);
    SP_RUN(a_second_instance_leaves_the_first_one_s_queue_alone);
    SP_RUN(a_state_handed_to_the_worker_is_stored);
    SP_RUN(the_model_path_survives_a_reload);
    SP_RUN(a_bad_model_path_is_refused);
    SP_RUN(a_scratch_symlink_is_refused_not_followed);
    SP_RUN(no_job_runs_while_a_call_is_up);
    SP_RUN(a_call_starting_pauses_the_running_job);
    SP_RUN(switching_off_stops_the_running_job);
    SP_RUN(a_long_side_is_transcribed_in_chunks);
    SP_RUN(words_over_line_noise_are_dropped);
    return SP_TEST_RESULT();
}
