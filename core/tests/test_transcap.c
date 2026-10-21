/*
 * A real call, captured for its transcript: one pjsua instance dials
 * its own listening port and auto-answers, as test_recording.c does,
 * so the bridge hands out real slots. The engine is a fake; what is
 * under test is the capture's life: when it starts, what it leaves on
 * disk, and what the call row says afterwards.
 */
#include "sp_test.h"
#include "core_internal.h"
#include "stt.h"
#include "transcriber.h"
#include "transcap.h"
#include "transcript.h"

#include <dirent.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Away from test_recording.c's 58713 and verify-13-1.sh's 5080. */
#define SP_TRANSCAP_PORT 58714

static _Atomic int g_confirmed;
static _Atomic int g_media;
static _Atomic int g_ready;
static _Atomic int g_failed;

static int fake_transcribe(void *self, const char *model, const float *pcm,
                           size_t n, int threads, const atomic_bool *abort_,
                           sp_stt_segment **out, size_t *n_out,
                           char *err, size_t errlen) {
    (void)self; (void)model; (void)pcm; (void)threads; (void)abort_;
    (void)err; (void)errlen;
    *out = calloc(1, sizeof **out);
    (*out)[0].start_ms = 0;
    (*out)[0].end_ms = (uint32_t)(n / 16);
    (*out)[0].text = strdup("words");
    *n_out = 1;
    return 0;
}
static const sp_stt_engine FAKE = { fake_transcribe, NULL };

static void on_event(const sp_event *ev, void *user) {
    sp_core *core = user;
    switch (ev->type) {
    case SP_EV_INCOMING_CALL:
        sp_call_answer(core, ev->u.incoming.call, 200, NULL, 0);
        break;
    case SP_EV_CALL_STATE:
        if (ev->u.call_state.state == SP_CALL_CONFIRMED)
            atomic_store(&g_confirmed, 1);
        break;
    case SP_EV_CALL_MEDIA_STATE:
        if (ev->u.media.active)
            atomic_fetch_add(&g_media, 1);
        break;
    case SP_EV_TRANSCRIPT:
        if (ev->u.transcript.state == SP_TRANSCRIPT_READY)
            atomic_fetch_add(&g_ready, 1);
        else if (ev->u.transcript.state == SP_TRANSCRIPT_FAILED)
            atomic_fetch_add(&g_failed, 1);
        break;
    default:
        break;
    }
}

static char g_dir[512];

static sp_core *boot(const char *name, bool with_model) {
    atomic_store(&g_confirmed, 0);
    atomic_store(&g_media, 0);
    atomic_store(&g_ready, 0);
    atomic_store(&g_failed, 0);
    snprintf(g_dir, sizeof g_dir, "%s/tc_%s", getenv("SP_TEST_TMP"), name);
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    SP_ASSERT(system(cmd) == 0);
    mkdir(g_dir, 0700);
    char cfg[600];
    snprintf(cfg, sizeof cfg, "%s/comlink.yaml", g_dir);
    FILE *f = fopen(cfg, "w");
    if (!f)
        return NULL;
    fprintf(f, "log_level: 0\nsip_port: %d\naudio_device: none\n"
               "modules:\n  transcript: true\n", SP_TRANSCAP_PORT);
    if (with_model)
        fprintf(f, "transcript:\n  model: %s\n", cfg);
    fputs("accounts:\n  - id: a\n    user: 1000\n    domain: 127.0.0.1\n"
          "    register: false\n", f);
    fclose(f);

    sp_core *c = NULL;
    if (sp_core_create(cfg, &c) != SP_OK)
        return NULL;
    sp_transcriber_set_engine(c->transcriber, &FAKE);
    sp_core_set_event_cb(c, on_event, c);
    if (sp_core_start(c) != SP_OK) {
        sp_core_destroy(c);
        return NULL;
    }
    return c;
}

static sp_call_id dial_self(sp_core *c) {
    char uri[64];
    snprintf(uri, sizeof uri, "sip:1000@127.0.0.1:%d", SP_TRANSCAP_PORT);
    sp_call_id call = 0;
    if (sp_call_make(c, 1, uri, NULL, 0, &call) != SP_OK)
        return 0;
    for (int i = 0; i < 5000 && !atomic_load(&g_confirmed); i++)
        usleep(1000);
    /* Both legs' media, so both have had their chance to capture. */
    for (int i = 0; i < 3000 && atomic_load(&g_media) < 2; i++)
        usleep(1000);
    return call;
}

