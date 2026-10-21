#include "sound.h"
#include "paths.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef SP_SOUND_DIR
#define SP_SOUND_DIR "/usr/share/comlink/sounds"
#endif

#define SP_N_SLOTS 3

struct sp_sound_lib {
    char           *user_root;      /* <config dir>/sounds */
    sp_sound_file  *files[SP_N_SLOTS];
    size_t          n_files[SP_N_SLOTS];
    char           *resolved[SP_N_SLOTS];   /* last resolve result */
    char           *defaulted[SP_N_SLOTS];  /* last default-name result */
    char           *imported;               /* last import result */
};

const char *sp_sound_slot_dir(sp_sound_slot slot) {
    switch (slot) {
    case SP_SOUND_RINGTONE:     return "inbound";
    case SP_SOUND_RINGBACK:     return "outbound";
    case SP_SOUND_NOTIFICATION: return "notifications";
    }
    return "inbound";
}

/* The shipped directory. Overridable so a build-tree run and the test
   suite can use the tree's own copy rather than an installed one. */
static const char *bundled_root(void) {
    const char *e = getenv("COMLINK_SOUND_DIR");
    return (e && *e) ? e : SP_SOUND_DIR;
}

sp_sound_lib *sp_sound_lib_create(const char *config_path) {
    sp_sound_lib *lib = calloc(1, sizeof *lib);
    if (!lib)
        return NULL;
    /* Beside the configuration file, so everything the operator owns
       lives in one place they already know about. */
    lib->user_root = sp_path_beside(config_path, "sounds");
    return lib;
}

static void free_slot(sp_sound_lib *lib, sp_sound_slot slot) {
    sp_sound_file *f = lib->files[slot];
    for (size_t i = 0; i < lib->n_files[slot]; i++) {
        free((void *)f[i].name);
        free((void *)f[i].path);
    }
    free(f);
    lib->files[slot] = NULL;
    lib->n_files[slot] = 0;
}

void sp_sound_lib_destroy(sp_sound_lib *lib) {
    if (!lib)
        return;
    for (int i = 0; i < SP_N_SLOTS; i++) {
        free_slot(lib, (sp_sound_slot)i);
        free(lib->resolved[i]);
        free(lib->defaulted[i]);
    }
    free(lib->imported);
    free(lib->user_root);
    free(lib);
}

/* Only WAV: pjmedia's player reads nothing else, so listing an mp3
   would offer a choice that cannot be played. */
static bool is_wav(const char *name) {
    size_t n = strlen(name);
    if (n < 5)
        return false;
    return strcasecmp(name + n - 4, ".wav") == 0;
}

static char *strip_ext(const char *name) {
    size_t n = strlen(name);
    char *out = malloc(n + 1);
    if (!out)
        return NULL;
    memcpy(out, name, n + 1);
    if (n > 4)
        out[n - 4] = '\0';
    return out;
}

static bool have_name(const sp_sound_file *f, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(f[i].name, name) == 0)
            return true;
    return false;
}

static int by_name(const void *a, const void *b) {
    const sp_sound_file *x = a, *y = b;
    return strcmp(x->name, y->name);
}

/* One scanned list. Owned by the caller, because the whole point of
   this split is that internal callers must not touch the list handed
   out by sp_sound_lib_scan. */
typedef struct {
    sp_sound_file *f;
    size_t         n;
} sp_list;

static void list_free(sp_list *l) {
    for (size_t i = 0; i < l->n; i++) {
        free((void *)l->f[i].name);
        free((void *)l->f[i].path);
    }
    free(l->f);
    l->f = NULL;
    l->n = 0;
}

static void scan_dir(sp_sound_lib *lib, sp_list *out, sp_sound_slot slot,
                     const char *root, bool bundled) {
    if (!root || !*root)
        return;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/%s", root, sp_sound_slot_dir(slot));
    DIR *d = opendir(dir);
    if (!d)
        return;

    (void)lib;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || !is_wav(e->d_name))
            continue;
        char *name = strip_ext(e->d_name);
        if (!name)
            continue;
        /* The operator's directory is scanned first, so a bundled file
           of the same name is the one that loses. */
        if (have_name(out->f, out->n, name)) {
            free(name);
            continue;
        }
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);

        sp_sound_file *grow = realloc(out->f, (out->n + 1) * sizeof *grow);
        if (!grow) {
            free(name);
            break;
        }
        out->f = grow;
        sp_sound_file *f = &grow[out->n];
        f->name = name;
        f->path = strdup(path);
        f->bundled = bundled;
        if (!f->path) {
            free(name);
            break;
        }
        out->n++;
    }
    closedir(d);
}

/* A fresh list for one slot. The caller frees it with list_free. */
static void scan_slot(sp_sound_lib *lib, sp_sound_slot slot, sp_list *out) {
    out->f = NULL;
    out->n = 0;
    scan_dir(lib, out, slot, lib->user_root, false);
    scan_dir(lib, out, slot, bundled_root(), true);
    if (out->n > 1)
        qsort(out->f, out->n, sizeof *out->f, by_name);
}

