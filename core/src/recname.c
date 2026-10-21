#include "recname.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* The stamp sorts by time, and the call id keeps two calls that start
   recording in the same second apart.

   SP_REC_WAV and SP_REC_WAV_ULAW are both WAV containers and share
   the extension: pjsua_recorder_create picks the container from it,
   and a file whose name disagreed with its contents would be worse
   than a large one. SP_REC_OPUS is an Ogg container and gets its own
   extension for the same reason.

   time, localtime_r and strftime are POSIX and present in bionic, so
   this stays buildable for the Android port. */
int sp_recording_name(char *out, size_t outlen, unsigned long long call,
                      sp_recording_format fmt) {
    if (outlen > 0)
        out[0] = '\0';

    char stamp[32] = "00000000-000000";
    const time_t now = time(NULL);
    struct tm tmv;
    if (localtime_r(&now, &tmv)) {
        /* POSIX leaves stamp indeterminate on failure rather than
           unchanged, so a zero return has to be put back to the
           fallback explicitly rather than trusted to still hold it.
           Unreachable at 32 bytes for a 15-byte format, but as cheap
           to check as the snprintf below and the same shape. */
        if (strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv) == 0)
            strcpy(stamp, "00000000-000000");
    }

    const char *ext = fmt == SP_REC_OPUS ? "opus" : "wav";
    const int n = snprintf(out, outlen, "call-%s-%llu.%s", stamp, call, ext);
    if (n < 0 || (size_t)n >= outlen) {
        /* Bounded by construction today -- see the header -- but a
           truncated name here would still compose into a path that
           passes sp_recording_path's own length check, since that
           check is sized off strlen(name) after the fact. Refusing
           here instead of truncating keeps that check honest. */
        if (outlen > 0)
            out[0] = '\0';
        return -1;
    }
    return 0;
}

int sp_recording_path(char *out, size_t outlen, const char *dir,
                      unsigned long long call, sp_recording_format fmt) {
    if (outlen > 0)
        out[0] = '\0';

    char name[128];
    if (sp_recording_name(name, sizeof name, call, fmt) != 0)
        return -1;

    /* dir + '/' + name + NUL, checked before writing so a directory
       that does not fit never leaves a truncated path in out for a
       caller that skips the return value. */
    const size_t need = strlen(dir) + 1 + strlen(name) + 1;
    if (need > outlen)
        return -1;
    snprintf(out, outlen, "%s/%s", dir, name);
    return 0;
}
