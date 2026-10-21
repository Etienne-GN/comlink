/* Decodes an OggOpus file written by oggopus_write.c (or any other
   RFC 7845 conformant single-stream, mapping-family-0 encoder) back
   into PCM for pjmedia.

   Two facts drive this file's shape, the reader-side twins of the
   writer's own:

   - OpusHead's pre-skip must be discarded from the FRONT of the
     decoder's own output, not computed from granule positions: RFC
     7845 S4.2 is explicit that pre-skip signals "the number of
     samples that SHOULD be skipped (decoded but discarded) at the
     beginning of the stream", full stop. Getting this from the
     file's granule accounting instead is more machinery for the
     same answer and easy to get wrong at page boundaries; a plain
     sample counter cannot be.
   - The granule position tells the truth about only one thing a
     decoder cannot: how much of the FINAL packet is real audio versus
     the silence padding oggopus_write.c pads a partial last frame
     with (see its writer_finalize). That padding decodes to real,
     inaudible-but-present silence -- nothing in the bitstream marks
     it as padding -- so it is trimmed using the last page's granule,
     and only there. Every other packet is used in full.

   get_frame() and the header parse it depends on do only pool-backed
   allocation and buffered stdio, matching the writer: safe to run on
   the media thread, no core lock, no callback into the core. */
#include "oggopus.h"

#include <ogg/ogg.h>
#include <opus/opus.h>

#include <pj/errno.h>
#include <pjmedia/signatures.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* 120ms is Opus's longest legal frame; at 48kHz (the highest rate a
   decoder can be created at) that is the most samples per channel a
   single opus_decode() call can ever produce. Sized once, generously,
   rather than computed from clock_rate, so it stays correct even
   though this reader only ever gets created at SP_REC_BRIDGE_CLOCK_RATE
   today. */
#define SP_OGGOPUS_MAX_FRAME_SAMPLES 5760u
#define SP_OGGOPUS_MAX_CHANNELS 2u

/* Read chunk for pulling more file bytes into libogg's sync buffer.
   Arbitrary but generous relative to a 4000-byte Opus packet (see
   oggopus_write.c's SP_OGGOPUS_MAX_PACKET). */
#define SP_OGGOPUS_READ_CHUNK 4096

typedef struct sp_oggopus_reader {
    pjmedia_port base;
    pj_pool_t *pool;
    FILE *f;

    ogg_sync_state oy;
    pj_bool_t oy_inited;
    ogg_stream_state os;
    pj_bool_t os_inited;
    long serialno;
    pj_bool_t file_eof; /* fread hit end of file; no more bytes to add */

    OpusDecoder *dec;
    unsigned clock_rate;
    unsigned channels;

    unsigned pre_skip_48k;    /* OpusHead's field, verbatim */
    unsigned pre_skip_remaining; /* decoded samples/channel still to drop */

    /* Running count of real (post-pre-skip) samples/channel already
       queued for the bridge, in the decoder's own clock_rate -- what
       the final packet's end-trim is measured against. Written by
       reader_get_frame on the media thread, read by
       sp_oggopus_reader_get_pos_ms from the caller's own thread with
       no other synchronisation -- _Atomic for the same reason as the
       eof flag below, and because a plain 64-bit read is not
       guaranteed atomic on a 32-bit target such as armeabi-v7a, where
       a torn read would show a wildly wrong position. */
    _Atomic pj_uint64_t emitted_at_rate;
    /* Set once a packet carrying e_o_s has been decoded (or the file
       simply ran out): total_real_at_rate below is final and no
       further packets will be pulled. */
    pj_bool_t saw_eos;
    pj_uint64_t total_real_at_rate; /* valid once saw_eos is set */

    pj_int16_t *decode_buf; /* SP_OGGOPUS_MAX_FRAME_SAMPLES * channels */

    /* Small queue of decoded-but-not-yet-served samples, interleaved.
       Sized for one worst-case decode; compacted to [0, fill) each
       time it runs dry rather than grown, since get_frame only ever
       adds at most one packet's worth before serving from it again. */
    pj_int16_t *out_buf;
    unsigned out_cap;  /* interleaved samples out_buf can hold */
    unsigned out_pos;  /* next unserved sample */
    unsigned out_fill; /* one past the last valid sample */

    _Atomic pj_bool_t eof; /* true once nothing further will ever decode */
} sp_oggopus_reader;

/* Pulls one more chunk of file bytes into the sync state. Returns
   PJ_FALSE once the file is exhausted (an ordinary end, not an
   error); further calls after that are a cheap no-op. */
