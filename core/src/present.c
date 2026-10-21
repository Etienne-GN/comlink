/* Rules about how something is shown, rather than what it is.
 *
 * They belong here for the reason comlink/core.h states at the top:
 * anything a frontend decides is something the other frontend has to
 * decide again, and the two then disagree. Every one of these lived
 * in frontend/qt until a second frontend needed it.
 *
 * Nothing here touches pjsip or the database. It is string work, and
 * it is tested as string work.
 */

#include "comlink/core.h"

#include <stdio.h>
#include <string.h>

/* How many bytes the UTF-8 sequence starting at s occupies, clamped
   to what is actually there. Zero for an empty string.

   Written out rather than borrowed because the alternative is a
   locale-dependent library call on a string that is always UTF-8. */
static size_t utf8_len(const char *s) {
    const unsigned char c = (unsigned char)s[0];
    if (c == 0)          return 0;
    if (c < 0x80)        return 1;
    if ((c & 0xE0) == 0xC0) return s[1] ? 2 : 1;
    if ((c & 0xF0) == 0xE0) return (s[1] && s[2]) ? 3 : 1;
    if ((c & 0xF8) == 0xF0) return (s[1] && s[2] && s[3]) ? 4 : 1;
    return 1;
}

/* Upper-cases one UTF-8 character in place, for the alphabets this
   application actually sees.
 *
 * ASCII, and the Latin-1 supplement so that "Étienne" gives "É" and
 * not a mangled byte. Qt did this with QString::toUpper, which knows
 * every script there is; a 911 centre in Quebec needs French, and a
 * character this does not know is passed through unchanged rather
 * than damaged. That is a deliberate limit, not an oversight. */
static void upper_one(char *s, size_t len) {
    if (len == 1) {
        if (s[0] >= 'a' && s[0] <= 'z')
            s[0] = (char)(s[0] - 32);
        return;
    }
    if (len == 2 && (unsigned char)s[0] == 0xC3) {
        const unsigned char c = (unsigned char)s[1];
        /* U+00E0..U+00FE lower-case to U+00C0..U+00DE, except U+00F7
           which is the division sign and has no case at all. */
        if (c >= 0xA0 && c <= 0xBE && c != 0xB7)
            s[1] = (char)(c - 0x20);
    }
}

sp_status sp_contact_initials(const char *name, char *out, size_t outlen) {
    if (!out || outlen < SP_INITIALS_MAX)
        return SP_ERR_INVALID_ARG;
    out[0] = '\0';
    if (!name)
        return SP_OK;

    /* First letters of the first and last words. "Sylvie Tremblay" is
       ST; "Repartition" is R, not RE, because one letter of one word
       is a monogram and two is a guess at a surname that is not
       there. */
    const char *first = NULL, *last = NULL;
    const char *p = name;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (!first) first = p;
        last = p;
        while (*p && *p != ' ') p++;
    }
    if (!first)
        return SP_OK;

    size_t n = 0;
    const size_t fl = utf8_len(first);
    memcpy(out, first, fl);
    n = fl;
    if (last != first) {
        const size_t ll = utf8_len(last);
        memcpy(out + n, last, ll);
        n += ll;
    }
    out[n] = '\0';

    upper_one(out, fl);
    if (last != first)
        upper_one(out + fl, utf8_len(out + fl));
    return SP_OK;
}

sp_status sp_call_quality_line(const sp_call_quality *q,
                               char *out, size_t outlen) {
    if (!q || !out || outlen < SP_QUALITY_LINE_MAX)
        return SP_ERR_INVALID_ARG;
    /* The middle dot is U+00B7, spelled in UTF-8 rather than written
       literally so the file's encoding cannot quietly change it. */
    const int n = snprintf(out, outlen,
                           "MOS %.2f \xc2\xb7 loss %.1f%% \xc2\xb7 jit %.1f "
                           "\xc2\xb7 rtt %.0f ms",
                           q->mos, q->rx_loss_pct,
                           q->rx_jitter.avg_us / 1000.0,
                           q->rtt.avg_us / 1000.0);
    return (n > 0 && (size_t)n < outlen) ? SP_OK : SP_ERR_INVALID_ARG;
}
