#include "sp_test.h"
#include "comlink/core.h"
#include "recname.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* extra_yaml is appended after the minimal config every test needs,
   so a case can set a top-level key without a second copy of this
   helper. Written to a fixed path so reopen_core() can find it again
   without rewriting it. */
static const char *cfg_path(void) {
    static char p[512];
    snprintf(p, sizeof p, "%s/rec_test/comlink.yaml", getenv("SP_TEST_TMP"));
    return p;
}

static sp_core *make_core(const char *extra_yaml) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/rec_test", getenv("SP_TEST_TMP"));
    mkdir(dir, 0777);

    FILE *f = fopen(cfg_path(), "w");
    if (!f) return NULL;
    fputs("log_level: 0\nsip_port: 0\naudio_device: none\naccounts: []\n", f);
    if (extra_yaml) fputs(extra_yaml, f);
    fclose(f);

    sp_core *c = NULL;
    if (sp_core_create(cfg_path(), &c) != SP_OK) return NULL;
    return c;
}

static sp_core *reopen_core(void) {
    sp_core *c = NULL;
    if (sp_core_create(cfg_path(), &c) != SP_OK) return NULL;
    return c;
}

/* A separate boot path from make_core() above: proving a recording
   reaches the file needs a real conference bridge slot, which pjsua
   only hands out once a call's media is actually negotiated (see
   pjsua_aud.c) -- a call to nowhere, the way the other call.c tests
   dial, never gets one. The rest of this file gets away with that;
   this one test cannot.

   The fix is the same one verify_tx_mutation.c and tests/verify/
   verify-13-1.sh already use: one pjsua instance calls its own
   listening port, on_incoming_call auto-answers, and the two legs of
   that single call give each other real, negotiated media within the
   same conference bridge -- no second process, no real network peer.
   sip_port has to be a fixed, known value for this rather than 0
   (an OS-assigned ephemeral port) because the dialled URI has to name
   it; chosen away from verify-13-1.sh's 5080 so the two never
   collide if ever run at the same moment. */
#define SP_REC_TEST_SELF_CALL_PORT 58713

static const char *call_cfg_path(void) {
    static char p[512];
    snprintf(p, sizeof p, "%s/rec_call_test/comlink.yaml",
             getenv("SP_TEST_TMP"));
    return p;
}

/* Set by on_self_call_event below, which sp_core_set_event_cb hands
   the core's own dispatch thread -- never this test's thread -- so
   these are atomics, not plain globals. Module-scope rather than
   passed through `user` because sp_call_answer needs the core
   pointer too and one test at a time ever runs, on the same rule
   verify_tx_mutation.c's own file-scope atomics rely on. */
static _Atomic int g_self_call_confirmed;

static void on_self_call_event(const sp_event *ev, void *user) {
    sp_core *core = (sp_core *)user;
    switch (ev->type) {
    case SP_EV_INCOMING_CALL:
        /* The far leg of our own self-dial: answered immediately so
           the near leg's media actually negotiates. */
        sp_call_answer(core, ev->u.incoming.call, 200, NULL, 0);
        break;
    case SP_EV_CALL_STATE:
        if (ev->u.call_state.state == SP_CALL_CONFIRMED)
            atomic_store(&g_self_call_confirmed, 1);
        break;
    default:
        break;
    }
}

/* The callback has to be set before sp_core_start, not after:
   sp_evq_start (core.c) hands the event dispatch thread whatever
   core->cb held at that moment, and does not consult it again --
   setting it post-start silently drops every event, which is not the
   same failure as the call itself failing and took a debugging pass
   here to tell apart. */
static sp_core *make_core_with_call_support(const char *extra_yaml) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/rec_call_test", getenv("SP_TEST_TMP"));
    mkdir(dir, 0777);

    FILE *f = fopen(call_cfg_path(), "w");
    if (!f) return NULL;
    fprintf(f, "log_level: 0\nsip_port: %d\naudio_device: none\n",
           SP_REC_TEST_SELF_CALL_PORT);
    if (extra_yaml) fputs(extra_yaml, f);
    fputs("accounts:\n  - id: a\n    user: 1000\n    domain: 127.0.0.1\n"
          "    register: false\n", f);
    fclose(f);

    sp_core *c = NULL;
    if (sp_core_create(call_cfg_path(), &c) != SP_OK) return NULL;
    sp_core_set_event_cb(c, on_self_call_event, c);
    if (sp_core_start(c) != SP_OK) { sp_core_destroy(c); return NULL; }
    return c;
}

