#include "core_internal.h"

#include "audioname.h"

#include <pjmedia-codec/opus.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

sp_status sp_media_enum_codecs(sp_core *core, const char ***names,
                               size_t *n_out) {
    SP_ENTRY(core);
    if (!names || !n_out)
        return SP_ERR_INVALID_ARG;
    if (!core->pj_ready)
        return SP_ERR_STATE;

    pjsua_codec_info info[64];
    unsigned count = (unsigned)(sizeof info / sizeof info[0]);
    pj_status_t st = pjsua_enum_codecs(info, &count);
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_MEDIA, "pjsua_enum_codecs");

    free((void *)core->codec_names);
    core->codec_names = NULL;
    free(core->codec_params);
    core->codec_params = NULL;
    core->n_codec_params = 0;
    core->n_codec_names = 0;
    if (count == 0) {
        *names = NULL;
        *n_out = 0;
        return SP_OK;
    }

    /* One block: pointer array followed by the strings it points at. */
    size_t bytes = count * sizeof(char *);
    for (unsigned i = 0; i < count; i++)
        bytes += (size_t)info[i].codec_id.slen + 1;

    char **arr = malloc(bytes);
    if (!arr)
        return SP_ERR_INTERNAL;
    char *text = (char *)(arr + count);
    for (unsigned i = 0; i < count; i++) {
        int len = (int)info[i].codec_id.slen;
        memcpy(text, info[i].codec_id.ptr, (size_t)len);
        text[len] = '\0';
        arr[i] = text;
        text += len + 1;
    }

    core->codec_names = (const char **)arr;
    core->n_codec_names = count;
    *names = core->codec_names;
    *n_out = count;
    return SP_OK;
}

/* Frees the enumerated device list. One allocation holds the array and
   every string it points at, matching sp_media_enum_codecs. */
static void free_audio_devices(sp_core *core) {
    free(core->audio_devices);
    core->audio_devices = NULL;
    core->n_audio_devices = 0;
}

sp_status sp_media_enum_audio_devices(sp_core *core,
                                      const sp_audio_device **out,
                                      size_t *n_out) {
    SP_ENTRY(core);
    if (!out || !n_out)
        return SP_ERR_INVALID_ARG;
    *out = NULL;
    *n_out = 0;
    if (!core->pj_ready)
        return SP_ERR_STATE;

    pjmedia_aud_dev_info info[64];
    unsigned count = (unsigned)(sizeof info / sizeof info[0]);
    pj_status_t st = pjsua_enum_aud_devs(info, &count);
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_MEDIA, "pjsua_enum_aud_devs");

    free_audio_devices(core);
    if (count == 0)
        return SP_OK;

    /* The label is generated, so its length has to be measured before
       the block is sized. 128 is what sp_audio_label writes into. */
    char labels[64][128];
    for (unsigned i = 0; i < count; i++)
        sp_audio_label(info[i].name, labels[i], sizeof labels[i]);

    size_t bytes = count * sizeof(sp_audio_device);
    for (unsigned i = 0; i < count; i++)
        bytes += strlen(info[i].name) + 1 + strlen(info[i].driver) + 1 +
                 strlen(labels[i]) + 1;

    sp_audio_device *arr = malloc(bytes);
    if (!arr)
        return SP_ERR_INTERNAL;
    char *text = (char *)(arr + count);
    for (unsigned i = 0; i < count; i++) {
        size_t nlen = strlen(info[i].name);
        size_t dlen = strlen(info[i].driver);
        memcpy(text, info[i].name, nlen + 1);
        arr[i].name = text;
        text += nlen + 1;
        memcpy(text, info[i].driver, dlen + 1);
        arr[i].driver = text;
        text += dlen + 1;
        size_t llen = strlen(labels[i]);
        memcpy(text, labels[i], llen + 1);
        arr[i].label = text;
        text += llen + 1;
        arr[i].index = (int)info[i].id;
        arr[i].input_count = info[i].input_count;
        arr[i].output_count = info[i].output_count;
        arr[i].default_rate_hz = info[i].default_samples_per_sec;
        arr[i].kind = sp_audio_classify(info[i].name);
        /* A device with no channels in either direction cannot carry a
           call, whatever its name suggests. The legacy "oss" bridge
           enumerates that way on a modern Debian box. */
        arr[i].recommended = sp_audio_is_recommended(info[i].name) &&
                             (info[i].input_count > 0 ||
                              info[i].output_count > 0);
    }

    core->audio_devices = arr;
    core->n_audio_devices = count;
    *out = arr;
    *n_out = count;
    return SP_OK;
}

/* pjmedia's device id for a name, or one of the two sentinels.
   SP_DEV_UNKNOWN says the operator named something that is not here,
   which is reported rather than silently replaced with the default. */
#define SP_DEV_UNKNOWN (-9999)