static size_t files_in(sp_core *c) {
    const char *dir = NULL;
    if (sp_transcript_get_scratch_dir(c, &dir) != SP_OK || !dir)
        return 0;
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

/* Every row's state, newest first: the self-call writes two, one per
   leg. */
static size_t states(sp_core *c, sp_transcript_state *out, size_t cap) {
    const sp_call_record *r = NULL;
    size_t n = 0;
    if (sp_history_read(c, 10, &r, &n) != SP_OK)
        return 0;
    for (size_t i = 0; i < n && i < cap; i++)
        out[i] = r[i].transcript;
    return n < cap ? n : cap;
}

static bool wait_for(_Atomic int *v, int want, int ms) {
    for (int i = 0; i < ms && atomic_load(v) < want; i += 10)
        usleep(10000);
    return atomic_load(v) >= want;
}

SP_TEST(an_answered_call_is_transcribed_after_hangup) {
    sp_core *c = boot("ok", true);
    if (!c) { SP_ASSERT(0); return; }
    /* No sound device: the legs carry each other's near-silence. */
    sp_transcript_set_silence_rms(0.0f);
    sp_call_id call = dial_self(c);
    SP_ASSERT(call != 0 && atomic_load(&g_confirmed));
    SP_ASSERT(files_in(c) == 4);   /* two legs, two sides each */
    usleep(500 * 1000);
    sp_call_hangup(c, call, 0);

    SP_ASSERT(wait_for(&g_ready, 2, 10000));
    SP_ASSERT(sp_transcriber_wait_idle(c->transcriber, 5000));
    SP_ASSERT(files_in(c) == 0);
    sp_transcript_state st[4];
    const size_t n = states(c, st, 4);
    SP_ASSERT(n == 2);
    for (size_t i = 0; i < n; i++)
        SP_ASSERT(st[i] == SP_TRANSCRIPT_READY);
    sp_transcript_set_silence_rms(SP_SILENCE_RMS);
    sp_core_destroy(c);
}

SP_TEST(switching_the_module_off_mid_call_discards_the_capture) {
    sp_core *c = boot("off", true);
    if (!c) { SP_ASSERT(0); return; }
    sp_call_id call = dial_self(c);
    SP_ASSERT(call != 0);
    SP_ASSERT(files_in(c) == 4);
    SP_ASSERT(sp_module_set(c, "transcript", false) == SP_OK);
    /* Switched off means collection stops now, not at hangup. */
    for (int i = 0; i < 200 && files_in(c) > 0; i++)
        usleep(10000);
    SP_ASSERT(files_in(c) == 0);
    sp_call_hangup(c, call, 0);

    for (int i = 0; i < 200 && files_in(c) > 0; i++)
        usleep(10000);
    SP_ASSERT(sp_transcriber_wait_idle(c->transcriber, 5000));
    SP_ASSERT(files_in(c) == 0);
    SP_ASSERT(atomic_load(&g_ready) == 0);
    sp_transcript_state st[4];
    const size_t n = states(c, st, 4);
    SP_ASSERT(n == 2);
    for (size_t i = 0; i < n; i++)
        SP_ASSERT(st[i] == SP_TRANSCRIPT_NONE);
    sp_core_destroy(c);
}

SP_TEST(no_model_marks_the_call_failed_and_captures_nothing) {
    sp_core *c = boot("nomodel", false);
    if (!c) { SP_ASSERT(0); return; }
    sp_call_id call = dial_self(c);
    SP_ASSERT(call != 0);
    SP_ASSERT(files_in(c) == 0);
    sp_call_hangup(c, call, 0);
    usleep(300 * 1000);
    sp_transcript_state st[4];
    const size_t n = states(c, st, 4);
    SP_ASSERT(n == 2);
    for (size_t i = 0; i < n; i++)
        SP_ASSERT(st[i] == SP_TRANSCRIPT_FAILED);
    sp_core_destroy(c);
}

/* Whether the microphone feeds this call's "Me" side right now. */
static bool mic_feeds(sp_core *c, sp_call_id call) {
    const pjsua_conf_port_id slot = sp_call_capture_mic_slot(c, call);
    if (slot == PJSUA_INVALID_ID)
        return false;
    pjsua_conf_port_info info;
    if (pjsua_conf_get_port_info(0, &info) != PJ_SUCCESS)
        return false;
    for (unsigned i = 0; i < info.listener_cnt; i++)
        if (info.listeners[i] == slot)
            return true;
    return false;
}

static bool wait_mic(sp_core *c, sp_call_id call, bool want) {
    for (int i = 0; i < 300 && mic_feeds(c, call) != want; i++)
        usleep(10000);
    return mic_feeds(c, call) == want;
}

SP_TEST(a_held_call_does_not_hear_the_operator) {
    /* What the operator says to the next caller is not this call's,
       and must not land in its transcript. */
    sp_core *c = boot("hold", true);
    if (!c) { SP_ASSERT(0); return; }
    sp_call_id call = dial_self(c);
    SP_ASSERT(call != 0);
    SP_ASSERT(wait_mic(c, call, true));
    SP_ASSERT(sp_call_set_hold(c, call, true) == SP_OK);
    SP_ASSERT(wait_mic(c, call, false));
    SP_ASSERT(sp_call_set_hold(c, call, false) == SP_OK);
    SP_ASSERT(wait_mic(c, call, true));
    sp_call_hangup(c, call, 0);
    sp_core_destroy(c);
}

SP_TEST(a_build_without_an_engine_captures_nothing) {
    sp_core *c = boot("noengine", true);
    if (!c) { SP_ASSERT(0); return; }
    sp_transcriber_disable_engine(c->transcriber);
    sp_call_id call = dial_self(c);
    SP_ASSERT(call != 0);
    SP_ASSERT(files_in(c) == 0);
    sp_call_hangup(c, call, 0);
    usleep(300 * 1000);
    sp_transcript_state st[4];
    const size_t n = states(c, st, 4);
    SP_ASSERT(n == 2);
    for (size_t i = 0; i < n; i++)
        SP_ASSERT(st[i] == SP_TRANSCRIPT_FAILED);
    sp_core_destroy(c);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(an_answered_call_is_transcribed_after_hangup);
    SP_RUN(switching_the_module_off_mid_call_discards_the_capture);
    SP_RUN(no_model_marks_the_call_failed_and_captures_nothing);
    SP_RUN(a_held_call_does_not_hear_the_operator);
    SP_RUN(a_build_without_an_engine_captures_nothing);
    return SP_TEST_RESULT();
}