static pj_bool_t fill_more(sp_oggopus_reader *r) {
    if (r->file_eof)
        return PJ_FALSE;
    char *buf = ogg_sync_buffer(&r->oy, SP_OGGOPUS_READ_CHUNK);
    if (!buf) {
        r->file_eof = PJ_TRUE;
        return PJ_FALSE;
    }
    size_t n = fread(buf, 1, SP_OGGOPUS_READ_CHUNK, r->f);
    if (ogg_sync_wrote(&r->oy, (long)n) != 0) {
        r->file_eof = PJ_TRUE;
        return PJ_FALSE;
    }
    if (n == 0) {
        r->file_eof = PJ_TRUE;
        return PJ_FALSE;
    }
    return PJ_TRUE;
}

/* Next packet of the file's one logical stream, whether header or
   audio. PJ_FALSE means the file is genuinely exhausted -- not a
   transient "need more bytes" -- there being nothing left for
   get_frame to do but serve silence. A page belonging to some other
   logical stream (a foreign BOS in a multiplexed file) is skipped
   rather than fed to the decoder as if it were ours; a lost sync (-1
   from ogg_sync_pageout or ogg_stream_packetout), a page that fails
   ogg_stream_init, or a page that fails ogg_stream_pagein are all
   likewise skipped forward rather than treated as fatal, since the
   honest recovery from a corrupt patch of an otherwise fine file is
   to keep reading, not to give up on everything after it -- a
   recording that stops early because of one damaged page, with no
   indication, is worse than one that loses that page's audio. */
static pj_bool_t next_packet(sp_oggopus_reader *r, ogg_packet *op) {
    for (;;) {
        if (r->os_inited) {
            int rc = ogg_stream_packetout(&r->os, op);
            if (rc == 1)
                return PJ_TRUE;
            /* 0: this page's packets are exhausted, need another page.
               -1: a gap in the packet sequence; fall through and keep
               going, same as a page-level resync. */
        }

        ogg_page og;
        int rc = ogg_sync_pageout(&r->oy, &og);
        if (rc == 0) {
            if (fill_more(r))
                continue;
            /* File just ran dry: one last drain in case a complete
               page was already sitting fully buffered before that
               final zero-byte read. */
            if (ogg_sync_pageout(&r->oy, &og) != 1)
                return PJ_FALSE;
        } else if (rc == -1) {
            continue;
        }

        long serial = ogg_page_serialno(&og);
        if (!r->os_inited) {
            /* A page this damaged that even ogg_stream_init refuses it
               is skipped, not fatal: the next page may still init a
               usable stream. os_inited stays false, so the next
               iteration simply tries again with the next page. */
            if (ogg_stream_init(&r->os, (int)serial) != 0)
                continue;
            r->os_inited = PJ_TRUE;
            r->serialno = serial;
        } else if (serial != r->serialno) {
            continue;
        }
        /* Likewise: a page that fails to feed into an already-good
           stream is one damaged page, not the end of the file -- skip
           it and keep reading rather than stopping the recording
           short with no indication. */
        if (ogg_stream_pagein(&r->os, &og) != 0)
            continue;
    }
}

/* Parses OpusHead (packet 1) and skips over OpusTags (packet 2).
   Field layout: RFC 7845 S5.1, the same one oggopus_write.c writes.
   Rejects anything this reader cannot honestly decode instead of
   guessing: a wrong channel mapping family, or a channel count that
   does not match what the caller asked to receive, both produce
   output that is not the recording -- silently resampling or
   remapping channels is exactly the kind of guess the pre-skip hazard
   this file's header comment warns about is a special case of. */
static pj_status_t parse_headers(sp_oggopus_reader *r) {
    ogg_packet op;
    if (!next_packet(r, &op) || op.bytes < 19 ||
        memcmp(op.packet, "OpusHead", 8) != 0)
        return PJ_EINVALIDOP;
    /* RFC 7845 S5.1: only the top 4 bits of the version byte need be
       zero for a decoder to proceed; the low bits may vary between
       otherwise-compatible encoders. */
    if ((op.packet[8] & 0xf0) != 0)
        return PJ_EINVALIDOP;

    unsigned head_channels = op.packet[9];
    unsigned pre_skip = (unsigned)op.packet[10] | ((unsigned)op.packet[11] << 8);
    unsigned char mapping_family = op.packet[18];

    if (mapping_family != 0)
        return PJ_ENOTSUP;
    if (head_channels != r->channels)
        return PJ_EINVALIDOP;

    r->pre_skip_48k = pre_skip;
    r->pre_skip_remaining =
        (unsigned)(((pj_uint64_t)pre_skip * r->clock_rate +
                    SP_OGGOPUS_GRANULE_RATE / 2) /
                   SP_OGGOPUS_GRANULE_RATE);

    if (!next_packet(r, &op) || op.bytes < 8 ||
        memcmp(op.packet, "OpusTags", 8) != 0)
        return PJ_EINVALIDOP;

    return PJ_SUCCESS;
}