static int dev_id_for(const char *name, bool capture) {
    if (!name || !*name || strcmp(name, "default") == 0)
        return capture ? PJMEDIA_AUD_DEFAULT_CAPTURE_DEV
                       : PJMEDIA_AUD_DEFAULT_PLAYBACK_DEV;

    pjmedia_aud_dev_info info[64];
    unsigned count = (unsigned)(sizeof info / sizeof info[0]);
    if (pjsua_enum_aud_devs(info, &count) != PJ_SUCCESS)
        return SP_DEV_UNKNOWN;
    for (unsigned i = 0; i < count; i++) {
        if (strcmp(info[i].name, name) != 0)
            continue;
        /* A playback-only device cannot capture, and choosing it would
           fail later inside pjsua with a less useful message. */
        if (capture && info[i].input_count == 0)
            return SP_DEV_UNKNOWN;
        if (!capture && info[i].output_count == 0)
            return SP_DEV_UNKNOWN;
        return (int)info[i].id;
    }
    return SP_DEV_UNKNOWN;
}

static void remember(char *dst, size_t cap, const char *name) {
    snprintf(dst, cap, "%s", (name && *name) ? name : "default");
}

sp_status sp_media_get_audio_devices(sp_core *core, const char **capture,
                                     const char **playback) {
    SP_ENTRY(core);
    if (!core->audio_capture[0])
        remember(core->audio_capture, sizeof core->audio_capture, NULL);
    if (!core->audio_playback[0])
        remember(core->audio_playback, sizeof core->audio_playback, NULL);
    if (capture)
        *capture = core->audio_capture;
    if (playback)
        *playback = core->audio_playback;
    return SP_OK;
}

/* Applies the pair without persisting. Both names may be "none", which
   selects the null device for the whole conference bridge: pjsua has
   no per-direction null. */
static sp_status apply_devices(sp_core *core, const char *capture,
                               const char *playback) {
    const bool cap_none = capture && strcmp(capture, "none") == 0;
    const bool play_none = playback && strcmp(playback, "none") == 0;
    if (cap_none || play_none) {
        pj_status_t st = pjsua_set_null_snd_dev();
        if (st != PJ_SUCCESS)
            return sp_core_fail(core, st, SP_ERR_MEDIA,
                                "pjsua_set_null_snd_dev");
        return SP_OK;
    }

    int cid = dev_id_for(capture, true);
    if (cid == SP_DEV_UNKNOWN) {
        sp_core_detail(core, "no capture device named \"%s\"",
                       capture ? capture : "");
        return SP_ERR_MEDIA;
    }
    int pid = dev_id_for(playback, false);
    if (pid == SP_DEV_UNKNOWN) {
        sp_core_detail(core, "no playback device named \"%s\"",
                       playback ? playback : "");
        return SP_ERR_MEDIA;
    }

    pj_status_t st = pjsua_set_snd_dev(cid, pid);
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_MEDIA, "pjsua_set_snd_dev");
    return SP_OK;
}

sp_status sp_media_set_audio_devices(sp_core *core, const char *capture,
                                     const char *playback) {
    SP_ENTRY(core);
    if (!core->pj_ready)
        return SP_ERR_STATE;

    /* NULL means "leave that side alone", so read the current value
       rather than resetting it to the default. */
    const char *cur_cap = NULL, *cur_play = NULL;
    sp_media_get_audio_devices(core, &cur_cap, &cur_play);
    char want_cap[128], want_play[128];
    remember(want_cap, sizeof want_cap, capture ? capture : cur_cap);
    remember(want_play, sizeof want_play, playback ? playback : cur_play);

    sp_status s = apply_devices(core, want_cap, want_play);
    if (s != SP_OK)
        return s;

    remember(core->audio_capture, sizeof core->audio_capture, want_cap);
    remember(core->audio_playback, sizeof core->audio_playback, want_play);

    /* Persisted, so the choice survives a restart. A failure to write
       the config is reported, but the device is already in use: the
       operator hears the change either way. */
    pthread_mutex_lock(&core->lock);
    free(core->cfg->audio_capture_device);
    free(core->cfg->audio_playback_device);
    core->cfg->audio_capture_device = strdup(core->audio_capture);
    core->cfg->audio_playback_device = strdup(core->audio_playback);
    pthread_mutex_unlock(&core->lock);
    return sp_core_persist(core);
}

sp_status sp_media_set_audio_device(sp_core *core, const char *name) {
    /* The older single-name entry point: both directions at once, and
       deliberately without persisting. sp_core_start calls this with
       what the config already says, and writing the config back during
       startup would be both pointless and a way to corrupt it. */
    SP_ENTRY(core);
    if (!core->pj_ready)
        return SP_ERR_STATE;
    const char *want = (name && *name) ? name : "default";
    sp_status s = apply_devices(core, want, want);
    if (s != SP_OK)
        return s;
    remember(core->audio_capture, sizeof core->audio_capture, want);
    remember(core->audio_playback, sizeof core->audio_playback, want);
    return SP_OK;
}

/* Applies what the configuration asks for, without writing it back.
   Called once from sp_core_start. */
