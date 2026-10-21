#include "traceexport.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Tracks the remaining space in out and refuses rather than
   truncating: sp_recording_path in recname.c earns its keep the same
   way, and a bare snprintf into a fixed offset is exactly the bug
   that has shipped from this codebase before. */
typedef struct {
    char   *buf;
    size_t  cap;
    size_t  used;
    bool    overflowed;
} sp_append_buf;

static void append(sp_append_buf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void append(sp_append_buf *b, const char *fmt, ...) {
    if (b->overflowed)
        return;
    const size_t remaining = b->cap - b->used;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(b->buf + b->used, remaining, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= remaining) {
        b->overflowed = true;
        return;
    }
    b->used += (size_t)n;
}

int sp_trace_format(char *out, size_t outlen, const sp_sip_message *msgs,
                    size_t n, const char *call_id) {
    if (!out || outlen == 0 || !msgs || n == 0 || !call_id)
        return -1;
    out[0] = '\0';

    bool any_unredacted = false;
    for (size_t i = 0; i < n; i++)
        if (!msgs[i].redacted)
            any_unredacted = true;

    char stamp[32] = "unknown time";
    const time_t now = time(NULL);
    struct tm tmv;
    if (localtime_r(&now, &tmv)) {
        /* POSIX leaves stamp indeterminate on a 0 return, not
           unchanged -- so a failure here must reset the fallback
           text explicitly rather than trust what "unknown time"
           left behind. Same discipline as compose_trace_path in
           core.c. */
        if (strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tmv) == 0)
            strcpy(stamp, "unknown time");
    }

    sp_append_buf b = { out, outlen, 0, false };

    append(&b, "SIP trace for call %s\n", call_id);
    append(&b, "%zu message%s, exported %s\n", n, n == 1 ? "" : "s", stamp);

    /* The warning goes before the messages, at a fixed point near the
       top: a warning below the messages is a warning read after the
       file has already been sent. */
    if (any_unredacted) {
        append(&b,
              "\n*** NOT REDACTED: this trace was captured with "
              "redaction off, so any Authorization or "
              "Proxy-Authorization headers it contains are in "
              "cleartext. Read it before sending it outside the "
              "building. ***\n");
    }
    append(&b, "\n");

    /* msgs arrives newest-first, matching sp_siplog_read; walk it
       back to front so the file reads oldest-first. */
    for (size_t idx = 0; idx < n && !b.overflowed; idx++) {
        const sp_sip_message *m = &msgs[n - 1 - idx];
        char tsbuf[32] = "unknown time";
        const time_t sec = (time_t)(m->ts_us / 1000000ULL);
        struct tm mtm;
        if (localtime_r(&sec, &mtm)) {
            if (strftime(tsbuf, sizeof tsbuf, "%Y-%m-%d %H:%M:%S", &mtm)
                == 0)
                strcpy(tsbuf, "unknown time");
        }

        if (m->method)
            append(&b, "---- %s.%06llu %s %s ----\n", tsbuf,
                  (unsigned long long)(m->ts_us % 1000000ULL),
                  m->direction ? m->direction : "?", m->method);
        else
            append(&b, "---- %s.%06llu %s %d ----\n", tsbuf,
                  (unsigned long long)(m->ts_us % 1000000ULL),
                  m->direction ? m->direction : "?", m->status_code);
        append(&b, "%s\n", m->raw ? m->raw : "");
    }

    if (b.overflowed)
        return -1;
    return (int)b.used;
}

int sp_trace_safe_name(char *out, size_t outlen, const char *in) {
    if (!out || !in || *in == '\0')
        return -1;
    if (outlen > 0)
        out[0] = '\0';

    const size_t len = strlen(in);
    if (len + 1 > outlen)
        return -1;

    bool all_dots = true;
    char last = '\0';
    for (size_t i = 0; i < len; i++) {
        const char c = in[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                        c == '-';
        char out_c = ok ? c : '_';
        /* However it arose, two dots in a row reads as "..", which
           this component must never contain: it lands in a filename,
           and a peer's Call-ID does not get to choose a path that
           walks out of the export directory. */
        if (out_c == '.' && last == '.')
            out_c = '_';
        if (out_c != '.')
            all_dots = false;
        out[i] = out_c;
        last = out_c;
    }
    out[len] = '\0';

    if (all_dots) {
        /* Empty is caught above; this is the other degenerate shape --
           a result that is nothing but dots, such as a source Call-ID
           of "." itself. A filename component with no material in it
           is as unsafe as one that walks out of the directory, so it
           does not get to keep its shape either. */
        if (outlen < 2)
            return -1;
        out[0] = '_';
        out[1] = '\0';
    }
    return 0;
}