/* Absent from the configuration, the directory is beside it. That is
   what makes this setting portable: on Android the configuration
   lives in the application's private storage and so does this. */
SP_TEST(the_default_directory_sits_beside_the_config) {
    sp_core *core = make_core("");
    SP_ASSERT(core != NULL);
    if (!core) return;
    const char *dir = NULL;
    SP_ASSERT(sp_recording_get_dir(core, &dir) == SP_OK);
    SP_ASSERT(dir != NULL);
    if (dir) {
        SP_ASSERT(dir[0] == '/');
        SP_ASSERT(strstr(dir, "/recordings") != NULL);
    }
    sp_core_destroy(core);
}

SP_TEST(the_default_trace_directory_sits_beside_the_config) {
    sp_core *core = make_core("");
    SP_ASSERT(core != NULL);
    if (!core) return;
    const char *dir = NULL;
    SP_ASSERT(sp_trace_get_export_dir(core, &dir) == SP_OK);
    SP_ASSERT(dir != NULL);
    if (dir) SP_ASSERT(strstr(dir, "/traces") != NULL);
    sp_core_destroy(core);
}

SP_TEST(a_configured_directory_wins) {
    sp_core *core = make_core("recording:\n  dir: /srv/rec\n");
    SP_ASSERT(core != NULL);
    if (!core) return;
    const char *dir = NULL;
    SP_ASSERT(sp_recording_get_dir(core, &dir) == SP_OK);
    SP_ASSERT_STR(dir, "/srv/rec");
    sp_core_destroy(core);
}

/* Setting it makes it, so an operator hears about an unusable choice
   now rather than when they press Record during a call. */
SP_TEST(setting_a_directory_creates_it) {
    sp_core *core = make_core("");
    SP_ASSERT(core != NULL);
    if (!core) return;
    char want[512];
    snprintf(want, sizeof want, "%s/rec_set/deeper", getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_recording_set_dir(core, want) == SP_OK);
    struct stat st;
    SP_ASSERT(stat(want, &st) == 0 && S_ISDIR(st.st_mode));
    const char *back = NULL;
    SP_ASSERT(sp_recording_get_dir(core, &back) == SP_OK);
    SP_ASSERT_STR(back, want);
    sp_core_destroy(core);
}

SP_TEST(a_relative_directory_is_refused) {
    sp_core *core = make_core("");
    SP_ASSERT(core != NULL);
    if (!core) return;
    SP_ASSERT(sp_recording_set_dir(core, "recordings")
              == SP_ERR_INVALID_ARG);
    sp_core_destroy(core);
}

/* Empty restores the default rather than storing an empty string,
   which is what a settings page's Reset button needs. */
SP_TEST(an_empty_directory_restores_the_default) {
    sp_core *core = make_core("recording:\n  dir: /srv/rec\n");
    SP_ASSERT(core != NULL);
    if (!core) return;
    SP_ASSERT(sp_recording_set_dir(core, "") == SP_OK);
    const char *dir = NULL;
    SP_ASSERT(sp_recording_get_dir(core, &dir) == SP_OK);
    SP_ASSERT(dir != NULL);
    if (dir) SP_ASSERT(strstr(dir, "/recordings") != NULL);
    sp_core_destroy(core);
}

SP_TEST(the_format_round_trips) {
    sp_core *core = make_core("");
    SP_ASSERT(core != NULL);
    if (!core) return;
    sp_recording_format f = SP_REC_WAV_ULAW;
    SP_ASSERT(sp_recording_get_format(core, &f) == SP_OK);
    SP_ASSERT(f == SP_REC_WAV);
    SP_ASSERT(sp_recording_set_format(core, SP_REC_WAV_ULAW) == SP_OK);
    SP_ASSERT(sp_recording_get_format(core, &f) == SP_OK);
    SP_ASSERT(f == SP_REC_WAV_ULAW);
    sp_core_destroy(core);
}