sp_status sp_media_apply_config(sp_core *core) {
    if (sp_config_wants_no_audio(core->cfg))
        return sp_media_set_audio_device(core, "none");

    const char *cap = core->cfg->audio_capture_device;
    const char *play = core->cfg->audio_playback_device;
    if (!cap && !play && !core->cfg->audio_device)
        return SP_OK;          /* system default; nothing to do */
    if (!cap && !play)
        return sp_media_set_audio_device(core, core->cfg->audio_device);

    sp_status s = apply_devices(core, cap ? cap : "default",
                               play ? play : "default");
    if (s != SP_OK)
        return s;
    remember(core->audio_capture, sizeof core->audio_capture, cap);
    remember(core->audio_playback, sizeof core->audio_playback, play);
    return SP_OK;
}

void sp_media_free_devices(sp_core *core) {
    free_audio_devices(core);
}

sp_status sp_media_set_codec_priority(sp_core *core, const char *codec_id,
                                      int priority) {
    SP_ENTRY(core);
    if (!codec_id || !*codec_id || priority < 0 || priority > 255)
        return SP_ERR_INVALID_ARG;
    if (!core->pj_ready)
        return SP_ERR_STATE;
    pj_str_t id = pj_str((char *)codec_id);
    pj_status_t st = pjsua_codec_set_priority(&id, (pj_uint8_t)priority);
    if (st != PJ_SUCCESS) {
        char what[192];
        snprintf(what, sizeof what, "pjsua_codec_set_priority for \"%s\"",
                 codec_id);
        return sp_core_fail(core, st, SP_ERR_MEDIA, what);
    }
    return SP_OK;
}

/* --- audio processing ------------------------------------------------ */

/* The echo-canceller option word for a configuration. The algorithm is
   left at PJMEDIA_ECHO_DEFAULT so pjmedia picks the best one it was
   built with; only the extras are ours to choose. */
unsigned sp_media_ec_options(const sp_config *cfg) {
    unsigned opt = PJMEDIA_ECHO_DEFAULT;
    if (cfg->noise_suppression)
        opt |= PJMEDIA_ECHO_USE_NOISE_SUPPRESSOR;
    if (cfg->auto_gain)
        opt |= PJMEDIA_ECHO_USE_GAIN_CONTROLLER;
    return opt;
}

/* Writes vad, cng and plc into every enabled codec. A codec's
   parameters are fixed once its stream exists, so this changes the
   next call rather than the current one. */
static sp_status apply_codec_settings(sp_core *core) {
    pjsua_codec_info info[64];
    unsigned count = (unsigned)(sizeof info / sizeof info[0]);
    pj_status_t st = pjsua_enum_codecs(info, &count);
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_MEDIA, "pjsua_enum_codecs");

    for (unsigned i = 0; i < count; i++) {
        pjmedia_codec_param prm;
        if (pjsua_codec_get_param(&info[i].codec_id, &prm) != PJ_SUCCESS)
            continue;   /* a codec that will not answer is not fatal */
        prm.setting.vad = core->cfg->silence_suppression ? 1 : 0;
        prm.setting.cng = core->cfg->comfort_noise ? 1 : 0;
        prm.setting.plc = core->cfg->packet_loss_concealment ? 1 : 0;
        /* Best effort per codec: a codec that refuses one of these
           must not stop the others being set. */
        pjsua_codec_set_param(&info[i].codec_id, &prm);
    }
    return SP_OK;
}

/* Everything the stack offers, merged with what the config says about
   each, in the order they are offered.
 *
 * Capability comes from the codec itself: pjmedia fills the settings
 * it supports when the param is fetched, so a frontend can grey out a
 * control instead of offering one that does nothing. Bitrate is the
 * exception -- it lives in the Opus config and nowhere else, so it is
 * a property of being Opus rather than something to ask about. */