/* Appends `n` interleaved samples to the out_buf queue, compacting the
   already-served prefix out of the way first if there is not enough
   room -- normal once the queue has been drained by earlier
   get_frame() calls, since out_buf is sized for exactly one decode's
   worth plus whatever a caller has not yet collected. */
static void queue_samples(sp_oggopus_reader *r, const pj_int16_t *samples,
                          unsigned n) {
    if (n == 0)
        return;
    if (r->out_fill + n > r->out_cap) {
        unsigned remaining = r->out_fill - r->out_pos;
        if (remaining > 0)
            memmove(r->out_buf, r->out_buf + r->out_pos,
                   (size_t)remaining * sizeof(pj_int16_t));
        r->out_pos = 0;
        r->out_fill = remaining;
    }
    /* out_cap is sized for one full decode either way; a queue that
       still would not fit after compaction is an internal sizing bug,
       not a condition to guess through, so the excess is dropped
       rather than overrunning out_buf. */
    unsigned room = r->out_cap - r->out_fill;
    if (n > room)
        n = room;
    memcpy(r->out_buf + r->out_fill, samples, (size_t)n * sizeof(pj_int16_t));
    r->out_fill += n;
}

/* Decodes exactly one more audio packet into the out_buf queue,
   applying pre-skip and (on the file's last packet) end-trim.
   Returns PJ_FALSE once there is nothing further to decode -- setting
   r->saw_eos happens here, the moment either an e_o_s packet or the
   plain end of the file is reached, whichever comes first. */
static pj_bool_t decode_one_packet(sp_oggopus_reader *r) {
    ogg_packet op;
    if (!next_packet(r, &op)) {
        r->saw_eos = PJ_TRUE;
        r->total_real_at_rate = atomic_load(&r->emitted_at_rate);
        return PJ_FALSE;
    }

    int n_dec = opus_decode(r->dec, op.packet, (opus_int32)op.bytes,
                            r->decode_buf, (int)SP_OGGOPUS_MAX_FRAME_SAMPLES,
                            0);
    if (n_dec < 0) {
        /* A corrupt packet in an otherwise fine file: opus_decode can
           conceal a single lost/garbled packet given a NULL "loss"
           call, but a packet that fails outright is skipped rather
           than aborting the whole file -- silence for that one
           packet's duration is a far smaller wrong than stopping
           playback of everything after it. */
        return PJ_TRUE;
    }
    unsigned n = (unsigned)n_dec; /* samples per channel */

    /* Pre-skip: drop from the front of decoder output, a plain sample
       count per RFC 7845 S4.2 -- never derived from granule math. */
    unsigned skip = r->pre_skip_remaining;
    if (skip > n)
        skip = n;
    r->pre_skip_remaining -= skip;

    unsigned usable = n - skip;
    const pj_int16_t *src = r->decode_buf + (size_t)skip * r->channels;

    if (op.e_o_s) {
        /* The one place a granule position is consulted: RFC 7845
           S4.3's total duration, final_granule - pre_skip, converted
           to this decoder's own clock_rate. Anything usable beyond
           that is the writer's end-of-stream silence padding
           (writer_finalize), not audio, and is trimmed here per RFC
           7845 S4.4 (End Trimming) rather than played back. A file
           with no valid granule on its last page (op.granulepos ==
           -1, malformed) is trusted for nothing beyond what was
           actually decoded -- no trim. */
        pj_uint64_t emitted_now = atomic_load(&r->emitted_at_rate);
        if (op.granulepos >= 0) {
            pj_uint64_t total_real_48k =
                (pj_uint64_t)op.granulepos >= r->pre_skip_48k
                    ? (pj_uint64_t)op.granulepos - r->pre_skip_48k
                    : 0;
            pj_uint64_t total_real_at_rate =
                (total_real_48k * r->clock_rate +
                SP_OGGOPUS_GRANULE_RATE / 2) /
                SP_OGGOPUS_GRANULE_RATE;
            pj_uint64_t allowed = total_real_at_rate > emitted_now
                                      ? total_real_at_rate - emitted_now
                                      : 0;
            if (allowed < usable)
                usable = (unsigned)allowed;
            r->total_real_at_rate = total_real_at_rate;
        } else {
            r->total_real_at_rate = emitted_now + usable;
        }
        r->saw_eos = PJ_TRUE;
    }

    if (usable > 0) {
        queue_samples(r, src, usable * r->channels);
        atomic_store(&r->emitted_at_rate,
                     atomic_load(&r->emitted_at_rate) + usable);
    }
    return PJ_TRUE;
}

