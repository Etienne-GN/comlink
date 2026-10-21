/* Encodes and muxes a call recording as an OggOpus file.

   Three facts drive this file's shape (see oggopus.h and the plan's
   Global Constraints for the long version):

   - Granule positions are always in 48 kHz samples, never the port's
     own clock rate. frame_granule_48k below is computed from the
     20ms frame duration and SP_OGGOPUS_GRANULE_RATE only -- it must
     never involve clock_rate, or every recording seeks wrong while
     still sounding right.
   - OpusHead's pre-skip comes from OPUS_GET_LOOKAHEAD, scaled to
     48 kHz, never hardcoded.
   - put_frame() is handed whatever samples_per_frame the conference
     bridge uses, which is not necessarily an Opus-legal frame size
     (2.5/5/10/20/40/60ms). This file buffers and always encodes
     exactly 20ms at a time.

   put_frame() and on_destroy() do only pool-backed allocation,
   buffered stdio, and one bounded 4000-byte stack buffer per frame
   for the encoded packet (SP_OGGOPUS_MAX_PACKET below) -- matching
   the existing WAV writer: safe to run on the media thread, no core
   lock, no callback into the core. */
#include "oggopus.h"

#include <ogg/ogg.h>
#include <opus/opus.h>

#include <pj/errno.h>
#include <pj/rand.h>
#include <pjmedia/signatures.h>

#include <stdio.h>
#include <string.h>

/* 2.5/5/10/20/40/60ms are Opus's only legal frame durations. 20ms
   divides every clock rate Opus accepts (8/12/16/24/48 kHz) exactly,
   so frame_samples below is always a whole number with no rounding
   to reason about. */
#define SP_OGGOPUS_FRAME_MS 20u
/* The spec's figure: 24 kbps mono. */
#define SP_OGGOPUS_BITRATE 24000
/* Comfortably above Opus's worst-case single-frame packet (1275
   bytes per RFC 6716 S3.2.1); matches the headroom opus_demo itself
   allocates. */
#define SP_OGGOPUS_MAX_PACKET 4000u

typedef struct sp_oggopus_writer {
    pjmedia_port base;
    pj_pool_t *pool;
    FILE *f;
    ogg_stream_state os;
    pj_bool_t os_inited;
    OpusEncoder *enc;

    unsigned clock_rate;
    unsigned channels;
    unsigned frame_samples;        /* Opus frame size, at clock_rate */
    ogg_int64_t frame_granule_48k; /* one Opus frame's duration, 48kHz */

    pj_int16_t *buf;   /* frame_samples * channels */
    unsigned buf_fill; /* samples per channel currently buffered */

    ogg_int64_t granulepos;
    ogg_int64_t packetno;
    pj_bool_t finalized;
    char *path; /* pool-owned copy, kept to name a failed finalisation
                    in the stderr log -- see writer_on_destroy */
} sp_oggopus_writer;

static pj_bool_t write_page(sp_oggopus_writer *w, const ogg_page *og) {
    size_t hn = fwrite(og->header, 1, (size_t)og->header_len, w->f);
    if (hn != (size_t)og->header_len)
        return PJ_FALSE;
    if (og->body_len > 0) {
        size_t bn = fwrite(og->body, 1, (size_t)og->body_len, w->f);
        if (bn != (size_t)og->body_len)
            return PJ_FALSE;
    }
    return PJ_TRUE;
}

/* Header packets (OpusHead, OpusTags) are each flushed to their own
   page immediately: RFC 7845 S3 requires the identification header
   alone on the stream's first page, and giving OpusTags the same
   treatment is what every OggOpus encoder in practice does too. */
static pj_status_t write_header_packet(sp_oggopus_writer *w,
                                       const unsigned char *data, long bytes,
                                       int bos) {
    ogg_packet op;
    op.packet = (unsigned char *)data;
    op.bytes = bytes;
    op.b_o_s = bos;
    op.e_o_s = 0;
    op.granulepos = 0;
    op.packetno = w->packetno++;

    if (ogg_stream_packetin(&w->os, &op) != 0)
        return PJ_EUNKNOWN;

    ogg_page og;
    while (ogg_stream_flush(&w->os, &og)) {
        if (!write_page(w, &og))
            return PJ_EUNKNOWN;
    }
    return PJ_SUCCESS;
}

/* Encodes the full frame currently sitting in w->buf and hands the
   packet to libogg. `granule_advance` is in 48kHz samples -- the
   fixed per-frame duration for a mid-stream frame, or a value
   trimmed to the real sample count for the last, padded frame. */