sp_status sp_media_get_codecs(sp_core *core, const sp_codec_params **out,
                              size_t *n_out) {
    SP_ENTRY(core);
    if (!out || !n_out)
        return SP_ERR_INVALID_ARG;
    if (!core->pj_ready)
        return SP_ERR_STATE;

    pjsua_codec_info info[64];
    unsigned count = (unsigned)(sizeof info / sizeof info[0]);
    pj_status_t pst = pjsua_enum_codecs(info, &count);
    if (pst != PJ_SUCCESS)
        return sp_core_fail(core, pst, SP_ERR_MEDIA, "pjsua_enum_codecs");

    free(core->codec_params);
    core->codec_params = calloc(count ? count : 1, sizeof *core->codec_params);
    if (!core->codec_params) {
        core->n_codec_params = 0;
        return SP_ERR_INTERNAL;
    }
    core->n_codec_params = count;

    for (unsigned i = 0; i < count; i++) {
        sp_codec_params *p = &core->codec_params[i];
        /* pjsua's own storage for the id outlives this call, and the
           whole array is invalidated on the next one anyway. */
        p->id = info[i].codec_id.ptr;
        p->priority = info[i].priority;
        p->enabled = info[i].priority > 0;
        p->bitrate = -1;
        p->complexity = -1;
        p->expected_loss_pct = -1;
        p->ptime_ms = -1;
        p->cbr = -1;

        pjmedia_codec_param prm;
        if (pjsua_codec_get_param(&info[i].codec_id, &prm) == PJ_SUCCESS) {
            p->clock_rate = (int)prm.info.clock_rate;
            p->channel_cnt = (int)prm.info.channel_cnt;
            p->frame_ptime_ms = prm.info.frm_ptime ? prm.info.frm_ptime : 20;
            p->supports_complexity = true;
            p->supports_expected_loss = true;
            p->supports_cbr = true;
            p->supports_ptime = prm.info.frm_ptime > 0;
        }
        p->supports_bitrate =
            strncasecmp(info[i].codec_id.ptr, "opus", 4) == 0
            && (size_t)info[i].codec_id.slen >= 4;

        /* What the operator actually chose, if this codec is named in
           the config. Matched the way pjsua matches: on a prefix, so
           "opus" finds "opus/48000/2". */
        for (size_t j = 0; j < core->cfg->n_codecs; j++) {
            const sp_codec_cfg *c = &core->cfg->codecs[j];
            const size_t len = strlen(c->id);
            if ((size_t)info[i].codec_id.slen < len
                || strncasecmp(info[i].codec_id.ptr, c->id, len) != 0)
                continue;
            p->bitrate = c->bitrate;
            p->complexity = c->complexity;
            p->expected_loss_pct = c->expected_loss_pct;
            p->ptime_ms = c->ptime_ms;
            p->cbr = c->cbr;
            break;
        }
    }
    *out = core->codec_params;
    *n_out = core->n_codec_params;
    return SP_OK;
}

/* Writes one codec's own parameters into the media stack.
 *
 * Two paths, because Opus has two homes for its settings. Bit rate
 * lives only in pjmedia_codec_opus_config, and the documented way to
 * apply that config is pjmedia_codec_opus_set_default_param, which
 * takes the codec param with it and replaces the generic setter --
 * there is no pjmedia_codec_opus_set_config. Every other codec goes
 * through pjsua_codec_set_param.
 */
sp_status sp_media_apply_codec_params(sp_core *core, const sp_codec_cfg *c) {
    if (!c->enabled)
        return SP_OK;   /* nothing to configure on a codec never offered */

    const bool is_opus = strncasecmp(c->id, "opus", 4) == 0;
    const bool generic_unset = c->complexity < 0 && c->expected_loss_pct < 0
                               && c->ptime_ms < 0 && c->cbr < 0;
    if (generic_unset && !(is_opus && c->bitrate >= 0))
        return SP_OK;   /* the operator chose nothing for this codec */

    pj_str_t id = pj_str(c->id);
    pjmedia_codec_param prm;
    pj_status_t st = pjsua_codec_get_param(&id, &prm);
    if (st != PJ_SUCCESS) {
        char what[160];
        snprintf(what, sizeof what, "pjsua_codec_get_param for \"%s\"", c->id);
        return sp_core_fail(core, st, SP_ERR_MEDIA, what);
    }

    if (c->complexity >= 0)
        prm.setting.complexity = (unsigned)c->complexity;
    if (c->expected_loss_pct >= 0)
        prm.setting.packet_loss = (unsigned)c->expected_loss_pct;
    if (c->cbr >= 0)
        prm.setting.cbr = c->cbr ? PJ_TRUE : PJ_FALSE;

    /* ptime is frames per packet in pjmedia and milliseconds to an
       operator reading a SIP trace. The codec's own frame time is the
       conversion, and a request that is not a whole number of frames
       is refused rather than rounded into something else. */
    if (c->ptime_ms >= 0) {
        const unsigned frm = prm.info.frm_ptime ? prm.info.frm_ptime : 20;
        if ((unsigned)c->ptime_ms % frm != 0) {
            char what[200];
            snprintf(what, sizeof what,
                     "ptime_ms %d for \"%s\": must be a multiple of the "
                     "codec's %u ms frame", c->ptime_ms, c->id, frm);
            return sp_core_fail(core, PJ_EINVAL, SP_ERR_CONFIG, what);
        }
        prm.setting.frm_per_pkt = (pj_uint8_t)((unsigned)c->ptime_ms / frm);
    }

#if PJMEDIA_HAS_OPUS_CODEC
    if (is_opus) {
        pjmedia_codec_opus_config ocfg;
        st = pjmedia_codec_opus_get_config(&ocfg);
        if (st != PJ_SUCCESS)
            return sp_core_fail(core, st, SP_ERR_MEDIA,
                                "pjmedia_codec_opus_get_config");
        if (c->bitrate >= 0)
            ocfg.bit_rate = (unsigned)c->bitrate;
        if (c->complexity >= 0)
            ocfg.complexity = (unsigned)c->complexity;
        if (c->expected_loss_pct >= 0)
            ocfg.packet_loss = (unsigned)c->expected_loss_pct;
        if (c->cbr >= 0)
            ocfg.cbr = c->cbr ? PJ_TRUE : PJ_FALSE;
        if (c->ptime_ms >= 0)
            ocfg.frm_ptime = (unsigned)c->ptime_ms;
        st = pjmedia_codec_opus_set_default_param(&ocfg, &prm);
        if (st != PJ_SUCCESS)
            return sp_core_fail(core, st, SP_ERR_MEDIA,
                                "pjmedia_codec_opus_set_default_param");
        return SP_OK;
    }
#endif

    if (c->bitrate >= 0) {
        char what[200];
        snprintf(what, sizeof what,
                 "bitrate is settable on opus only, not on \"%s\"", c->id);
        return sp_core_fail(core, PJ_EINVAL, SP_ERR_CONFIG, what);
    }

    st = pjsua_codec_set_param(&id, &prm);
    if (st != PJ_SUCCESS) {
        char what[160];
        snprintf(what, sizeof what, "pjsua_codec_set_param for \"%s\"", c->id);
        return sp_core_fail(core, st, SP_ERR_MEDIA, what);
    }
    return SP_OK;
}

