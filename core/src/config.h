/*
 * Typed configuration, mapped from the YAML node tree.
 *
 * Unknown keys are rejected rather than ignored: a silently dropped
 * typo in a header rule is the failure mode this project cannot
 * tolerate. Every error message names the key, the offending value
 * and the source line.
 *
 * Codec priority is a top-level setting, not per-account, because
 * pjsua_codec_set_priority is global in pjsua-lib and there is no
 * per-account equivalent to back a per-account list with.
 */
#ifndef SP_CONFIG_H
#define SP_CONFIG_H

/* The registration interval's default and bounds, in one place
   because two enforced them separately and disagreed: the config
   loader rejected anything outside this range while sp_account_add
   took whatever it was given, so an account added through the API
   with reg_expires_s left at zero produced a saved configuration the
   core then refused to load. Found by the Android bridge, which
   memsets the struct like any C caller would. */
#define SP_REG_EXPIRES_DEFAULT 300
#define SP_REG_EXPIRES_MIN     30
#define SP_REG_EXPIRES_MAX     86400


#include "comlink/core.h"
#include "hdr_rules.h"
#include "yaml_tree.h"

/* One codec's place in the offer and the settings that are its own.
   Every parameter is -1 for "the codec's own default": absent from
   the config means absent from the offer, not zero. */
typedef struct {
    char *id;
    bool  enabled;
    int   priority;             /* 0..255, 0 == disabled */
    int   bitrate;              /* bps; Opus only */
    int   complexity;           /* 0..10 */
    int   expected_loss_pct;    /* 0..100; >0 turns on Opus inband FEC */
    int   ptime_ms;
    int   cbr;                  /* -1 unset, 0 off, 1 on */
} sp_codec_cfg;