static pj_status_t encode_and_write(sp_oggopus_writer *w,
                                    ogg_int64_t granule_advance, int eos) {
    unsigned char packet[SP_OGGOPUS_MAX_PACKET];
    opus_int32 n = opus_encode(w->enc, w->buf, (int)w->frame_samples, packet,
                               (opus_int32)sizeof packet);
    if (n < 0)
        return PJ_EUNKNOWN;

    w->granulepos += granule_advance;

    ogg_packet op;
    op.packet = packet;
    op.bytes = n;
    op.b_o_s = 0;
    op.e_o_s = eos;
    op.granulepos = w->granulepos;
    op.packetno = w->packetno++;

    if (ogg_stream_packetin(&w->os, &op) != 0)
        return PJ_EUNKNOWN;

    ogg_page og;
    while (ogg_stream_pageout(&w->os, &og)) {
        if (!write_page(w, &og))
            return PJ_EUNKNOWN;
    }
    return PJ_SUCCESS;
}

static pj_status_t writer_put_frame(pjmedia_port *this_port,
                                    pjmedia_frame *frame) {
    sp_oggopus_writer *w = (sp_oggopus_writer *)this_port;

    if (w->finalized)
        return PJ_EINVALIDOP;

    const pj_int16_t *src = (const pj_int16_t *)frame->buf;
    const unsigned in_samples =
        (unsigned)(frame->size / ((pj_size_t)w->channels * sizeof(pj_int16_t)));
    unsigned consumed = 0;

    while (consumed < in_samples) {
        unsigned room = w->frame_samples - w->buf_fill;
        unsigned take = in_samples - consumed;
        if (take > room)
            take = room;

        memcpy(w->buf + (size_t)w->buf_fill * w->channels,
              src + (size_t)consumed * w->channels,
              (size_t)take * w->channels * sizeof(pj_int16_t));
        w->buf_fill += take;
        consumed += take;

        if (w->buf_fill == w->frame_samples) {
            pj_status_t st = encode_and_write(w, w->frame_granule_48k, 0);
            if (st != PJ_SUCCESS)
                return st;
            w->buf_fill = 0;
        }
    }
    return PJ_SUCCESS;
}

/* Flushes, flags the last page end-of-stream, and closes the file.
   Idempotent: a second call (there should not be one, but on_destroy
   guards it) is a no-op rather than a double free/close.

   The file is closed exactly once here, on every path -- success or
   failure -- so on_destroy never has to guess whether it still needs
   to. A failure anywhere in finalisation leaves a file with no EOS
   page and a wrong final granule: the same shape as a truncated
   recording. Unlike the trace-export writer, that file is kept
   rather than removed: writer_on_destroy runs later, on the bridge's
   own thread under the group lock, where nothing reads a return
   value, so nobody would ever learn a removed recording was gone.
   For a 911 recording, an unfinalized file -- usually still readable
   by opusdec or ffmpeg despite the missing EOS page -- beats a
   silently deleted one. */
static pj_status_t writer_finalize(sp_oggopus_writer *w) {
    if (w->finalized)
        return PJ_SUCCESS;
    w->finalized = PJ_TRUE;

    pj_status_t st;
    if (w->buf_fill > 0) {
        /* Opus cannot encode a partial frame, so the tail is padded
           with silence. The granule position advances only by the
           real sample count, scaled to 48kHz and rounded to the
           nearest sample -- not by the padded frame's full duration
           -- so a compliant player end-trims the padding instead of
           playing it back as audio. */
        memset(w->buf + (size_t)w->buf_fill * w->channels, 0,
              (size_t)(w->frame_samples - w->buf_fill) * w->channels *
                  sizeof(pj_int16_t));
        ogg_int64_t advance =
            ((ogg_int64_t)w->buf_fill * SP_OGGOPUS_GRANULE_RATE +
            w->clock_rate / 2) /
            w->clock_rate;
        st = encode_and_write(w, advance, 1);
        w->buf_fill = 0;
    } else {
        /* Nothing buffered: the last real packet already went to
           libogg with e_o_s=0 and may already be inside an emitted
           page, so it cannot be re-flagged now. A frame of pure
           silence carries the end-of-stream flag instead; its granule
           position does not advance past what real audio already
           established, so it end-trims away rather than extending
           the file's audible duration. */
        memset(w->buf, 0,
              (size_t)w->frame_samples * w->channels * sizeof(pj_int16_t));
        st = encode_and_write(w, 0, 1);
    }

    if (st == PJ_SUCCESS) {
        ogg_page og;
        while (ogg_stream_flush(&w->os, &og)) {
            if (!write_page(w, &og)) {
                st = PJ_EUNKNOWN;
                break;
            }
        }
    }

    if (w->f) {
        pj_bool_t closed_ok = (fclose(w->f) == 0);
        w->f = NULL;
        if (st == PJ_SUCCESS && !closed_ok)
            st = PJ_EUNKNOWN;
    }

    /* Kept, not removed, on failure -- see this function's own
       comment above. writer_on_destroy logs it. */
    return st;
}