/* Applies and persists a whole codec list.
 *
 * Validated first, in full. A list that is going to be refused must
 * leave the media stack and the config exactly as they were, rather
 * than applying the entries that happened to come before the bad one.
 */
sp_status sp_media_set_codecs(sp_core *core, const sp_codec_params *in,
                              size_t n) {
    SP_ENTRY(core);
    if (!in && n)
        return SP_ERR_INVALID_ARG;
    if (!core->pj_ready)
        return SP_ERR_STATE;

    for (size_t i = 0; i < n; i++) {
        const sp_codec_params *p = &in[i];
        if (!p->id || !*p->id)
            return SP_ERR_INVALID_ARG;
        if (p->complexity > 10 || p->expected_loss_pct > 100
            || p->bitrate > 512000 || p->ptime_ms > 200 || p->cbr > 1
            || p->complexity < -1 || p->expected_loss_pct < -1
            || p->bitrate < -1 || p->ptime_ms < -1 || p->cbr < -1)
            return SP_ERR_INVALID_ARG;
        if (p->bitrate >= 0 && strncasecmp(p->id, "opus", 4) != 0) {
            /* Straight through the format, not via a buffer:
               sp_core_detail is printf-style, so a codec id carrying
               a percent sign would be read as a conversion. */
            sp_core_detail(core,
                           "bitrate is settable on opus only, not on \"%s\"",
                           p->id);
            return SP_ERR_INVALID_ARG;
        }
        for (size_t j = i + 1; j < n; j++)
            if (in[j].id && strcasecmp(p->id, in[j].id) == 0)
                return SP_ERR_INVALID_ARG;
    }

    sp_codec_cfg *fresh = n ? calloc(n, sizeof *fresh) : NULL;
    if (n && !fresh)
        return SP_ERR_INTERNAL;
    for (size_t i = 0; i < n; i++) {
        fresh[i].id = strdup(in[i].id);
        if (!fresh[i].id) {
            for (size_t j = 0; j < i; j++)
                free(fresh[j].id);
            free(fresh);
            return SP_ERR_INTERNAL;
        }
        fresh[i].enabled = in[i].enabled;
        /* Order in the list is the offer order. The caller's priority
           field is what the stack reported; it is not what decides. */
        fresh[i].priority = in[i].enabled
                                ? (254 - (int)i > 1 ? 254 - (int)i : 1)
                                : 0;
        fresh[i].bitrate = in[i].bitrate;
        fresh[i].complexity = in[i].complexity;
        fresh[i].expected_loss_pct = in[i].expected_loss_pct;
        fresh[i].ptime_ms = in[i].ptime_ms;
        fresh[i].cbr = in[i].cbr;
    }

    for (size_t i = 0; i < n; i++) {
        pj_str_t id = pj_str(fresh[i].id);
        pj_status_t pst = pjsua_codec_set_priority(
            &id, (pj_uint8_t)fresh[i].priority);
        if (pst != PJ_SUCCESS) {
            char what[160];
            snprintf(what, sizeof what, "pjsua_codec_set_priority for \"%s\"",
                     fresh[i].id);
            for (size_t j = 0; j < n; j++)
                free(fresh[j].id);
            free(fresh);
            return sp_core_fail(core, pst, SP_ERR_MEDIA, what);
        }
        sp_status s = sp_media_apply_codec_params(core, &fresh[i]);
        if (s != SP_OK) {
            for (size_t j = 0; j < n; j++)
                free(fresh[j].id);
            free(fresh);
            return s;
        }
    }

    for (size_t i = 0; i < core->cfg->n_codecs; i++)
        free(core->cfg->codecs[i].id);
    free(core->cfg->codecs);
    core->cfg->codecs = fresh;
    core->cfg->n_codecs = n;
    return sp_core_persist(core);
}

/* Rebuilds the echo canceller from the configuration. A tail of zero
   is how pjsua is told to switch it off. */
