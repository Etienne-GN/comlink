#include "sp_test.h"
#include "sound.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A minimal but real WAV: 16-bit PCM, mono, 16 kHz, one frame of
   silence. Written rather than fabricated as bytes in a comment,
   because the import path checks the extension and the copy, and a
   test that fed it nonsense would not be testing the same thing. */
static void write_wav(const char *path) {
    unsigned char h[44] = {
        'R','I','F','F', 0x28,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 0x10,0,0,0, 1,0, 1,0,
        0x80,0x3e,0,0, 0,0x7d,0,0, 2,0, 16,0,
        'd','a','t','a', 4,0,0,0
    };
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(h, 1, sizeof h, f);
    const unsigned char silence[4] = {0, 0, 0, 0};
    fwrite(silence, 1, sizeof silence, f);
    fclose(f);
}

static const char *tmp(const char *name) {
    static char p[512];
    snprintf(p, sizeof p, "%s/%s", getenv("SP_TEST_TMP"), name);
    return p;
}

/* Deletes every wav in a directory, so a run starts from a known
   state. The import cases add files, and without this the second run
   of the suite counts the first run's leftovers and the "one more
   than before" assertion fails -- which is exactly what happened. */
static void clear_wavs(const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".wav") != 0)
            continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        remove(p);
    }
    closedir(d);
}

/* A bundled tree and a config path whose directory is the user root. */
static void build_tree(char *bundled, size_t bcap, char *cfg, size_t ccap) {
    snprintf(bundled, bcap, "%s/snd-bundled", getenv("SP_TEST_TMP"));
    snprintf(cfg, ccap, "%s/snd-user/comlink.yaml", getenv("SP_TEST_TMP"));

    char d[768];
    snprintf(d, sizeof d, "%s/inbound", bundled);
    mkdir(bundled, 0700);
    mkdir(d, 0700);
    char f[1024];
    snprintf(f, sizeof f, "%s/bundled-one.wav", d);
    write_wav(f);
    snprintf(f, sizeof f, "%s/shared-name.wav", d);
    write_wav(f);
    /* An mp3 alongside them: it must not be offered, because the media
       stack cannot play it. */
    snprintf(f, sizeof f, "%s/not-playable.mp3", d);
    FILE *m = fopen(f, "wb");
    if (m) { fputs("not audio", m); fclose(m); }

    snprintf(d, sizeof d, "%s/snd-user", getenv("SP_TEST_TMP"));
    mkdir(d, 0700);
    snprintf(d, sizeof d, "%s/snd-user/sounds", getenv("SP_TEST_TMP"));
    mkdir(d, 0700);
    snprintf(d, sizeof d, "%s/snd-user/sounds/inbound", getenv("SP_TEST_TMP"));
    mkdir(d, 0700);
    clear_wavs(d);
    snprintf(f, sizeof f, "%s/shared-name.wav", d);
    write_wav(f);
    snprintf(f, sizeof f, "%s/imported-one.wav", d);
    write_wav(f);

    /* The other two slots start empty, so the import cases count from
       a known state however many times the suite has run. */
    snprintf(d, sizeof d, "%s/snd-user/sounds/outbound",
             getenv("SP_TEST_TMP"));
    mkdir(d, 0700);
    clear_wavs(d);
    snprintf(d, sizeof d, "%s/snd-user/sounds/notifications",
             getenv("SP_TEST_TMP"));
    mkdir(d, 0700);
    clear_wavs(d);
}

SP_TEST(slot_directories_match_the_shipped_layout) {
    SP_ASSERT_STR(sp_sound_slot_dir(SP_SOUND_RINGTONE), "inbound");
    SP_ASSERT_STR(sp_sound_slot_dir(SP_SOUND_RINGBACK), "outbound");
    SP_ASSERT_STR(sp_sound_slot_dir(SP_SOUND_NOTIFICATION), "notifications");
}