static pj_status_t reader_get_frame(pjmedia_port *this_port,
                                    pjmedia_frame *frame) {
    sp_oggopus_reader *r = (sp_oggopus_reader *)this_port;

    unsigned want =
        (unsigned)(frame->size / sizeof(pj_int16_t)); /* interleaved */
    pj_int16_t *dst = (pj_int16_t *)frame->buf;
    unsigned written = 0;

    while (written < want) {
        unsigned avail = r->out_fill - r->out_pos;
        if (avail > 0) {
            unsigned take = want - written;
            if (take > avail)
                take = avail;
            memcpy(dst + written, r->out_buf + r->out_pos,
                  (size_t)take * sizeof(pj_int16_t));
            r->out_pos += take;
            written += take;
            continue;
        }
        if (r->saw_eos)
            break;
        if (!decode_one_packet(r) && !r->saw_eos)
            break; /* defensive: decode_one_packet always sets it */
    }

    if (written < want)
        memset(dst + written, 0, (size_t)(want - written) * sizeof(pj_int16_t));

    if (written == 0 && r->saw_eos && r->out_fill == r->out_pos) {
        frame->type = PJMEDIA_FRAME_TYPE_NONE;
        atomic_store(&r->eof, PJ_TRUE);
    } else {
        frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
    }
    return PJ_SUCCESS;
}

static pj_status_t reader_on_destroy(pjmedia_port *this_port) {
    sp_oggopus_reader *r = (sp_oggopus_reader *)this_port;
    if (r->dec)
        opus_decoder_destroy(r->dec);
    if (r->os_inited)
        ogg_stream_clear(&r->os);
    if (r->oy_inited)
        ogg_sync_clear(&r->oy);
    if (r->f)
        fclose(r->f);
    pj_pool_release(r->pool);
    return PJ_SUCCESS;
}

sp_status sp_oggopus_reader_create(pj_pool_t *pool_, const char *path,
                                   unsigned clock_rate, unsigned channels,
                                   unsigned samples_per_frame,
                                   pjmedia_port **out) {
    if (!pool_ || !path || !out)
        return SP_ERR_INVALID_ARG;
    if (channels < 1 || channels > SP_OGGOPUS_MAX_CHANNELS)
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
        pj_pool_create(pool_->factory, "oggopus_reader", 1000, 1000, NULL);
    if (!pool)
        return SP_ERR_INTERNAL;

    sp_oggopus_reader *r = PJ_POOL_ZALLOC_T(pool, sp_oggopus_reader);
    if (!r) {
        pj_pool_release(pool);
        return SP_ERR_INTERNAL;
    }
    r->pool = pool;
    r->clock_rate = clock_rate;
    r->channels = channels;
    atomic_init(&r->eof, PJ_FALSE);

    r->decode_buf = pj_pool_alloc(
        pool, (pj_size_t)SP_OGGOPUS_MAX_FRAME_SAMPLES * channels *
                  sizeof(pj_int16_t));
    r->out_cap = SP_OGGOPUS_MAX_FRAME_SAMPLES * channels;
    r->out_buf =
        pj_pool_alloc(pool, (pj_size_t)r->out_cap * sizeof(pj_int16_t));
    if (!r->decode_buf || !r->out_buf) {
        pj_pool_release(pool);
        return SP_ERR_INTERNAL;
    }

    r->f = fopen(path, "rb");
    if (!r->f) {
        pj_pool_release(pool);
        return SP_ERR_IO;
    }

    ogg_sync_init(&r->oy);
    r->oy_inited = PJ_TRUE;

    if (parse_headers(r) != PJ_SUCCESS) {
        if (r->os_inited)
            ogg_stream_clear(&r->os);
        ogg_sync_clear(&r->oy);
        fclose(r->f);
        pj_pool_release(pool);
        return SP_ERR_MEDIA;
    }

    int opus_err = 0;
    r->dec = opus_decoder_create((opus_int32)clock_rate, (int)channels,
                                 &opus_err);
    if (opus_err != OPUS_OK || !r->dec) {
        if (r->os_inited)
            ogg_stream_clear(&r->os);
        ogg_sync_clear(&r->oy);
        fclose(r->f);
        pj_pool_release(pool);
        return SP_ERR_MEDIA;
    }

    r->base.get_frame = &reader_get_frame;
    r->base.on_destroy = &reader_on_destroy;
    pj_str_t name = pj_str((char *)"oggopus_reader");
    pjmedia_port_info_init(&r->base.info, &name,
                           PJMEDIA_SIG_CLASS_PORT_AUD('O', 'R'), clock_rate,
                           channels, 16, samples_per_frame);

    *out = &r->base;
    return SP_OK;
}

