#include "sipuri.h"

#include <string.h>

static void copy_trimmed(char *dst, size_t cap, const char *src, size_t len) {
    while (len > 0 && (*src == ' ' || *src == '\t')) {
        src++;
        len--;
    }
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t'))
        len--;
    if (len >= cap)
        len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

void sp_sipuri_parse(const char *raw, sp_party *out) {
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    if (!raw || !*raw)
        return;

    const char *end = raw + strlen(raw);

    /* Display name: quoted, else whatever precedes the angle bracket. */
    const char *uri = raw;
    const char *lt = strchr(raw, '<');
    if (raw[0] == '"') {
        const char *close = strchr(raw + 1, '"');
        if (close)
            copy_trimmed(out->display, sizeof out->display, raw + 1,
                         (size_t)(close - raw - 1));
    } else if (lt && lt > raw) {
        copy_trimmed(out->display, sizeof out->display, raw,
                     (size_t)(lt - raw));
    }

    if (lt) {
        uri = lt + 1;
        const char *gt = strchr(uri, '>');
        if (gt)
            end = gt;
    }

    /* Scheme. Without one there is nothing to parse, and inventing a
       user from arbitrary text would be worse than reporting none. */
    if (end - uri > 4 && strncmp(uri, "sip:", 4) == 0)
        uri += 4;
    else if (end - uri > 5 && strncmp(uri, "sips:", 5) == 0)
        uri += 5;
    else
        return;

    /* The user part ends at '@'; without one the whole thing is a host. */
    const char *at = memchr(uri, '@', (size_t)(end - uri));
    const char *host = uri;
    if (at) {
        copy_trimmed(out->user, sizeof out->user, uri, (size_t)(at - uri));
        host = at + 1;
    }

    /* Trim the port and any parameters off the host. */
    size_t hlen = (size_t)(end - host);
    for (size_t i = 0; i < hlen; i++) {
        if (host[i] == ':' || host[i] == ';' || host[i] == '?') {
            hlen = i;
            break;
        }
    }
    copy_trimmed(out->host, sizeof out->host, host, hlen);
}

const char *sp_sipuri_label(const sp_party *p) {
    if (!p)
        return "unknown";
    if (p->display[0])
        return p->display;
    if (p->user[0])
        return p->user;
    if (p->host[0])
        return p->host;
    return "unknown";
}