static sp_status apply_ec(sp_core *core) {
    const unsigned tail = core->cfg->echo_cancel
                              ? (unsigned)core->cfg->echo_tail_ms
                              : 0u;
    pj_status_t st = pjsua_set_ec(tail, sp_media_ec_options(core->cfg));
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_MEDIA, "pjsua_set_ec");
    return SP_OK;
}

sp_status sp_media_apply_dsp(sp_core *core) {
    sp_status s = apply_ec(core);
    if (s != SP_OK)
        return s;
    return apply_codec_settings(core);
}

sp_status sp_media_get_dsp(sp_core *core, sp_audio_dsp *out) {
    SP_ENTRY(core);
    if (!out)
        return SP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&core->lock);
    out->echo_cancel = core->cfg->echo_cancel;
    out->echo_tail_ms = core->cfg->echo_tail_ms;
    out->noise_suppression = core->cfg->noise_suppression;
    out->auto_gain = core->cfg->auto_gain;
    out->silence_suppression = core->cfg->silence_suppression;
    out->comfort_noise = core->cfg->comfort_noise;
    out->packet_loss_concealment = core->cfg->packet_loss_concealment;
    pthread_mutex_unlock(&core->lock);
    return SP_OK;
}

sp_status sp_media_set_dsp(sp_core *core, const sp_audio_dsp *in) {
    SP_ENTRY(core);
    if (!in)
        return SP_ERR_INVALID_ARG;
    if (in->echo_tail_ms < 0 || in->echo_tail_ms > 500) {
        sp_core_detail(core, "echo_tail_ms must be 0..500, got %d",
                       in->echo_tail_ms);
        return SP_ERR_INVALID_ARG;
    }
    if (!core->pj_ready)
        return SP_ERR_STATE;

    pthread_mutex_lock(&core->lock);
    core->cfg->echo_cancel = in->echo_cancel;
    /* A tail of zero with the canceller on would mean "on but with no
       history", which is off with extra steps. Keep the default. */
    core->cfg->echo_tail_ms = in->echo_tail_ms > 0 ? in->echo_tail_ms : 200;
    core->cfg->noise_suppression = in->noise_suppression;
    core->cfg->auto_gain = in->auto_gain;
    core->cfg->silence_suppression = in->silence_suppression;
    core->cfg->comfort_noise = in->comfort_noise;
    core->cfg->packet_loss_concealment = in->packet_loss_concealment;
    pthread_mutex_unlock(&core->lock);

    sp_status s = sp_media_apply_dsp(core);
    if (s != SP_OK)
        return s;
    return sp_core_persist(core);
}

/* --- sounds ---------------------------------------------------------- */

static const char *slot_config(const sp_core *core, sp_sound_slot slot) {
    switch (slot) {
    case SP_SOUND_RINGTONE:     return core->cfg->ringtone;
    case SP_SOUND_RINGBACK:     return core->cfg->ringback;
    case SP_SOUND_NOTIFICATION: return core->cfg->notification_sound;
    }
    return NULL;
}

static char **slot_config_slot(sp_core *core, sp_sound_slot slot) {
    switch (slot) {
    case SP_SOUND_RINGTONE:     return &core->cfg->ringtone;
    case SP_SOUND_RINGBACK:     return &core->cfg->ringback;
    case SP_SOUND_NOTIFICATION: return &core->cfg->notification_sound;
    }
    return NULL;
}

/* The configured value, or the library's default when nothing has been
   chosen. The default comes from sp_sound_lib_default rather than from
   a scan here: scanning would free the list sp_sound_list last handed
   a frontend. */
static const char *slot_value(sp_core *core, sp_sound_slot slot) {
    const char *v = slot_config(core, slot);
    if (v && *v)
        return v;
    return sp_sound_lib_default(core->sounds, slot);
}

void sp_sound_stop(sp_core *core, sp_sound_slot slot) {
    if (!core || (int)slot < 0 || (int)slot > 2)
        return;
    if (!core->pj_ready)
        return;
    if (slot == SP_SOUND_NOTIFICATION)
        pjsua_cancel_timer(&core->notification_timer);
    if (core->players[slot] == PJSUA_INVALID_ID)
        return;
    pjsua_player_destroy(core->players[slot]);
    core->players[slot] = PJSUA_INVALID_ID;
}

#define NOTIFICATION_MAX_MS 3000

static void notification_expired(pj_timer_heap_t *th, pj_timer_entry *e) {
    (void)th;
    sp_core *core = (sp_core *)e->user_data;
    if (core)
        sp_sound_stop(core, SP_SOUND_NOTIFICATION);
}

void sp_sound_timer_init(sp_core *core) {
    pj_timer_entry_init(&core->notification_timer, 0, core,
                        &notification_expired);
}

void sp_sound_stop_all(sp_core *core) {
    for (int i = 0; i < 3; i++)
        sp_sound_stop(core, (sp_sound_slot)i);
}