typedef struct {
    sp_account_cfg *accounts;
    size_t          n_accounts;

    sp_hdr_rule    *rules;
    size_t          n_rules;

    char           *audio_device;      /* NULL => system default */
    /* Per-direction device names. NULL => system default. Kept apart
       from audio_device because a tech routinely wants a headset mic
       with the speakers on the desk, and because "none" belongs to
       audio_device: it is a property of the whole phone, not of one
       direction. */
    char           *audio_capture_device;
    char           *audio_playback_device;

    /* TLS material. Paths are resolved by the frontend and handed in
       absolute, like the config path itself. */
    char           *tls_cert_file;     /* NULL => no client certificate */
    char           *tls_key_file;      /* NULL => key is inside the cert */
    char           *tls_ca_file;       /* NULL => system CA store */

    /* Logging. The database holds the SIP trace and the action log. */
    char           *db_file;           /* NULL => comlink.db beside config */
    bool            sip_log;           /* default true */
    bool            sip_log_redact;    /* default true */
    int             sip_log_max;       /* messages retained, default 5000 */

    /* Where recordings land and in what form. A NULL dir means
       "recordings" beside the configuration file, which is the one
       default that is right on a desktop and inside an Android
       application's private storage. */
    char                *recording_dir;
    sp_recording_format  recording_format;

    /* Where a trace export is written. NULL means "traces" beside the
       configuration file, for the same reason. */
    char                *sip_trace_export_dir;

    /* The speech model the transcript module uses: an absolute path to
       a whisper.cpp ggml file, or NULL for none. */
    char                *transcript_model;

    /* Audio quality sampling. 0 disables it entirely; otherwise the
       interval in milliseconds, between 100 and 60000. */
    int             metrics_interval_ms;

    /* Silence suppression. Off by default, unlike pjsua: a phone whose
       job is to measure a stream should keep sending one, or the
       jitter and loss figures come from a handful of packets. Set true
       to behave like an ordinary softphone. */
    bool            silence_suppression;

    /* The rest of the audio processing. Defaults suit an instrument:
       echo cancellation on because a speakerphone without it howls,
       everything that alters what you hear off unless asked for, and
       packet loss concealment on because hearing every lost packet is
       a deliberate choice rather than a default. */
    bool            echo_cancel;              /* default true */
    int             echo_tail_ms;             /* 0..500, default 200 */
    bool            noise_suppression;        /* default false */
    bool            auto_gain;                /* default false */
    bool            comfort_noise;            /* default true */
    bool            packet_loss_concealment;  /* default true */

    /* Volume, as a percentage: 100 is unity, 0 silence, 200 twice as
       loud. Percent rather than a float so the value round-trips
       through YAML exactly and needs no float parsing. */
    int             tx_volume_pct;     /* microphone, default 100 */
    int             rx_volume_pct;     /* speaker, default 100 */

    /* Sounds. A name from the sound library, an absolute path, or
       "none" for silence. NULL means the built-in default, which is
       the first bundled sound for that slot. */
    char           *ringtone;
    char           *ringback;
    char           *notification_sound;

    /* Theme. Overrides are NULL when absent, meaning "the preset's". */
    sp_theme_preset theme_preset;
    /* A preset name this build does not know -- written by a newer
       Comlink -- kept as it was read, so saving any other setting
       writes it back instead of quietly replacing it with the default
       the window is drawn in. Cleared when a theme is chosen. */
    char           *theme_preset_unknown;
    char           *theme_accent;
    char           *theme_font;
    sp_text_scale   theme_scale;

    /* Named sets of ad-hoc headers, plus header names seen before so
       an editor can offer completion. */
    sp_header_preset *presets;
    size_t            n_presets;
    char            **header_names;
    size_t            n_header_names;
    /* Remembered window size for a desktop frontend. 0 means "not set
       yet, use the frontend's default". Kept in the shared config so
       there is one configuration file rather than a Qt-specific second
       one; Android has no window and ignores both. */
    int             window_width;
    int             window_height;
    /* Whether the desktop frontend shows its activity pane. Here with
       the window size for the same reason: one configuration file. */
    bool            show_activity_log;
    /* Whether the call log panel is open. Persisted because
       the window's WIDTH already was: without this, reopening
       restored the widened window with nothing in the space
       that widened it. */
    bool            show_call_log;   /* default true */
    /* Record every call once it is answered. Top-level rather than
       under recording: an older Comlink refuses an unknown key inside
       a section and only warns about one at the top, and a setting
       must not stop the previous version starting. */
    bool            auto_record;     /* default false */

    int             log_level;         /* 0..5 */
    int             sip_port;          /* 0 => ephemeral */

    /* Highest priority first. codec_priority in a config file is read
       into this and written back out as codecs, which is the whole of
       the migration. */
    sp_codec_cfg   *codecs;
    size_t          n_codecs;

    /* Indexed by the registry's order, so adding a module cannot
       leave a config field behind. */
    bool            modules[8];
} sp_config;

/* False only when the config says so; anything unknown is on, which
   is what an old config without a modules section has to mean. */
bool sp_config_module_enabled(const sp_config *cfg, const char *id);

/* True when the operator asked for no audio device at all, by any of
   the three keys that can say so. */
bool sp_config_wants_no_audio(const sp_config *cfg);

sp_status sp_config_load(const char *path, sp_config **out,
                         char *err, size_t errlen);
void      sp_config_free(sp_config *cfg);

/* Writes cfg to path. Written via a temp file and rename, so a failure
   cannot leave a truncated config behind. Everything that load reads
   is written back, header rules included: anything that did not
   survive the round trip would be silently destroyed the first time a
   user edited an account. */
sp_status sp_config_save(const sp_config *cfg, const char *path,
                         char *err, size_t errlen);

/* Deep-copies cfg_in into the account list. */
sp_status sp_config_add_account(sp_config *cfg, const sp_account_cfg *cfg_in);

/* Removes by configured id. 0 on success, -1 if no such id. */
int sp_config_remove_account(sp_config *cfg, const char *id);

/* Borrowed pointer into the account list, or NULL. */
const sp_account_cfg *sp_config_find_account(const sp_config *cfg,
                                             const char *id);

void sp_config_presets_free(sp_header_preset *presets, size_t n);

/* Exposed for the test: how many unknown top-level keys a document
   carries. They are warned about and ignored rather than refused, so a
   config written by a newer Comlink does not stop an older one
   starting. See the comment at the call site in config.c. */
size_t sp_config_warn_unknown_top(const sp_yaml_node *map);

#endif /* SP_CONFIG_H */