/* A setting that does not survive a restart is not a setting. */
SP_TEST(the_format_survives_a_reload) {
    sp_core *core = make_core("");
    SP_ASSERT(core != NULL);
    if (!core) return;
    SP_ASSERT(sp_recording_set_format(core, SP_REC_WAV_ULAW) == SP_OK);
    sp_core_destroy(core);

    sp_core *again = reopen_core();
    SP_ASSERT(again != NULL);
    if (!again) return;
    sp_recording_format f = SP_REC_WAV;
    SP_ASSERT(sp_recording_get_format(again, &f) == SP_OK);
    SP_ASSERT(f == SP_REC_WAV_ULAW);
    sp_core_destroy(again);
}

/* Proves the configured format actually reaches the bytes on disk --
   the equivalent claim for mu-law was never made on the wire (see
   task-3-brief.md) and this closes that gap for Opus while it is
   still a four-byte check.

   Getting there needs a call whose media has actually negotiated (see
   make_core_with_call_support's comment for why and how), which takes
   real wall-clock time -- the polling loop below, not a fixed sleep,
   because how long that negotiation takes is not this test's to
   guess. */
SP_TEST(an_opus_recording_starts_with_an_ogg_page) {
    atomic_store(&g_self_call_confirmed, 0);
    sp_core *core = make_core_with_call_support("recording:\n  format: opus\n");
    SP_ASSERT(core != NULL);
    if (!core) return;
    const sp_account_id acc = 1;   /* first handle issued by sp_core_start */

    char uri[64];
    snprintf(uri, sizeof uri, "sip:1000@127.0.0.1:%d",
            SP_REC_TEST_SELF_CALL_PORT);
    sp_call_id call = 0;
    SP_ASSERT(sp_call_make(core, acc, uri, NULL, 0, &call) == SP_OK);

    for (int i = 0; i < 5000 && !atomic_load(&g_self_call_confirmed); i++)
        usleep(1000);
    SP_ASSERT(atomic_load(&g_self_call_confirmed));

    const char *path = NULL;
    SP_ASSERT(sp_call_record_start_auto(core, call, &path) == SP_OK);
    SP_ASSERT(path != NULL);
    if (path) SP_ASSERT(strstr(path, ".opus") != NULL);

    /* A little real audio time, so more than the two Opus header
       packets exist to be found -- not load-bearing for the OggS
       check itself, but a file recorded for zero seconds is a weaker
       proof that the wiring, not just the writer, works. */
    usleep(300 * 1000);

    /* Stopping -- not merely starting -- is what finalises the file:
       on_destroy flushes the buffered remainder and writes the
       end-of-stream page. Reading before this would only prove the
       header packets, not that teardown actually ran. */
    SP_ASSERT(sp_call_record_stop(core, call) == SP_OK);

    if (path) {
        /* Finalising is not necessarily synchronous with the call
           above returning: the port sp_call_record_start added to
           the bridge picked up a group lock of the bridge's own
           making (pjmedia_conf_add_port, conference.c), and
           pjmedia_port_destroy on a grp_lock'd port only drops this
           side's reference (port.c) -- on_destroy runs once the
           bridge's own reference is dropped too, which happens when
           it processes the queued removal on its own clock, not
           necessarily inside this call stack. Polled for the same
           reason the confirmation wait above is a loop, not a sleep. */
        struct stat pst = {0};
        for (int i = 0; i < 2000; i++) {
            if (stat(path, &pst) == 0 && pst.st_size > 0)
                break;
            usleep(1000);
        }
        SP_ASSERT(pst.st_size > 0);

        FILE *f = fopen(path, "rb");
        SP_ASSERT(f != NULL);
        if (f) {
            unsigned char magic[4] = {0};
            size_t got = fread(magic, 1, sizeof magic, f);
            SP_ASSERT(got == sizeof magic &&
                     memcmp(magic, "OggS", 4) == 0);
            fclose(f);
        }
    }

    sp_call_hangup(core, call, 0);
    sp_core_destroy(core);
}