void sp_sound_start(sp_core *core, sp_sound_slot slot) {
    if (!core || !core->pj_ready || (int)slot < 0 || (int)slot > 2)
        return;
    /* Nothing to play into. Asking pjsua to play to a null device
       would work and be inaudible; skipping is honest and cheaper. */
    if (sp_config_wants_no_audio(core->cfg))
        return;

    bool silent = false;
    const char *path = sp_sound_lib_resolve(core->sounds, slot,
                                            slot_value(core, slot), &silent);
    if (silent || !path)
        return;

    sp_sound_stop(core, slot);

    /* The notification is a tone, not a ring: it plays once. */
    const unsigned opts = (slot == SP_SOUND_NOTIFICATION)
                              ? PJMEDIA_FILE_NO_LOOP : 0;
    pj_str_t f = pj_str((char *)path);
    pjsua_player_id id = PJSUA_INVALID_ID;
    if (pjsua_player_create(&f, opts, &id) != PJ_SUCCESS) {
        /* A sound that will not open must not stop a call ringing, so
           this is recorded and stepped over. */
        sp_actionlog_add(core, 2, "a configured sound could not be played");
        return;
    }
    /* To the speaker only. The call's own ports are untouched, so the
       far end never hears the ringtone. */
    if (pjsua_conf_connect(pjsua_player_get_conf_port(id), 0) != PJ_SUCCESS) {
        pjsua_player_destroy(id);
        return;
    }
    core->players[slot] = id;

    if (slot == SP_SOUND_NOTIFICATION) {
        pj_time_val delay = { NOTIFICATION_MAX_MS / 1000,
                              NOTIFICATION_MAX_MS % 1000 };
        /* Unscheduled, the tone plays to its end: louder for longer,
           never silent, so this is noted and not treated as a fault. */
        if (pjsua_schedule_timer(&core->notification_timer, &delay)
                != PJ_SUCCESS)
            sp_actionlog_add(core, 2,
                             "the call-failed tone could not be cut short");
    }
}

sp_status sp_sound_list(sp_core *core, sp_sound_slot slot,
                        const sp_sound_file **out, size_t *n_out) {
    SP_ENTRY(core);
    return sp_sound_lib_scan(core->sounds, slot, out, n_out);
}

sp_status sp_sound_get(sp_core *core, sp_sound_slot slot,
                       const char **value) {
    SP_ENTRY(core);
    if (!value || (int)slot < 0 || (int)slot > 2)
        return SP_ERR_INVALID_ARG;
    pthread_mutex_lock(&core->lock);
    *value = slot_value(core, slot);
    pthread_mutex_unlock(&core->lock);
    return SP_OK;
}

sp_status sp_sound_set(sp_core *core, sp_sound_slot slot,
                       const char *value) {
    SP_ENTRY(core);
    if (!value || !*value || (int)slot < 0 || (int)slot > 2)
        return SP_ERR_INVALID_ARG;

    bool silent = false;
    if (!sp_sound_lib_resolve(core->sounds, slot, value, &silent) && !silent) {
        sp_core_detail(core, "no sound named \"%s\" for %s", value,
                       sp_sound_slot_dir(slot));
        /* INVALID_ARG rather than NOT_FOUND: the generic text for
           NOT_FOUND is about retired handles, and "handle not found:
           no sound named ..." wastes the reader's time. */
        return SP_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&core->lock);
    char **store = slot_config_slot(core, slot);
    char *copy = strdup(value);
    if (copy) {
        free(*store);
        *store = copy;
    }
    pthread_mutex_unlock(&core->lock);
    if (!copy)
        return SP_ERR_INTERNAL;
    return sp_core_persist(core);
}

sp_status sp_sound_preview(sp_core *core, sp_sound_slot slot) {
    SP_ENTRY(core);
    if ((int)slot < 0 || (int)slot > 2)
        return SP_ERR_INVALID_ARG;
    if (!core->pj_ready)
        return SP_ERR_STATE;

    sp_sound_stop_all(core);

    bool silent = false;
    const char *path = sp_sound_lib_resolve(core->sounds, slot,
                                            slot_value(core, slot), &silent);
    if (silent || !path)
        return SP_OK;      /* nothing to hear is not a failure */

    pj_str_t f = pj_str((char *)path);
    pjsua_player_id id = PJSUA_INVALID_ID;
    /* Once, whatever the slot: a preview that looped would have to be
       stopped by hand every time. */
    if (pjsua_player_create(&f, PJMEDIA_FILE_NO_LOOP, &id) != PJ_SUCCESS)
        return sp_core_fail(core, PJ_EUNKNOWN, SP_ERR_MEDIA,
                            "pjsua_player_create");
    if (pjsua_conf_connect(pjsua_player_get_conf_port(id), 0) != PJ_SUCCESS) {
        pjsua_player_destroy(id);
        return SP_ERR_MEDIA;
    }
    core->players[slot] = id;
    return SP_OK;
}

sp_status sp_sound_preview_stop(sp_core *core) {
    SP_ENTRY(core);
    sp_sound_stop_all(core);
    return SP_OK;
}