static pj_status_t writer_on_destroy(pjmedia_port *this_port) {
    sp_oggopus_writer *w = (sp_oggopus_writer *)this_port;
    pj_status_t st = writer_finalize(w);

    /* writer_on_destroy runs on the bridge's own thread once the
       group lock releases it, so this return value reaches nobody --
       the caller that tore down the call has long since moved on.
       Reported here the same way core.c reports other non-fatal
       problems, or a finalisation failure would be invisible: the
       file itself is now the only trace of what happened, and it is
       kept (not removed) exactly so that trace is not also lost. */
    if (st != PJ_SUCCESS) {
        char pjmsg[PJ_ERR_MSG_SIZE] = {0};
        pj_strerror(st, pjmsg, sizeof pjmsg);
        fprintf(stderr,
                "comlink: recording not finalized, kept as-is (%s): %s\n",
                w->path ? w->path : "?", pjmsg);
    }

    if (w->enc)
        opus_encoder_destroy(w->enc);
    if (w->os_inited)
        ogg_stream_clear(&w->os);
    pj_pool_release(w->pool);
    return st;
}

sp_status sp_oggopus_writer_create(pj_pool_t *pool_, const char *path,
                                   unsigned clock_rate, unsigned channels,
                                   unsigned samples_per_frame,
                                   pjmedia_port **out) {
    if (!pool_ || !path || !out)
        return SP_ERR_INVALID_ARG;
    if (channels < 1 || channels > 2)
        return SP_ERR_INVALID_ARG;
    switch (clock_rate) {
    case 8000:
    case 12000:
    case 16000:
    case 24000:
    case 48000:
        break;
    default:
        return SP_ERR_INVALID_ARG;
    }
    *out = NULL;

    pj_pool_t *pool =
        pj_pool_create(pool_->factory, "oggopus_writer", 1000, 1000, NULL);
    if (!pool)
        return SP_ERR_INTERNAL;

    sp_oggopus_writer *w = PJ_POOL_ZALLOC_T(pool, sp_oggopus_writer);
    if (!w) {
        pj_pool_release(pool);
        return SP_ERR_INTERNAL;
    }
    w->pool = pool;
    w->clock_rate = clock_rate;
    w->channels = channels;
    w->frame_samples = clock_rate * SP_OGGOPUS_FRAME_MS / 1000u;
    w->frame_granule_48k =
        (ogg_int64_t)SP_OGGOPUS_GRANULE_RATE * SP_OGGOPUS_FRAME_MS / 1000;

    size_t path_len = strlen(path) + 1;
    w->path = pj_pool_alloc(pool, path_len);
    if (!w->path) {
        pj_pool_release(pool);
        return SP_ERR_INTERNAL;
    }
    memcpy(w->path, path, path_len);

    w->buf = pj_pool_alloc(
        pool, (pj_size_t)w->frame_samples * channels * sizeof(pj_int16_t));
    if (!w->buf) {
        pj_pool_release(pool);
        return SP_ERR_INTERNAL;
    }

    w->f = fopen(path, "wb");
    if (!w->f) {
        pj_pool_release(pool);
        return SP_ERR_IO;
    }

    ogg_stream_init(&w->os, pj_rand());
    w->os_inited = PJ_TRUE;

    int opus_err = 0;
    w->enc = opus_encoder_create((opus_int32)clock_rate, (int)channels,
                                 OPUS_APPLICATION_VOIP, &opus_err);
    if (opus_err != OPUS_OK || !w->enc) {
        (void)fclose(w->f);
        (void)remove(path);
        ogg_stream_clear(&w->os);
        pj_pool_release(pool);
        return SP_ERR_MEDIA;
    }
    /* Both ctl calls are cheap and, on a freshly created encoder with
       valid arguments, do not fail in practice; a failure here means
       something is actually wrong, so it is reported rather than
       silently tolerated. The bitrate one would otherwise merely fall
       back to the encoder's default -- low-impact by itself -- but
       proceeding past a failed OPUS_GET_LOOKAHEAD would leave
       `lookahead` at its initialised 0, silently making pre_skip 0
       and the granule accounting below wrong in a way that looks
       deliberate. Both are treated the same way for that reason. */
    if (opus_encoder_ctl(w->enc, OPUS_SET_BITRATE(SP_OGGOPUS_BITRATE)) !=
       OPUS_OK) {
        opus_encoder_destroy(w->enc);
        (void)fclose(w->f);
        (void)remove(path);
        ogg_stream_clear(&w->os);
        pj_pool_release(pool);
        return SP_ERR_MEDIA;
    }

    opus_int32 lookahead = 0;
    if (opus_encoder_ctl(w->enc, OPUS_GET_LOOKAHEAD(&lookahead)) != OPUS_OK) {
        opus_encoder_destroy(w->enc);
        (void)fclose(w->f);
        (void)remove(path);
        ogg_stream_clear(&w->os);
        pj_pool_release(pool);
        return SP_ERR_MEDIA;
    }
    /* Pre-skip: the encoder's own lookahead, scaled from clock_rate
       to the fixed 48kHz granule rate. Never hardcoded -- a wrong
       value here loses the first syllable of every recording, or
       opens with a click. */
    unsigned pre_skip =
        (unsigned)(((pj_int64_t)lookahead * SP_OGGOPUS_GRANULE_RATE +
                    clock_rate / 2) /
                   clock_rate);
    /* RFC 7845 S4.3: a stream's granule position counts decoded
       samples from the start of the logical stream, and that count
       always includes the pre-skip samples themselves -- playback
       duration is granule minus pre-skip, not granule alone. Starting
       the running total here, before any real audio, rather than at
       0, is what keeps every page's granule (including an empty
       stream's, which must equal pre_skip exactly) consistent with
       that. Verified against opusinfo: starting at 0 makes an empty
       recording report a negative duration and "buggy encoder". */
    w->granulepos = (ogg_int64_t)pre_skip;

    unsigned char head[19];
    memcpy(head, "OpusHead", 8);
    head[8] = 1; /* version */
    head[9] = (unsigned char)channels;
    head[10] = (unsigned char)(pre_skip & 0xffu);
    head[11] = (unsigned char)((pre_skip >> 8) & 0xffu);
    head[12] = (unsigned char)(clock_rate & 0xffu);
    head[13] = (unsigned char)((clock_rate >> 8) & 0xffu);
    head[14] = (unsigned char)((clock_rate >> 16) & 0xffu);
    head[15] = (unsigned char)((clock_rate >> 24) & 0xffu);
    head[16] = 0; /* output gain, low byte */
    head[17] = 0; /* output gain, high byte */
    head[18] = 0; /* channel mapping family 0: mono/stereo, no table */

    pj_status_t st = write_header_packet(w, head, (long)sizeof head, 1);
    if (st != PJ_SUCCESS) {
        opus_encoder_destroy(w->enc);
        ogg_stream_clear(&w->os);
        (void)fclose(w->f);
        (void)remove(path);
        pj_pool_release(pool);
        return SP_ERR_IO;
    }

    const char *vendor = opus_get_version_string();
    size_t vlen = strlen(vendor);
    /* Vendor strings from libopus are short ("libopus 1.6.1"); this
       bound is generous and checked, never assumed. */
    if (vlen > 200)
        vlen = 200;
    unsigned char tags[8 + 4 + 200 + 4];
    memcpy(tags, "OpusTags", 8);
    tags[8] = (unsigned char)(vlen & 0xffu);
    tags[9] = (unsigned char)((vlen >> 8) & 0xffu);
    tags[10] = (unsigned char)((vlen >> 16) & 0xffu);
    tags[11] = (unsigned char)((vlen >> 24) & 0xffu);
    memcpy(tags + 12, vendor, vlen);
    tags[12 + vlen] = 0;
    tags[13 + vlen] = 0;
    tags[14 + vlen] = 0;
    tags[15 + vlen] = 0;

    st = write_header_packet(w, tags, (long)(16 + vlen), 0);
    if (st != PJ_SUCCESS) {
        opus_encoder_destroy(w->enc);
        ogg_stream_clear(&w->os);
        (void)fclose(w->f);
        (void)remove(path);
        pj_pool_release(pool);
        return SP_ERR_IO;
    }

    w->base.put_frame = &writer_put_frame;
    w->base.on_destroy = &writer_on_destroy;
    pj_str_t name = pj_str((char *)"oggopus_writer");
    pjmedia_port_info_init(&w->base.info, &name,
                           PJMEDIA_SIG_CLASS_PORT_AUD('O', 'W'), clock_rate,
                           channels, 16, samples_per_frame);

    *out = &w->base;
    return SP_OK;
}