/* With auto_record on, the call is recording once it is answered and
   its audio is up, without anyone pressing Record: the whole point of
   the setting is the call nobody remembered to record. */
SP_TEST(auto_record_records_an_answered_call_unasked) {
    atomic_store(&g_self_call_confirmed, 0);
    sp_core *core = make_core_with_call_support("auto_record: true\n");
    SP_ASSERT(core != NULL);
    if (!core) return;
    SP_ASSERT(sp_recording_get_auto(core));

    char uri[64];
    snprintf(uri, sizeof uri, "sip:1000@127.0.0.1:%d",
            SP_REC_TEST_SELF_CALL_PORT);
    sp_call_id call = 0;
    SP_ASSERT(sp_call_make(core, 1, uri, NULL, 0, &call) == SP_OK);
    for (int i = 0; i < 5000 && !atomic_load(&g_self_call_confirmed); i++)
        usleep(1000);
    SP_ASSERT(atomic_load(&g_self_call_confirmed));

    /* Media comes up on its own clock after the confirmation, so the
       recording is waited for rather than expected at once. */
    sp_call_info info = {0};
    bool recording = false;
    for (int i = 0; i < 3000 && !recording; i++) {
        if (sp_call_get_info(core, call, &info) == SP_OK)
            recording = info.recording;
        if (!recording)
            usleep(1000);
    }
    SP_ASSERT(recording);

    sp_call_hangup(core, call, 0);
    sp_core_destroy(core);
}

/* Off unless asked for, and asking survives a restart. */
SP_TEST(auto_record_is_off_by_default_and_persists) {
    sp_core *core = make_core("");
    SP_ASSERT(core != NULL);
    if (!core) return;
    SP_ASSERT(!sp_recording_get_auto(core));
    SP_ASSERT(sp_recording_set_auto(core, true) == SP_OK);
    sp_core_destroy(core);

    sp_core *again = reopen_core();
    SP_ASSERT(again != NULL);
    if (!again) return;
    SP_ASSERT(sp_recording_get_auto(again));
    sp_core_destroy(again);
}

/* A setting that does not survive a restart is not a setting -- the
   directory's equivalent of the_format_survives_a_reload above. Uses
   get_dir's resolved slot, same as that test does for the format, so
   a regression in what gets persisted (rather than what gets read
   back before any restart) does not slip past it. */
SP_TEST(the_directory_survives_a_reload) {
    sp_core *core = make_core("");
    SP_ASSERT(core != NULL);
    if (!core) return;
    char want[512];
    snprintf(want, sizeof want, "%s/rec_reload", getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_recording_set_dir(core, want) == SP_OK);
    sp_core_destroy(core);

    sp_core *again = reopen_core();
    SP_ASSERT(again != NULL);
    if (!again) return;
    const char *back = NULL;
    SP_ASSERT(sp_recording_get_dir(again, &back) == SP_OK);
    SP_ASSERT_STR(back, want);
    sp_core_destroy(again);
}

/* The name has to sort by time and stay unique when two calls start
   recording in the same second, which is why the call id is in it. */
SP_TEST(the_generated_name_carries_a_stamp_and_the_call_id) {
    char name[128];
    SP_ASSERT(sp_recording_name(name, sizeof name, 7, SP_REC_WAV) == 0);
    SP_ASSERT(strncmp(name, "call-", 5) == 0);
    SP_ASSERT(strstr(name, "-7.wav") != NULL);
    SP_ASSERT(strlen(name) == strlen("call-YYYYmmdd-HHMMSS-7.wav"));

    char ulaw[128];
    SP_ASSERT(sp_recording_name(ulaw, sizeof ulaw, 7, SP_REC_WAV_ULAW) == 0);
    /* SP_REC_WAV and SP_REC_WAV_ULAW are both WAV containers, so both
       end .wav: a file that claims to be something it is not would be
       worse than a large one. */
    SP_ASSERT(strstr(ulaw, ".wav") != NULL);

    char opus[128];
    SP_ASSERT(sp_recording_name(opus, sizeof opus, 7, SP_REC_OPUS) == 0);
    /* SP_REC_OPUS is an Ogg container and gets its own extension, on
       the same rule. */
    SP_ASSERT(strstr(opus, "-7.opus") != NULL);
}