sp_status sp_sound_stop_notification(sp_core *core) {
    SP_ENTRY(core);
    sp_sound_stop(core, SP_SOUND_NOTIFICATION);
    return SP_OK;
}

sp_status sp_sound_import(sp_core *core, sp_sound_slot slot,
                          const char *src_path, const char **stored_out) {
    SP_ENTRY(core);
    if (stored_out)
        *stored_out = NULL;
    char err[512] = {0};
    const char *stored = NULL;
    sp_status s = sp_sound_lib_import(core->sounds, slot, src_path, &stored,
                                      err, sizeof err);
    if (s != SP_OK) {
        sp_core_detail(core, "%s", err[0] ? err : "import failed");
        return s;
    }

    /* Proven playable before it is offered: the media stack is the
       only authority on whether a WAV is one it can read, and a file
       that fails at ring time is worse than one rejected now. */
    if (core->pj_ready) {
        pj_str_t f = pj_str((char *)stored);
        pjsua_player_id id = PJSUA_INVALID_ID;
        if (pjsua_player_create(&f, PJMEDIA_FILE_NO_LOOP, &id)
                != PJ_SUCCESS) {
            remove(stored);
            sp_core_detail(core,
                           "the media stack cannot play %s; it must be a "
                           "PCM WAV file", src_path);
            return SP_ERR_MEDIA;
        }
        pjsua_player_destroy(id);
    }

    if (stored_out)
        *stored_out = stored;
    return SP_OK;
}

/* --- levels ---------------------------------------------------------- */

/* Mute is runtime only, deliberately: see the note on sp_audio_levels. */
static bool g_tx_muted, g_rx_muted;

/* Applies both directions to the sound device's conference slot.
 *
 * The naming inverts here and it matters. pjsua's "tx level" for a
 * port is the signal the bridge sends TO that port, which for the
 * sound device is the speaker -- what you hear. Its "rx level" is the
 * signal the bridge receives FROM the port, which is the microphone --
 * what the far end hears. So our tx is pjsua's rx, and vice versa.
 */
static sp_status apply_levels(sp_core *core) {
    const float tx = g_tx_muted ? 0.0f
                                : (float)core->cfg->tx_volume_pct / 100.0f;
    const float rx = g_rx_muted ? 0.0f
                                : (float)core->cfg->rx_volume_pct / 100.0f;

    pj_status_t st = pjsua_conf_adjust_rx_level(0, tx);   /* microphone */
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_MEDIA,
                            "pjsua_conf_adjust_rx_level");
    st = pjsua_conf_adjust_tx_level(0, rx);               /* speaker */
    if (st != PJ_SUCCESS)
        return sp_core_fail(core, st, SP_ERR_MEDIA,
                            "pjsua_conf_adjust_tx_level");
    return SP_OK;
}

sp_status sp_media_apply_levels(sp_core *core) {
    return apply_levels(core);
}

sp_status sp_media_get_levels(sp_core *core, sp_audio_levels *out) {
    SP_ENTRY(core);
    if (!out)
        return SP_ERR_INVALID_ARG;
    pthread_mutex_lock(&core->lock);
    out->tx_level = (float)core->cfg->tx_volume_pct / 100.0f;
    out->rx_level = (float)core->cfg->rx_volume_pct / 100.0f;
    pthread_mutex_unlock(&core->lock);
    out->tx_muted = g_tx_muted;
    out->rx_muted = g_rx_muted;
    return SP_OK;
}

sp_status sp_media_set_levels(sp_core *core, const sp_audio_levels *in) {
    SP_ENTRY(core);
    if (!in)
        return SP_ERR_INVALID_ARG;
    /* Rejected rather than clamped: a frontend that computes a bad
       level should be told, not quietly obeyed at half volume. */
    if (in->tx_level < 0.0f || in->tx_level > 2.0f ||
        in->rx_level < 0.0f || in->rx_level > 2.0f) {
        sp_core_detail(core, "levels must be 0.0..2.0, got tx=%.2f rx=%.2f",
                       (double)in->tx_level, (double)in->rx_level);
        return SP_ERR_INVALID_ARG;
    }
    if (!core->pj_ready)
        return SP_ERR_STATE;

    const int tx_pct = (int)(in->tx_level * 100.0f + 0.5f);
    const int rx_pct = (int)(in->rx_level * 100.0f + 0.5f);

    pthread_mutex_lock(&core->lock);
    const bool changed = core->cfg->tx_volume_pct != tx_pct ||
                         core->cfg->rx_volume_pct != rx_pct;
    core->cfg->tx_volume_pct = tx_pct;
    core->cfg->rx_volume_pct = rx_pct;
    pthread_mutex_unlock(&core->lock);
    g_tx_muted = in->tx_muted;
    g_rx_muted = in->rx_muted;

    sp_status s = apply_levels(core);
    if (s != SP_OK)
        return s;
    /* Only the levels persist, so a mute on its own writes nothing:
       dragging a slider should not rewrite the config on every pixel
       either, hence the change check. */
    return changed ? sp_core_persist(core) : SP_OK;
}