SP_TEST(both_directories_are_scanned_and_only_wavs_are_offered) {
    char bundled[512], cfg[512];
    build_tree(bundled, sizeof bundled, cfg, sizeof cfg);
    setenv("COMLINK_SOUND_DIR", bundled, 1);

    sp_sound_lib *lib = sp_sound_lib_create(cfg);
    SP_ASSERT(lib != NULL);
    if (!lib) return;

    const sp_sound_file *f = NULL;
    size_t n = 0;
    SP_ASSERT(sp_sound_lib_scan(lib, SP_SOUND_RINGTONE, &f, &n) == SP_OK);
    /* bundled-one, imported-one and one shared-name: three, not four,
       and the mp3 is not among them. */
    SP_ASSERT(n == 3);
    bool saw_mp3 = false, saw_bundled = false, saw_imported = false;
    for (size_t i = 0; i < n; i++) {
        if (strstr(f[i].name, "not-playable"))
            saw_mp3 = true;
        if (strcmp(f[i].name, "bundled-one") == 0)
            saw_bundled = true;
        if (strcmp(f[i].name, "imported-one") == 0)
            saw_imported = true;
    }
    SP_ASSERT(saw_mp3 == false);
    SP_ASSERT(saw_bundled);
    SP_ASSERT(saw_imported);
    sp_sound_lib_destroy(lib);
}

SP_TEST(an_imported_name_shadows_a_bundled_one) {
    char bundled[512], cfg[512];
    build_tree(bundled, sizeof bundled, cfg, sizeof cfg);
    setenv("COMLINK_SOUND_DIR", bundled, 1);

    sp_sound_lib *lib = sp_sound_lib_create(cfg);
    if (!lib) return;
    const sp_sound_file *f = NULL;
    size_t n = 0;
    SP_ASSERT(sp_sound_lib_scan(lib, SP_SOUND_RINGTONE, &f, &n) == SP_OK);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(f[i].name, "shared-name") != 0)
            continue;
        /* The operator's copy wins, so an import can replace a bundled
           sound without touching the package. */
        SP_ASSERT(f[i].bundled == false);
        SP_ASSERT(strstr(f[i].path, "snd-user") != NULL);
    }
    sp_sound_lib_destroy(lib);
}

SP_TEST(resolve_tells_silence_apart_from_a_missing_name) {
    char bundled[512], cfg[512];
    build_tree(bundled, sizeof bundled, cfg, sizeof cfg);
    setenv("COMLINK_SOUND_DIR", bundled, 1);
    sp_sound_lib *lib = sp_sound_lib_create(cfg);
    if (!lib) return;

    bool silent = true;
    /* A name that exists resolves to a readable path. */
    const char *p = sp_sound_lib_resolve(lib, SP_SOUND_RINGTONE,
                                         "bundled-one", &silent);
    SP_ASSERT(p != NULL);
    SP_ASSERT(silent == false);
    SP_ASSERT(access(p, R_OK) == 0);

    /* "none" is silence, which is a setting. */
    silent = false;
    SP_ASSERT(sp_sound_lib_resolve(lib, SP_SOUND_RINGTONE, "none",
                                   &silent) == NULL);
    SP_ASSERT(silent == true);

    /* An empty value is the same. */
    silent = false;
    SP_ASSERT(sp_sound_lib_resolve(lib, SP_SOUND_RINGTONE, "",
                                   &silent) == NULL);
    SP_ASSERT(silent == true);

    /* A name that does not exist is NOT silence: the caller has to be
       able to tell a typo from a deliberate choice. */
    silent = true;
    SP_ASSERT(sp_sound_lib_resolve(lib, SP_SOUND_RINGTONE, "no-such",
                                   &silent) == NULL);
    SP_ASSERT(silent == false);

    /* An absolute path that does not exist fails rather than falling
       back to a bundled sound, so a bad path in the config is visible. */
    silent = true;
    SP_ASSERT(sp_sound_lib_resolve(lib, SP_SOUND_RINGTONE,
                                   "/nonexistent/x.wav", &silent) == NULL);
    SP_ASSERT(silent == false);
    sp_sound_lib_destroy(lib);
}