/* A name too long for its buffer must be refused, not truncated --
   sp_recording_path sizes its own length check off strlen(out) after
   the call, so a silently shortened name here would let a directory
   that does not actually fit pass that check. Unreachable in
   production at the real 128-byte buffer, but the function has to
   fail correctly at any size, not just the one nothing ever hits. */
SP_TEST(a_buffer_too_small_for_the_name_fails_without_truncating) {
    char name[8] = "sentin";
    SP_ASSERT(sp_recording_name(name, sizeof name, 7, SP_REC_WAV) == -1);
    SP_ASSERT(name[0] == '\0');
}

/* sp_call_record_start_auto's rec_path buffer is 512 bytes
   (sp_call_rec.rec_path in core/src/core_internal.h). Pinned here
   rather than derived, since that struct is behind the pjproject
   wall this test binary has no include path for -- the same reason
   sp_recording_path lives in this pjproject-free file at all. */
#define SP_TEST_RECPATH_CAP 512

SP_TEST(the_composed_path_is_the_directory_and_a_wav_name) {
    char path[SP_TEST_RECPATH_CAP];
    SP_ASSERT(sp_recording_path(path, sizeof path, "/srv/rec", 42,
                                SP_REC_WAV) == 0);
    SP_ASSERT(strncmp(path, "/srv/rec/", strlen("/srv/rec/")) == 0);
    SP_ASSERT(strstr(path, ".wav") != NULL);

    char opus_path[SP_TEST_RECPATH_CAP];
    SP_ASSERT(sp_recording_path(opus_path, sizeof opus_path, "/srv/rec", 42,
                                SP_REC_OPUS) == 0);
    SP_ASSERT(strncmp(opus_path, "/srv/rec/", strlen("/srv/rec/")) == 0);
    SP_ASSERT(strstr(opus_path, ".opus") != NULL);
}

/* The failure this guards against: a directory too long to compose
   must be reported, not silently shortened into something that looks
   like a real path. */
SP_TEST(a_directory_too_long_for_the_buffer_fails_without_truncating) {
    char big_dir[SP_TEST_RECPATH_CAP * 2];
    memset(big_dir, 'a', sizeof big_dir - 1);
    big_dir[0] = '/';
    big_dir[sizeof big_dir - 1] = '\0';

    char path[SP_TEST_RECPATH_CAP];
    strcpy(path, "sentinel");
    SP_ASSERT(sp_recording_path(path, sizeof path, big_dir, 1,
                                SP_REC_WAV) == -1);
    /* Not a truncated prefix of big_dir either: left empty, so a
       caller that skips the return value gets nothing usable rather
       than something plausible. */
    SP_ASSERT(path[0] == '\0');
}

/* The off-by-one is the whole point of a length check: a directory
   one byte too long fails, and the same directory one byte shorter
   -- the largest that still fits -- succeeds. Sized against the
   generated name's real length rather than a guess, since the name
   carries a timestamp. */
SP_TEST(the_length_boundary_is_exact) {
    char name[128];
    SP_ASSERT(sp_recording_name(name, sizeof name, 1, SP_REC_WAV) == 0);
    const size_t name_len = strlen(name);
    /* need = dirlen + 1 ('/') + name_len + 1 (NUL); the largest
       dirlen for which need still fits SP_TEST_RECPATH_CAP. */
    const size_t fit_dirlen =
        SP_TEST_RECPATH_CAP - 1 - name_len - 1;

    char dir_fits[SP_TEST_RECPATH_CAP];
    memset(dir_fits, 'd', fit_dirlen);
    dir_fits[0] = '/';
    dir_fits[fit_dirlen] = '\0';

    char path[SP_TEST_RECPATH_CAP];
    SP_ASSERT(sp_recording_path(path, SP_TEST_RECPATH_CAP, dir_fits, 1,
                                SP_REC_WAV) == 0);

    char dir_over[SP_TEST_RECPATH_CAP + 2];
    memset(dir_over, 'd', fit_dirlen + 1);
    dir_over[0] = '/';
    dir_over[fit_dirlen + 1] = '\0';

    SP_ASSERT(sp_recording_path(path, SP_TEST_RECPATH_CAP, dir_over, 1,
                                SP_REC_WAV) == -1);
}