sp_status sp_oggopus_reader_is_eof(pjmedia_port *port, pj_bool_t *out) {
    if (!port || !out)
        return SP_ERR_INVALID_ARG;
    sp_oggopus_reader *r = (sp_oggopus_reader *)port;
    *out = atomic_load(&r->eof);
    return SP_OK;
}

sp_status sp_oggopus_reader_get_pos_ms(pjmedia_port *port, uint32_t *out) {
    if (!port || !out)
        return SP_ERR_INVALID_ARG;
    sp_oggopus_reader *r = (sp_oggopus_reader *)port;
    /* emitted_at_rate already excludes pre-skip (queue_samples only
       ever sees the post-pre-skip tail of each decode), so position
       is a plain rate conversion, not an offset. atomic_load because
       this runs on the caller's own thread while reader_get_frame
       writes emitted_at_rate from the media thread, with no other
       synchronisation between them -- same shape as the eof flag. */
    pj_uint64_t emitted = atomic_load(&r->emitted_at_rate);
    *out = (uint32_t)((emitted * 1000 + r->clock_rate / 2) / r->clock_rate);
    return SP_OK;
}

sp_status sp_oggopus_duration_ms(const char *path, uint32_t *out) {
    if (!path || !out)
        return SP_ERR_INVALID_ARG;
    *out = 0;

    FILE *f = fopen(path, "rb");
    if (!f)
        return SP_ERR_IO;

    ogg_sync_state oy;
    ogg_sync_init(&oy);

    unsigned pre_skip = 0;
    pj_bool_t got_head = PJ_FALSE;
    pj_bool_t any_page = PJ_FALSE;
    pj_bool_t have_serial = PJ_FALSE;
    long serialno = 0;
    ogg_int64_t last_granule = 0;

    for (;;) {
        ogg_page og;
        int rc = ogg_sync_pageout(&oy, &og);
        if (rc == 0) {
            char *buf = ogg_sync_buffer(&oy, SP_OGGOPUS_READ_CHUNK);
            if (!buf)
                break;
            size_t n = fread(buf, 1, SP_OGGOPUS_READ_CHUNK, f);
            if (ogg_sync_wrote(&oy, (long)n) != 0)
                break;
            if (n == 0)
                break;
            continue;
        }
        if (rc == -1)
            continue;

        long serial = ogg_page_serialno(&og);
        if (!have_serial) {
            serialno = serial;
            have_serial = PJ_TRUE;
        } else if (serial != serialno) {
            continue; /* a foreign logical stream; not this file's own */
        }

        any_page = PJ_TRUE;
        last_granule = ogg_page_granulepos(&og);

        /* RFC 7845 S3: the identification header is alone on the
           stream's first page, so a BOS page's body is exactly the
           OpusHead packet -- read straight off it rather than
           standing up ogg_stream_state and opus for a single field. */
        if (!got_head && ogg_page_bos(&og) && og.body_len >= 19 &&
           memcmp(og.body, "OpusHead", 8) == 0) {
            pre_skip =
                (unsigned)og.body[10] | ((unsigned)og.body[11] << 8);
            got_head = PJ_TRUE;
        }
    }

    ogg_sync_clear(&oy);
    fclose(f);

    if (!any_page || !got_head)
        return SP_ERR_MEDIA;

    ogg_int64_t dur48k = last_granule - (ogg_int64_t)pre_skip;
    if (dur48k < 0)
        dur48k = 0;
    *out = (uint32_t)(((pj_uint64_t)dur48k * 1000 +
                       SP_OGGOPUS_GRANULE_RATE / 2) /
                      SP_OGGOPUS_GRANULE_RATE);
    return SP_OK;
}