sp_status sp_sound_lib_scan(sp_sound_lib *lib, sp_sound_slot slot,
                            const sp_sound_file **out, size_t *n_out) {
    if (!out || !n_out)
        return SP_ERR_INVALID_ARG;
    *out = NULL;
    *n_out = 0;
    if (!lib)
        return SP_OK;
    if ((int)slot < 0 || (int)slot >= SP_N_SLOTS)
        return SP_ERR_INVALID_ARG;

    free_slot(lib, slot);
    sp_list l;
    scan_slot(lib, slot, &l);
    lib->files[slot] = l.f;
    lib->n_files[slot] = l.n;

    *out = lib->files[slot];
    *n_out = lib->n_files[slot];
    return SP_OK;
}

static char *dup_into(char **slot_store, const char *value) {
    free(*slot_store);
    *slot_store = value ? strdup(value) : NULL;
    return *slot_store;
}

const char *sp_sound_lib_resolve(sp_sound_lib *lib, sp_sound_slot slot,
                                 const char *configured, bool *silent) {
    if (silent)
        *silent = false;
    if (!lib || (int)slot < 0 || (int)slot >= SP_N_SLOTS)
        return NULL;

    if (!configured || !*configured || strcmp(configured, "none") == 0) {
        if (silent)
            *silent = true;
        return NULL;
    }

    if (configured[0] == '/') {
        /* An absolute path is the operator's business, but it still has
           to exist: silently falling back to a bundled sound would
           hide a typo in a path. */
        if (access(configured, R_OK) != 0)
            return NULL;
        return dup_into(&lib->resolved[slot], configured);
    }

    /* Its own scan, deliberately. Reusing the public list would free
       whatever sp_sound_list last handed a caller, which is a
       use-after-free the moment a frontend lists the files and then
       asks what is selected -- exactly what the Sounds dialog does. */
    sp_list l;
    scan_slot(lib, slot, &l);
    const char *found = NULL;
    for (size_t i = 0; i < l.n; i++) {
        if (strcmp(l.f[i].name, configured) == 0) {
            found = dup_into(&lib->resolved[slot], l.f[i].path);
            break;
        }
    }
    list_free(&l);
    return found;
}

const char *sp_sound_lib_default(sp_sound_lib *lib, sp_sound_slot slot) {
    if (!lib || (int)slot < 0 || (int)slot >= SP_N_SLOTS)
        return "none";

    sp_list l;
    scan_slot(lib, slot, &l);
    if (l.n == 0) {
        list_free(&l);
        return "none";
    }

    /* Not simply the first file. Alphabetical order put a dial tone on
       "the call ended without being answered", which is the wrong
       thing to hear; a busy tone is what that event sounds like on a
       real phone. A heuristic, and it falls back to the first file. */
    static const char *const NOTIFY_PREF[] = { "busy", "beep", NULL };
    const char *pick = l.f[0].name;
    if (slot == SP_SOUND_NOTIFICATION) {
        bool done = false;
        for (size_t p = 0; NOTIFY_PREF[p] && !done; p++)
            for (size_t i = 0; i < l.n && !done; i++)
                if (strstr(l.f[i].name, NOTIFY_PREF[p])) {
                    pick = l.f[i].name;
                    done = true;
                }
    }
    const char *out = dup_into(&lib->defaulted[slot], pick);
    list_free(&l);
    return out ? out : "none";
}

sp_status sp_sound_lib_import(sp_sound_lib *lib, sp_sound_slot slot,
                              const char *src, const char **stored_out,
                              char *err, size_t errlen) {
    if (stored_out)
        *stored_out = NULL;
    if (!lib || !src || !*src)
        return SP_ERR_INVALID_ARG;
    if ((int)slot < 0 || (int)slot >= SP_N_SLOTS)
        return SP_ERR_INVALID_ARG;

    if (!is_wav(src)) {
        snprintf(err, errlen,
                 "only WAV files can be imported: the media stack has no "
                 "decoder for anything else");
        return SP_ERR_INVALID_ARG;
    }

    FILE *in = fopen(src, "rb");
    if (!in) {
        snprintf(err, errlen, "cannot read %s: %s", src, strerror(errno));
        return SP_ERR_INVALID_ARG;
    }

    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;

    char dir[1024];
    snprintf(dir, sizeof dir, "%s/%s", lib->user_root,
             sp_sound_slot_dir(slot));
    if (sp_path_mkdir_p(dir) != 0) {
        fclose(in);
        snprintf(err, errlen, "cannot create %s: %s", dir, strerror(errno));
        return SP_ERR_INTERNAL;
    }

    char dest[2048];
    snprintf(dest, sizeof dest, "%s/%s", dir, base);

    /* Written to a temporary name and renamed, so an interrupted copy
       cannot leave a truncated sound behind for the phone to play. */
    char tmp[2100];
    snprintf(tmp, sizeof tmp, "%s.part", dest);
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        fclose(in);
        snprintf(err, errlen, "cannot write %s: %s", tmp, strerror(errno));
        return SP_ERR_INTERNAL;
    }

    char buf[16384];
    size_t got;
    bool ok = true;
    while ((got = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, got, out) != got) {
            ok = false;
            break;
        }
    }
    if (ferror(in))
        ok = false;
    fclose(in);
    if (fclose(out) != 0)
        ok = false;

    if (!ok) {
        remove(tmp);
        snprintf(err, errlen, "copy of %s failed", base);
        return SP_ERR_INTERNAL;
    }
    if (rename(tmp, dest) != 0) {
        remove(tmp);
        snprintf(err, errlen, "cannot install %s: %s", dest,
                 strerror(errno));
        return SP_ERR_INTERNAL;
    }

    if (stored_out)
        *stored_out = dup_into(&lib->imported, dest);
    return SP_OK;
}