/* The tests above pin sp_recording_path's own length check. This one
   is a different claim: that an operator can actually reach the
   precondition through the public config API -- sp_recording_set_dir
   does not itself refuse a directory this long, only the composition
   does, and sp_path_mkdir_p's own 1024-byte limit is not the same
   limit as rec_path's 512. */
SP_TEST(a_directory_long_enough_to_overflow_the_recording_path_is_legal) {
    sp_core *core = make_core("");
    SP_ASSERT(core != NULL);
    if (!core) return;

    char long_dir[900];
    int off = snprintf(long_dir, sizeof long_dir, "%s/rec_long",
                       getenv("SP_TEST_TMP"));
    SP_ASSERT(off > 0 && (size_t)off < sizeof long_dir);
    /* The unbranched prefix this test's nested directories hang off,
       kept so cleanup below can walk back up to exactly this point
       and no further -- SP_TEST_TMP itself is not ours to remove. */
    char rec_long_dir[900];
    strcpy(rec_long_dir, long_dir);
    /* One segment well under NAME_MAX (255), repeated until the whole
       path clears 512 bytes -- rec_path's size -- while staying
       comfortably under sp_path_mkdir_p's 1024-byte limit. */
    const char *seg = "/dddddddddddddddddddddddddddddddddddddddddddddd";
    while ((size_t)off < sizeof long_dir - strlen(seg) - 1 &&
           off < 700) {
        int n = snprintf(long_dir + off, sizeof long_dir - (size_t)off,
                         "%s", seg);
        SP_ASSERT(n > 0);
        off += n;
    }
    SP_ASSERT(strlen(long_dir) > 512);
    SP_ASSERT(strlen(long_dir) < 1024);

    SP_ASSERT(sp_recording_set_dir(core, long_dir) == SP_OK);
    const char *back = NULL;
    SP_ASSERT(sp_recording_get_dir(core, &back) == SP_OK);
    SP_ASSERT_STR(back, long_dir);
    sp_core_destroy(core);

    /* Unlike the length-boundary tests above, sp_recording_set_dir
       actually creates this one on disk -- an unbranched chain of
       directories, one child per level -- and it is not cleaned up
       between runs the way SP_TEST_TMP itself is. Walk it from the
       leaf back up to rec_long_dir, removing what mkdir_p made, the
       same discipline test_paths.c uses. */
    char cleanup[sizeof long_dir];
    strcpy(cleanup, long_dir);
    while (strcmp(cleanup, rec_long_dir) != 0) {
        (void)rmdir(cleanup);
        char *slash = strrchr(cleanup, '/');
        if (!slash) break;
        *slash = '\0';
    }
    (void)rmdir(cleanup);
}

int main(void) {
    SP_RUN(the_default_directory_sits_beside_the_config);
    SP_RUN(the_default_trace_directory_sits_beside_the_config);
    SP_RUN(a_configured_directory_wins);
    SP_RUN(setting_a_directory_creates_it);
    SP_RUN(a_relative_directory_is_refused);
    SP_RUN(an_empty_directory_restores_the_default);
    SP_RUN(the_format_round_trips);
    SP_RUN(the_format_survives_a_reload);
    SP_RUN(an_opus_recording_starts_with_an_ogg_page);
    SP_RUN(the_directory_survives_a_reload);
    SP_RUN(the_generated_name_carries_a_stamp_and_the_call_id);
    SP_RUN(a_buffer_too_small_for_the_name_fails_without_truncating);
    SP_RUN(the_composed_path_is_the_directory_and_a_wav_name);
    SP_RUN(a_directory_too_long_for_the_buffer_fails_without_truncating);
    SP_RUN(the_length_boundary_is_exact);
    SP_RUN(a_directory_long_enough_to_overflow_the_recording_path_is_legal);
    SP_RUN(auto_record_records_an_answered_call_unasked);
    SP_RUN(auto_record_is_off_by_default_and_persists);
    return SP_TEST_RESULT();
}