SP_TEST(import_copies_a_wav_and_refuses_anything_else) {
    char bundled[512], cfg[512];
    build_tree(bundled, sizeof bundled, cfg, sizeof cfg);
    setenv("COMLINK_SOUND_DIR", bundled, 1);
    sp_sound_lib *lib = sp_sound_lib_create(cfg);
    if (!lib) return;

    const char *src = tmp("to-import.wav");
    write_wav(src);

    char err[512] = {0};
    const char *stored = NULL;
    SP_ASSERT(sp_sound_lib_import(lib, SP_SOUND_NOTIFICATION, src,
                                  &stored, err, sizeof err) == SP_OK);
    SP_ASSERT(stored != NULL);
    if (stored) {
        SP_ASSERT(access(stored, R_OK) == 0);
        /* Landed in the operator's tree, in the slot's directory. */
        SP_ASSERT(strstr(stored, "snd-user") != NULL);
        SP_ASSERT(strstr(stored, "notifications") != NULL);
    }

    /* Not a WAV: refused with a reason, and nothing copied. */
    const char *bad = tmp("to-import.mp3");
    FILE *f = fopen(bad, "wb");
    if (f) { fputs("x", f); fclose(f); }
    err[0] = '\0';
    SP_ASSERT(sp_sound_lib_import(lib, SP_SOUND_NOTIFICATION, bad,
                                  &stored, err, sizeof err) != SP_OK);
    SP_ASSERT(strstr(err, "WAV") != NULL);

    /* A source that is not there at all. */
    err[0] = '\0';
    SP_ASSERT(sp_sound_lib_import(lib, SP_SOUND_NOTIFICATION,
                                  tmp("absent.wav"), &stored,
                                  err, sizeof err) != SP_OK);
    SP_ASSERT(err[0] != '\0');
    sp_sound_lib_destroy(lib);
}

SP_TEST(an_import_becomes_visible_to_the_next_scan) {
    char bundled[512], cfg[512];
    build_tree(bundled, sizeof bundled, cfg, sizeof cfg);
    setenv("COMLINK_SOUND_DIR", bundled, 1);
    sp_sound_lib *lib = sp_sound_lib_create(cfg);
    if (!lib) return;

    const sp_sound_file *f = NULL;
    size_t before = 0, after = 0;
    sp_sound_lib_scan(lib, SP_SOUND_RINGBACK, &f, &before);

    const char *src = tmp("fresh-ringback.wav");
    write_wav(src);
    char err[512] = {0};
    SP_ASSERT(sp_sound_lib_import(lib, SP_SOUND_RINGBACK, src, NULL,
                                  err, sizeof err) == SP_OK);

    sp_sound_lib_scan(lib, SP_SOUND_RINGBACK, &f, &after);
    SP_ASSERT(after == before + 1);
    /* And it resolves, which is what the frontend does next. */
    bool silent = true;
    SP_ASSERT(sp_sound_lib_resolve(lib, SP_SOUND_RINGBACK,
                                   "fresh-ringback", &silent) != NULL);
    sp_sound_lib_destroy(lib);
}

SP_TEST(null_and_bad_slots_do_not_crash) {
    const sp_sound_file *f = NULL;
    size_t n = 99;
    SP_ASSERT(sp_sound_lib_scan(NULL, SP_SOUND_RINGTONE, &f, &n) == SP_OK);
    SP_ASSERT(n == 0);
    bool silent = true;
    SP_ASSERT(sp_sound_lib_resolve(NULL, SP_SOUND_RINGTONE, "x",
                                   &silent) == NULL);
    char err[64] = {0};
    SP_ASSERT(sp_sound_lib_import(NULL, SP_SOUND_RINGTONE, "x.wav", NULL,
                                  err, sizeof err) == SP_ERR_INVALID_ARG);
    sp_sound_lib_destroy(NULL);
    SP_ASSERT(1);
}

