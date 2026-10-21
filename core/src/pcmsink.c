#include "pcmsink.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    pjmedia_port         base;
    pj_pool_t           *pool;
    FILE                *f;
    const pj_int16_t    *zeros;
    unsigned             frame_bytes;
    bool                 failed;
    sp_pcmsink_closed_cb on_closed;
    void                *user;
} sp_pcmsink;

static void write_bytes(sp_pcmsink *s, const void *buf, size_t n) {
    if (s->failed || n == 0)
        return;
    if (fwrite(buf, 1, n, s->f) != n)
        s->failed = true;
}

/* Never an error to the bridge: a full disk loses the transcript, not
   the call's audio. */
static pj_status_t sink_put_frame(pjmedia_port *this_port,
                                  pjmedia_frame *frame) {
    sp_pcmsink *s = (sp_pcmsink *)this_port;
    if (frame->type == PJMEDIA_FRAME_TYPE_AUDIO && frame->buf)
        write_bytes(s, frame->buf, frame->size);
    else
        /* A heartbeat: nobody is talking into this side, but the time
           still passes, and both sides have to stay on one clock. */
        write_bytes(s, s->zeros, s->frame_bytes);
    return PJ_SUCCESS;
}

static pj_status_t sink_get_frame(pjmedia_port *this_port,
                                  pjmedia_frame *frame) {
    (void)this_port;
    frame->type = PJMEDIA_FRAME_TYPE_NONE;
    frame->size = 0;
    return PJ_SUCCESS;
}

static pj_status_t sink_on_destroy(pjmedia_port *this_port) {
    sp_pcmsink *s = (sp_pcmsink *)this_port;
    if (s->f && fclose(s->f) != 0)
        s->failed = true;
    s->f = NULL;
    sp_pcmsink_closed_cb cb = s->on_closed;
    void *user = s->user;
    const bool ok = !s->failed;
    pj_pool_release(s->pool);
    if (cb)
        cb(user, ok);
    return PJ_SUCCESS;
}

sp_status sp_pcmsink_create(pj_pool_t *pool_, const char *path,
                            unsigned clock_rate, unsigned samples_per_frame,
                            sp_pcmsink_closed_cb on_closed, void *user,
                            pjmedia_port **out) {
    if (!pool_ || !path || !*path || !out || clock_rate == 0
        || samples_per_frame == 0)
        return SP_ERR_INVALID_ARG;
    *out = NULL;

    pj_pool_t *pool =
        pj_pool_create(pool_->factory, "pcmsink", 1000, 1000, NULL);
    if (!pool)
        return SP_ERR_INTERNAL;
    sp_pcmsink *s = PJ_POOL_ZALLOC_T(pool, sp_pcmsink);
    pj_int16_t *zeros =
        pj_pool_zalloc(pool, samples_per_frame * sizeof(pj_int16_t));
    if (!s || !zeros) {
        pj_pool_release(pool);
        return SP_ERR_INTERNAL;
    }

    const int fd = open(path,
                        O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                        0600);
    if (fd < 0) {
        pj_pool_release(pool);
        return SP_ERR_IO;
    }
    s->f = fdopen(fd, "wb");
    if (!s->f) {
        close(fd);
        unlink(path);
        pj_pool_release(pool);
        return SP_ERR_IO;
    }

    s->pool = pool;
    s->zeros = zeros;
    s->frame_bytes = samples_per_frame * (unsigned)sizeof(pj_int16_t);
    s->on_closed = on_closed;
    s->user = user;
    s->base.put_frame = &sink_put_frame;
    s->base.get_frame = &sink_get_frame;
    s->base.on_destroy = &sink_on_destroy;
    pj_str_t name = pj_str((char *)"pcmsink");
    pjmedia_port_info_init(&s->base.info, &name,
                           PJMEDIA_SIG_CLASS_PORT_AUD('P', 'S'), clock_rate,
                           1, 16, samples_per_frame);
    *out = &s->base;
    return SP_OK;
}
