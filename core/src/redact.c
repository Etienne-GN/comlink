#include "redact.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *const CREDENTIAL_HEADERS[] = {
    "Authorization",
    "Proxy-Authorization",
    "WWW-Authenticate",
    "Proxy-Authenticate",
    NULL
};

#define REDACTED ": <redacted>\r\n"

/* Whole-name match on the text before the colon. A substring search
   would also eat X-Authorization-Note, which is somebody's own header
   and none of our business. */
static const char *credential_header(const char *line, size_t len) {
    const char *colon = memchr(line, ':', len);
    if (!colon)
        return NULL;
    size_t n = (size_t)(colon - line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t'))
        n--;
    for (size_t i = 0; CREDENTIAL_HEADERS[i]; i++) {
        if (strlen(CREDENTIAL_HEADERS[i]) == n &&
            strncasecmp(line, CREDENTIAL_HEADERS[i], n) == 0)
            return CREDENTIAL_HEADERS[i];
    }
    return NULL;
}

char *sp_redact_sip(const char *raw) {
    if (!raw)
        return NULL;

    const size_t in_len = strlen(raw);
    /* Replacement is never longer than the longest header name plus
       the marker, so the input length plus a fixed slack is enough. */
    size_t cap = in_len + 64 * (sizeof CREDENTIAL_HEADERS / sizeof *CREDENTIAL_HEADERS) + 64;
    char *out = malloc(cap);
    if (!out)
        return NULL;

    size_t w = 0;
    const char *p = raw;
    const char *end = raw + in_len;
    int in_body = 0;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl + 1 : end;
        size_t len = (size_t)(line_end - p);

        if (in_body) {
            memcpy(out + w, p, len);
            w += len;
            p = line_end;
            continue;
        }

        /* An empty line ends the headers; everything after is body. */
        size_t bare = len;
        while (bare > 0 && (p[bare - 1] == '\r' || p[bare - 1] == '\n'))
            bare--;
        if (bare == 0) {
            memcpy(out + w, p, len);
            w += len;
            in_body = 1;
            p = line_end;
            continue;
        }

        const char *name = credential_header(p, bare);
        if (!name) {
            memcpy(out + w, p, len);
            w += len;
            p = line_end;
            continue;
        }

        /* Emit the name and the marker, then skip the value and any
           folded continuation lines, which carry the rest of it. */
        size_t nlen = strlen(name);
        memcpy(out + w, name, nlen);
        w += nlen;
        memcpy(out + w, REDACTED, sizeof REDACTED - 1);
        w += sizeof REDACTED - 1;
        p = line_end;

        while (p < end && (*p == ' ' || *p == '\t')) {
            const char *cont_nl = memchr(p, '\n', (size_t)(end - p));
            p = cont_nl ? cont_nl + 1 : end;
        }
    }

    out[w] = '\0';
    return out;
}