SP_TEST(the_list_survives_a_default_lookup) {
    /* The crash this covers: a frontend lists the files, then asks
       which one is selected, then walks the list. Asking used to
       rescan the slot, freeing the very array it had just handed out.
       The CLI never saw it because it happens to ask first; the Sounds
       dialog asked second and died.
     *
     * Run under valgrind for this to detect the regression: freed
     * memory usually still holds the old bytes, so the comparisons
     * below would pass by luck. The assertions document the contract;
     * valgrind is what enforces it. */
    char bundled[512], cfg[512];
    build_tree(bundled, sizeof bundled, cfg, sizeof cfg);
    setenv("COMLINK_SOUND_DIR", bundled, 1);
    sp_sound_lib *lib = sp_sound_lib_create(cfg);
    if (!lib) return;

    const sp_sound_file *f = NULL;
    size_t n = 0;
    SP_ASSERT(sp_sound_lib_scan(lib, SP_SOUND_RINGTONE, &f, &n) == SP_OK);
    SP_ASSERT(n > 0);
    if (n == 0) { sp_sound_lib_destroy(lib); return; }

    /* Copy what the list says, before anything else runs. */
    char first[256];
    snprintf(first, sizeof first, "%s", f[0].name);

    /* The default lookup, which is what sp_sound_get does when the
       slot is unset -- the case the operator was in. */
    const char *def = sp_sound_lib_default(lib, SP_SOUND_RINGTONE);
    SP_ASSERT(def != NULL);

    /* And a resolve, which is what sp_sound_set does. */
    bool silent = false;
    SP_ASSERT(sp_sound_lib_resolve(lib, SP_SOUND_RINGTONE, first,
                                   &silent) != NULL);

    /* The list handed out earlier is still the list. */
    SP_ASSERT_STR(f[0].name, first);
    for (size_t i = 0; i < n; i++) {
        SP_ASSERT(f[i].name != NULL);
        SP_ASSERT(f[i].path != NULL);
        SP_ASSERT(f[i].name[0] != '\0');
    }
    sp_sound_lib_destroy(lib);
}

SP_TEST(the_notification_default_prefers_a_busy_tone) {
    /* Alphabetical order would pick the dial tone, which is the wrong
       sound for "the call was not answered". */
    char bundled[512], cfg[512];
    build_tree(bundled, sizeof bundled, cfg, sizeof cfg);

    char d[768];
    snprintf(d, sizeof d, "%s/notifications", bundled);
    mkdir(d, 0700);
    clear_wavs(d);
    char f[1024];
    snprintf(f, sizeof f, "%s/a-dial-tone.wav", d);
    write_wav(f);
    snprintf(f, sizeof f, "%s/z-analog_busy_tone.wav", d);
    write_wav(f);
    setenv("COMLINK_SOUND_DIR", bundled, 1);

    sp_sound_lib *lib = sp_sound_lib_create(cfg);
    if (!lib) return;
    const char *def = sp_sound_lib_default(lib, SP_SOUND_NOTIFICATION);
    SP_ASSERT(def != NULL);
    /* Last alphabetically, chosen anyway. */
    SP_ASSERT(def && strstr(def, "busy") != NULL);

    /* A ringtone slot has no such preference: first wins. */
    const char *r = sp_sound_lib_default(lib, SP_SOUND_RINGTONE);
    SP_ASSERT(r != NULL);
    sp_sound_lib_destroy(lib);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(slot_directories_match_the_shipped_layout);
    SP_RUN(both_directories_are_scanned_and_only_wavs_are_offered);
    SP_RUN(an_imported_name_shadows_a_bundled_one);
    SP_RUN(resolve_tells_silence_apart_from_a_missing_name);
    SP_RUN(import_copies_a_wav_and_refuses_anything_else);
    SP_RUN(an_import_becomes_visible_to_the_next_scan);
    SP_RUN(the_list_survives_a_default_lookup);
    SP_RUN(the_notification_default_prefers_a_busy_tone);
    SP_RUN(null_and_bad_slots_do_not_crash);
    return SP_TEST_RESULT();
}
