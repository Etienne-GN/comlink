/*
 * comlink core — public ABI.
 *
 * This header must never include a PJSIP type. It is compiled with no
 * pjproject include path by core/tests/test_abi_purity.c precisely so
 * that a leak breaks the build rather than the Android binding layer.
 *
 * Handle discipline: sp_account_id and sp_call_id are monotonically
 * increasing and never reused. 0 is always invalid. An operation on a
 * retired handle returns SP_ERR_NOT_FOUND.
 *
 * Threading: events are delivered only from the core's own dispatch
 * thread. Callbacks must not block, but may call back into sp_*.
 * Pointers in an event payload are valid for the callback's duration
 * only; copy anything you retain.
 *
 * One sp_core may be started per process. A second concurrent
 * sp_core_start returns SP_ERR_STATE.
 */
#ifndef COMLINK_CORE_H
#define COMLINK_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sp_core sp_core;
typedef uint64_t sp_account_id;   /* 0 = invalid */
typedef uint64_t sp_call_id;      /* 0 = invalid */

typedef enum {
    SP_OK = 0,
    SP_ERR_INVALID_ARG,
    SP_ERR_STATE,
    SP_ERR_NOT_FOUND,
    SP_ERR_TRANSPORT,
    SP_ERR_AUTH,
    SP_ERR_MEDIA,
    SP_ERR_CONFIG,
    SP_ERR_INTERNAL,
    /* Appended, never inserted: these numbers cross the ABI to the
       Android build. The capability exists but is switched off; the
       caller is told rather than left to discover the absence. */
    SP_ERR_DISABLED,
    /* A filesystem operation failed: the path itself was fine, the
       disk or permissions were not. Distinct from SP_ERR_CONFIG,
       which is a bad configured value rather than a bad filesystem. */
    SP_ERR_IO
} sp_status;

const char *sp_strerror(sp_status status);

/* Underlying PJSIP message for the last failure on this core, for logs.
   Never NULL; returns an empty string when nothing has failed. */
const char *sp_last_error_detail(sp_core *core);

typedef struct {
    const char *name;
    const char *value;
} sp_header;

typedef enum { SP_TP_UDP, SP_TP_TCP, SP_TP_TLS } sp_transport;

/* Three states, because that is what the media stack actually has.
   A boolean cannot express "do not offer SRTP at all", which some
   peers need and which is the only way to test that MANDATORY really
   refuses an unprotected peer. */
typedef enum {
    SP_SRTP_DISABLED,    /* never offered */
    SP_SRTP_OPTIONAL,    /* offered, plain RTP accepted */
    SP_SRTP_MANDATORY    /* required; the call fails without it */
} sp_srtp_mode;

typedef struct {
    bool        registered;
    int         reg_code;        /* last REGISTER response code */
    int         reg_expires_s;   /* granted expiry, may differ from asked */
    const char *detail;          /* failure reason, NULL when registered */
} sp_account_state;

typedef enum {
    SP_CALL_NULL,
    SP_CALL_CALLING,
    SP_CALL_INCOMING,
    SP_CALL_EARLY,
    SP_CALL_CONNECTING,
    SP_CALL_CONFIRMED,
    SP_CALL_DISCONNECTED
} sp_call_state;

typedef struct {
    sp_call_state  state;
    sp_account_id  acc;
    bool           on_hold;
    bool           recording;
    bool           inbound;
    int            last_code;    /* last SIP response code seen */
    const char    *remote_uri;     /* the full From/To value */
    const char    *remote_display; /* display name, "" if none */
    const char    *remote_user;    /* extension or number, "" if none */
    const char    *remote_label;   /* best label for a UI, never empty */
    const char    *local_uri;
    const char    *codec;          /* negotiated audio codec, NULL if none */
    uint32_t       duration_s;   /* since CONFIRMED */
} sp_call_info;

/* --- audio quality ---------------------------------------------------
 *
 * Sampled from the RTCP statistics pjmedia keeps for the call's audio
 * stream. Every counter here is what the media stack measured. The
 * only derived figures are r_factor and mos, and they are estimates:
 * see the note on mos below.
 */

/* A measured range, in microseconds. pjmedia accumulates min, max and
   mean itself, so these are its numbers, not a second accumulation. */
typedef struct {
    uint32_t cur_us;
    uint32_t min_us;
    uint32_t max_us;
    uint32_t avg_us;
} sp_metric_us;

typedef struct {
    /* False until the audio stream exists and has been sampled. A
       zeroed struct with valid == false means "no data", which is a
       different statement from "a perfect call". */
    bool     valid;
    uint64_t sampled_us;    /* when this snapshot was taken */
    /* True when the snapshot was taken as the stream was torn down,
       which makes it the last word on the call. */
    bool     final;

    /* rx_* is what this phone received and measured. tx_* is what the
       peer reported back to us in its RTCP receiver reports, so it
       stays zero until the peer has sent one. */
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint32_t rx_lost;
    uint32_t tx_lost;
    uint32_t rx_discarded;
    uint32_t rx_reordered;
    uint32_t rx_duplicated;

    double   rx_loss_pct;
    double   tx_loss_pct;

    sp_metric_us rx_jitter;
    sp_metric_us tx_jitter;
    sp_metric_us rtt;        /* zero until the peer reports */

    /* Jitter buffer, in the units pjmedia reports: frames for size and
       prefetch, milliseconds for the delays. */
    uint32_t jb_size_frames;
    uint32_t jb_prefetch_frames;
    uint32_t jb_avg_delay_ms;
    uint32_t jb_min_delay_ms;
    uint32_t jb_max_delay_ms;
    uint32_t jb_lost;
    uint32_t jb_discarded;
    uint32_t jb_empty;

    /* An estimate, not a measurement. The rating-to-MOS conversion is
       ITU-T G.107's; the terms that produce the rating from delay and
       loss are the widely used simplification, not G.107's impairment
       factors, and they are generous about heavy loss. Good for
       comparing calls on this phone; not a certified score. Zero when
       valid is false. */
    double   mos;         /* 1.0 .. 4.5 */
    int      r_factor;    /* 0 .. 93 */
} sp_call_quality;

typedef struct {
    const char   *id;             /* stable name, used in rule matching */
    const char   *user;
    const char   *domain;
    const char   *auth_user;      /* NULL => same as user */
    const char   *password;
    const char   *registrar;      /* NULL => derive from domain */
    const char   *outbound_proxy; /* optional */
    sp_transport  transport;
    int           reg_expires_s;
    bool          do_register;
    /* SRTP policy. MANDATORY without transport SP_TP_TLS still works,
       but the SDES keys then travel in cleartext SDP; the core logs a
       warning rather than refusing, since many PBXes are configured
       exactly that way. */
    sp_srtp_mode  srtp;
    const char   *stun_server;    /* NULL => no STUN */
} sp_account_cfg;

/* Where a call's transcript stands. Stored with the call, so it reads
   the same after a restart. */
typedef enum {
    SP_TRANSCRIPT_NONE,
    SP_TRANSCRIPT_PENDING,
    SP_TRANSCRIPT_READY,
    SP_TRANSCRIPT_FAILED
} sp_transcript_state;

/* One turn of speech. Times are from the start of the call's audio. */
typedef struct {
    uint32_t    start_ms;
    uint32_t    end_ms;
    bool        local;      /* true: this phone's microphone */
    const char *speaker;
    const char *text;
} sp_transcript_line;

typedef enum {
    SP_EV_ACCOUNT_STATE,
    SP_EV_CALL_STATE,
    SP_EV_CALL_MEDIA_STATE,
    SP_EV_INCOMING_CALL,
    SP_EV_DTMF_RECEIVED,
    SP_EV_XFER_STATUS,
    SP_EV_TRANSPORT_ERROR,
    SP_EV_SIP_MESSAGE,
    SP_EV_LOG,
    /* One audio-quality sample. Appended, so the numeric value of
       every earlier event is unchanged. */
    SP_EV_CALL_QUALITY,
    /* A call's transcript changed state. Appended, on the same rule. */
    SP_EV_TRANSCRIPT
} sp_event_type;

typedef struct {
    sp_event_type type;
    union {
        struct {
            sp_account_id acc;
            int           reg_code;
            bool          registered;
            const char   *detail;
        } account;
        struct {
            sp_call_id    call;
            sp_call_state state;
            int           last_code;
        } call_state;
        struct {
            sp_call_id call;
            bool       active;
        } media;
        struct {
            sp_call_id    call;
            sp_account_id acc;
            const char   *from_uri;
        } incoming;
        struct {
            sp_call_id call;
            char       digit;
        } dtmf;
        struct {
            sp_call_id  call;
            int         code;
            const char *reason;
        } xfer;
        struct {
            sp_account_id acc;
            const char   *detail;
        } transport;
        /* Fixed buffers rather than pointers: this fires once per SIP
           message, and an allocation per message on the signalling
           path is not acceptable. The full text is in the SIP log, not
           in the event. */
        struct {
            bool outbound;        /* true = tx */
            char method[16];      /* empty for a response */
            int  status_code;     /* 0 for a request */
            char call_id[128];    /* truncated if longer */
        } sip;
        struct {
            int         level;
            const char *msg;
        } log;
        /* The headline figures only, by value: a UI can draw a bar
           without calling back into the core, and anything more
           detailed comes from sp_call_get_quality. */
        struct {
            sp_call_id call;
            double     mos;
            double     rx_loss_pct;
            uint32_t   rx_jitter_us;
            uint32_t   rtt_us;
            bool       final;
        } quality;
        struct {
            int64_t             history_id;
            sp_transcript_state state;
            const char         *detail;   /* why, for FAILED; else NULL */
        } transcript;
    } u;
} sp_event;

typedef void (*sp_event_cb)(const sp_event *ev, void *user);

/* --- how things are shown --------------------------------------------
 *
 * Rules about presentation that every frontend needs and none should
 * own. They lived in frontend/qt until a second frontend had to
 * reimplement them, which is the duplication this header warns about
 * at the top.
 */

/* Two characters and a terminator, and a character may be three bytes
   of UTF-8. */
#define SP_INITIALS_MAX 8

/* The initials for an avatar: the first letters of the first and last
   words, upper-cased. "Sylvie Tremblay" is ST. "Repartition" is R,
   not RE, because one letter of one word is a monogram and two is a
   guess at a surname that is not there. Empty for an empty or
   whitespace-only name, which is a frontend's cue to draw the
   anonymous figure instead.
 *
 * Upper-casing covers ASCII and the Latin-1 supplement, so "Étienne"
 * gives "É". A character outside that is passed through unchanged
 * rather than damaged. */
sp_status sp_contact_initials(const char *name, char *out, size_t outlen);

/* Enough for the line below with room to spare. */
#define SP_QUALITY_LINE_MAX 96

/* The one-line quality summary, in the words both frontends use:
   "MOS 4.41 · loss 0.0% · jit 0.4 · rtt 12 ms". Microseconds are
   shown as milliseconds because that is the unit a technician thinks
   in. Two frontends spelling the same four figures differently is
   two frontends describing one call differently. */
sp_status sp_call_quality_line(const sp_call_quality *q,
                               char *out, size_t outlen);

/* --- build identity --------------------------------------------------
 *
 * Available before a core exists, because --version must not have to
 * start a SIP stack to answer.
 */
typedef struct {
    const char *version;            /* 0.1.0 */
    /* The commit the build was configured from, with "+dirty" when the
       tree had uncommitted changes. Every package is the same version,
       so this is what actually identifies a build. */
    const char *git_commit;
    /* When the build was CONFIGURED, not when it was compiled. CMake
       records it at configure time, so a rebuild without reconfiguring
       keeps the old date. Named for what it is: calling it the build
       date was a small lie that would waste someone's afternoon. */
    const char *configured_date;    /* UTC, YYYY-MM-DD */
    const char *pjproject_version;
} sp_build_info;

/* Points to static storage; never NULL. */
const sp_build_info *sp_build(void);

/* --- lifecycle ------------------------------------------------------ */

/* Parses config_path. Does not touch pjsua, so config errors are cheap.
   Returns SP_ERR_CONFIG with a parse failure, SP_ERR_INVALID_ARG if
   out is NULL. */
/* Android only, and the first call the process must make -- before
   sp_core_create, because pjsua_create is not the thing that needs it
   but everything after is. pjlib's Android build reaches Java through
   JNI for two things it has no native path for: dialog tags come from
   java.util.UUID.randomUUID(), and the audio device is
   android.media.AudioRecord. Without the VM the first is an empty
   string, which trips an assertion inside pjsip the moment a dialog
   is created, and the second is a null JNIEnv dereferenced in
   pjmedia. Measured on an emulator, not assumed -- see
   docs/roadmap.md, M1a.

   vm is a JavaVM*, declared void* so this header stays free of both
   pjsip and jni.h. On any other platform this is a no-op, so a
   frontend may call it unconditionally. */
void      sp_core_set_jvm(void *vm);

sp_status sp_core_create(const char *config_path, sp_core **out);
sp_status sp_core_set_event_cb(sp_core *core, sp_event_cb cb, void *user);
sp_status sp_core_start(sp_core *core);
sp_status sp_core_stop(sp_core *core);
void      sp_core_destroy(sp_core *core);

/* --- theming ---------------------------------------------------------
 *
 * Themes are resolved in the core and applied by each frontend, so
 * Debian and Android cannot drift. A frontend applies these values and
 * decides nothing: any decision made in a frontend is a decision the
 * other platform has to duplicate.
 */
/* Appended to, never reordered: the numbers reach the Android build
   through the same ABI, and a config names a preset by string, so a
   renumbering would silently repaint someone's phone. */
typedef enum {
    SP_THEME_DARK,
    SP_THEME_GOLD,
    SP_THEME_LIGHT,
    SP_THEME_HIGH_CONTRAST,
    SP_THEME_AMBER_CRT,
    SP_THEME_GREEN_PHOSPHOR,
    SP_THEME_OSCILLOSCOPE,
    SP_THEME_COPPER_PAIR,
    SP_THEME_PUNCHDOWN,
    SP_THEME_NEBULA,
    SP_THEME_EVENT_HORIZON,
    SP_THEME_AURORA,
    SP_THEME_REDSHIFT,
    SP_THEME_KYBER,
    SP_THEME_PAPER_TAPE,
    SP_THEME_WHITE_DWARF,
    SP_THEME_REPUBLIC,
    SP_THEME_EMPIRE,
    SP_THEME_JEDI_ORDER,
    SP_THEME_SITH_ORDER
} sp_theme_preset;

typedef enum {
    SP_TEXT_SMALL, SP_TEXT_MEDIUM, SP_TEXT_LARGE, SP_TEXT_XLARGE
} sp_text_scale;

/* Every field is resolved: the preset's value unless overridden.
   Strings point to static storage and are valid for the process. */
typedef struct {
    sp_theme_preset preset;
    sp_text_scale   text_scale;
    float           text_multiplier;
    bool            dark;
    const char     *accent_hex;
    const char     *background_hex;
    const char     *surface_hex;
    const char     *foreground_hex;
    const char     *muted_hex;
    const char     *font_family;
    /* A mark the preset draws faintly behind the call, named rather
       than drawn here so each frontend paints it in its own toolkit;
       "" for none. Appended, like the preset list. */
    const char     *emblem;
} sp_theme;

sp_status sp_theme_get(sp_core *core, sp_theme *out);

/* --- colours the theme implies rather than states --------------------
 *
 * Two rules that every frontend needs and none should own. They lived
 * in frontend/qt/theme.cpp until the Android frontend had to
 * reimplement them by hand, which is the drift this header keeps
 * warning about. All three write "#RRGGBB" and need outlen >= 8.
 */

/* The level meter's two traces.
 *
 * Transmit is the theme's accent. Receive is that hue turned a
 * quarter of the way round the wheel -- far enough to be a different
 * colour at a glance, near enough to still belong to the theme.
 *
 * A rotation was tried once and thrown away for two reasons that both
 * still hold, and are both handled here rather than avoided: it can
 * land on red, which in this application ends a call and marks a
 * recording, so the result is pushed clear of red; and it can come
 * out too dark or too pale against the surface, so the lightness is
 * walked until it clears 3:1. An accent with no hue at all -- high
 * contrast is white -- has nothing to rotate, and falls back to being
 * blended towards the surface until it is plainly the quieter trace.
 *
 * Direction still says which is which. Colour is what makes the two
 * read as two signals rather than as one shape. */
sp_status sp_theme_channel(const sp_theme *t, bool receive,
                           char *out, size_t outlen);

/* Colours that carry a meaning rather than a mood.
 *
 * Go and Stop are the two an operator must not have to read a label
 * to tell apart, and they are the same two the world over, so the hue
 * is fixed and only the lightness moves -- walked away from the
 * surface until it clears 3:1, which is what makes one pair of hues
 * work on Paper Tape and on high contrast alike.
 *
 * Meant to be used sparingly: in the call panel three buttons of five
 * stay in the theme's own ink, because if everything is coloured then
 * nothing is. */
typedef enum { SP_SEMANTIC_GO, SP_SEMANTIC_STOP } sp_semantic;

sp_status sp_theme_semantic(const sp_theme *t, sp_semantic which,
                            char *out, size_t outlen);

/* How many presets exist, and how each one is spelled. A frontend
   builds its picker from these rather than from a list of its own:
   a second list is a list that drifts, and the Android build would
   have to keep the same one. The name is the config spelling
   (lowercase, underscores); the label is for showing a person. */
int sp_theme_preset_count(void);
const char *sp_theme_preset_label(sp_theme_preset p);

/* Resolves a candidate theme without storing anything, so a preview
   shows exactly what saving would produce. Needs no core: resolution
   is pure. Returns SP_ERR_CONFIG for an accent that is not #RRGGBB. */
sp_status sp_theme_preview(sp_theme_preset preset, const char *accent_hex,
                           const char *font_family, sp_text_scale scale,
                           sp_theme *out);

/* accent_hex and font_family may be NULL, meaning "the preset's".
   Persists to the configuration file. */
sp_status sp_theme_set(sp_core *core, sp_theme_preset preset,
                       const char *accent_hex, const char *font_family,
                       sp_text_scale scale);

/* Settings a frontend needs to reflect in its UI. Read-only: changing
   them means editing the config, which the core owns. */
typedef struct {
    bool sip_log;
    bool sip_log_redact;
    int  sip_log_max;
    int  log_level;
    int  sip_port;
} sp_settings;

sp_status sp_core_get_settings(sp_core *core, sp_settings *out);

/* Rebuild transports and re-register after a network change. */
sp_status sp_core_network_changed(sp_core *core);

/* --- accounts ------------------------------------------------------- */

sp_status sp_account_add(sp_core *core, const sp_account_cfg *cfg,
                         sp_account_id *out);
/* As sp_account_add, but the account is never written to the
   configuration file: it exists for this run only, and the caller is
   responsible for putting it back on the next one.

   It exists because sp_account_add persists everything it is given,
   including the password, in cleartext -- which is the right default
   for a desktop config the operator edits by hand, and the wrong one
   for a frontend that keeps the credential somewhere better. The
   Android app holds its password under an Android Keystore key and
   adds its account this way, so the secret never reaches a file at
   all. Anything else about the account that should survive a restart
   is then the frontend's to store.

   Appended to the ABI, never inserted. */
sp_status sp_account_add_transient(sp_core *core, const sp_account_cfg *cfg,
                                   sp_account_id *out);

sp_status sp_account_remove(sp_core *core, sp_account_id acc);

/* Checks a configuration the way a settings form should, and writes a
   sentence saying what is wrong into detail.

   The rules are the core's because both frontends have to refuse the
   same account for the same reason, and because the wording is what
   the operator reads. detail is always terminated; it is set empty on
   SP_OK. Passing a NULL detail is allowed when only the verdict is
   wanted. */
sp_status sp_account_validate(const sp_account_cfg *cfg, char *detail,
                              size_t detail_len);

/* Replaces an account's settings in place, keeping exactly one account
   where there was one.

   The core has no in-place pjsua update, so this is a remove followed
   by an add under the OLD name, and then a rename if the name changed
   -- in that order, so a failure at any point leaves the account and
   any header rule scoped to it agreeing with each other. If the add
   fails, the previous settings are put back and the add's status is
   returned; editing an account can therefore not destroy it.

   out receives the new handle: the old one is retired, because
   handles are never reused. rules_updated reports how many header
   rules followed a rename, and may be NULL.

   Persistent accounts only. A transient account is not in the
   configuration, so there is nothing to read back and this returns
   SP_ERR_NOT_FOUND. */
sp_status sp_account_update(sp_core *core, sp_account_id acc,
                            const sp_account_cfg *cfg, sp_account_id *out,
                            size_t *rules_updated);

/* Fills out with the account's stored settings, for pre-filling an
   edit form. Strings point to core-owned storage, valid until the
   account is removed or the core is destroyed. */
sp_status sp_account_get_cfg(sp_core *core, sp_account_id acc,
                             sp_account_cfg *out);
/* Gives a live account its password, without writing it anywhere.
 *
 * sp_account_add persists everything it is given, password included.
 * A frontend that keeps the credential somewhere better -- a keyring,
 * an Android Keystore -- adds the account without one and supplies it
 * here, so the configuration file holds an account that can be
 * registered and no secret that can be read.
 *
 * Applies at once; a registration already in progress is not
 * restarted, so a caller that wants the account registered with the
 * new credential calls sp_account_set_registration afterwards. NULL
 * or "" removes the credential. */
/* The username the account authenticates with, as the media stack
   actually holds it -- the account's auth user, or its user when
   there is none. Exposed so a test can assert what goes on the wire
   rather than only that a call returned SP_OK; a credential filed
   under the wrong username is answered 401 for ever and looks like a
   working phone from every other angle. */
sp_status sp_account_auth_user(sp_core *core, sp_account_id acc,
                               char *out, size_t outlen);

sp_status sp_account_set_password(sp_core *core, sp_account_id acc,
                                  const char *password);

sp_status sp_account_set_registration(sp_core *core, sp_account_id acc,
                                      bool on);
sp_status sp_account_get_state(sp_core *core, sp_account_id acc,
                               sp_account_state *out);

/* Live account handles, oldest first. Writes at most cap entries and
   reports the true count in n_out, so a caller can detect truncation. */
sp_status sp_account_list(sp_core *core, sp_account_id *ids, size_t cap,
                          size_t *n_out);

/* Renames an account, carrying anything that referenced the old name.
 *
 * Header rules can be scoped to an account by name, so a rename that
 * only changed the account would leave those rules matching a name
 * that no longer exists -- they would stop firing, silently, which is
 * the failure this project refuses. Any rule scoped to the old name is
 * moved to the new one and the count is reported through
 * rules_updated, which may be NULL.
 *
 * Call history and SIP log rows are deliberately NOT rewritten: they
 * record what the account was called when the call happened, and
 * editing them would falsify a record.
 *
 * Returns SP_ERR_INVALID_ARG for an empty name or one already in use.
 */
sp_status sp_account_rename(sp_core *core, sp_account_id acc,
                            const char *new_id, size_t *rules_updated);

/* The configured id string for a handle, or NULL if unknown. */
const char *sp_account_id_of(sp_core *core, sp_account_id acc);

/* The account's domain, so a dialer can complete a bare extension
   into a full URI. NULL if the handle is unknown. */
const char *sp_account_domain_of(sp_core *core, sp_account_id acc);

/* --- calls ---------------------------------------------------------- */

/* extra/n_extra are ad-hoc headers for this request only. They are
   applied after the configured rule set, so they always win. */
sp_status sp_call_make(sp_core *core, sp_account_id acc, const char *uri,
                       const sp_header *extra, size_t n_extra,
                       sp_call_id *out);
/* Completes what an operator typed into something sp_call_make will
   accept: a bare extension gains the account's domain, a user@host
   gains a scheme, and anything already sip: or sips: is left alone.
   Surrounding space is trimmed.

   The rule lives here because it is a product decision -- what three
   keystrokes mean -- and not a rendering one. SP_ERR_NOT_FOUND for an
   unknown account, SP_ERR_INVALID_ARG for an empty input or for an
   out buffer too small to hold the answer: a truncated URI dials
   somebody else, which is worse than a refusal. */
sp_status sp_call_uri_complete(sp_core *core, sp_account_id acc,
                               const char *input, char *out, size_t outlen);

sp_status sp_call_answer(sp_core *core, sp_call_id call, int code,
                         const sp_header *extra, size_t n_extra);
sp_status sp_call_hangup(sp_core *core, sp_call_id call, int code);
sp_status sp_call_set_hold(sp_core *core, sp_call_id call, bool hold);
/* uri is completed as sp_call_uri_complete does, against the call's
   own account, so a bare extension transfers where it would dial. */
sp_status sp_call_xfer_blind(sp_core *core, sp_call_id call,
                             const char *uri);
sp_status sp_call_xfer_attended(sp_core *core, sp_call_id a, sp_call_id b);
sp_status sp_call_send_dtmf(sp_core *core, sp_call_id call,
                            const char *digits);
/* --- recordings ------------------------------------------------------
 *
 * Where a recording lands and in what form belong to the core, not to
 * a frontend. A frontend that picks its own directory is a second
 * answer to "where are my recordings", and the Android build would be
 * a third.
 */
typedef enum {
    SP_REC_WAV,        /* 16-bit PCM in a WAV container. Plays anywhere.
                          Recorded at the conference bridge's rate
                          (16 kHz), not the call's own codec rate: this
                          is a capture of the mixed conversation, not a
                          bit-exact copy of what crossed the wire. */
    SP_REC_WAV_ULAW,   /* G.711 mu-law in a WAV container, at the same
                          16 kHz bridge rate as SP_REC_WAV. Half the
                          size of SP_REC_WAV for a modest loss of
                          quality; not the call's original RTP payload,
                          and not lossless for any call. */
    /* Appended, never inserted: these numbers cross the ABI to the
       Android build, on the same rule as sp_status above. Opus in an
       Ogg container, 24 kbps mono, at the same 16 kHz bridge rate as
       the other two -- about a fifth the size of SP_REC_WAV. Plays in
       current browsers and in VLC; Windows support depends on the
       version installed. WAV is the format nobody has to argue with,
       which is why this is a setting rather than a default chosen for
       everyone. */
    SP_REC_OPUS
} sp_recording_format;

/* The directory recordings are written to: absolute, never NULL,
   pointing into core-owned storage that stays valid until the next
   sp_recording_set_dir. */
sp_status sp_recording_get_dir(sp_core *core, const char **dir);

/* Sets, creates and persists. NULL or "" restores the default, which
   is "recordings" beside the configuration file. A relative path is
   SP_ERR_INVALID_ARG rather than resolved against the working
   directory, which is a directory nobody chose. A path that cannot be
   created is SP_ERR_IO now, rather than a recording that silently
   never starts during a call. */
sp_status sp_recording_set_dir(sp_core *core, const char *dir);

sp_status sp_recording_get_format(sp_core *core, sp_recording_format *out);

/* Record every call once it is answered and its audio is up, into the
   configured directory and format, as sp_call_record_start_auto does.
   Once per call: a recording the operator stops stays stopped. Needs
   the recording module; with it off, nothing is recorded. Persists. */
bool      sp_recording_get_auto(sp_core *core);
sp_status sp_recording_set_auto(sp_core *core, bool on);

/* Sets and persists. Applies to the next recording; one already
   running keeps the format it started with, since a WAV header is
   written before the first sample. */
sp_status sp_recording_set_format(sp_core *core, sp_recording_format fmt);

/* Where a SIP trace export is written. Same rules as the recording
   directory, defaulting to "traces" beside the configuration file. */
sp_status sp_trace_get_export_dir(sp_core *core, const char **dir);
sp_status sp_trace_set_export_dir(sp_core *core, const char *dir);

/* Writes one call's signalling to a file in the trace export
   directory and returns its path.
 *
 * SP_ERR_NOT_FOUND when no message carries that Call-ID: an empty file
 * is worse than a refusal, because the operator forwards it before
 * noticing it is empty.
 *
 * The file warns when it was captured with redaction off, which is
 * the only case worth flagging: a fully redacted trace carries no
 * warning, because a warning that always appears means nothing.
 * Redaction happens at capture, so an export can neither add nor
 * remove it -- what it can do is say so, where somebody about to
 * forward the file will see it.
 *
 * *path_out is core-owned, valid until the next sp_trace_export --
 * and, like sp_siplog_read and sp_actionlog_read, meant for one
 * caller at a time. */
sp_status sp_trace_export(sp_core *core, const char *call_id,
                          const char **path_out);

sp_status sp_call_record_start(sp_core *core, sp_call_id call,
                               const char *path);

/* Starts recording into a file the core names, in the configured
   directory and format. *path_out points into core-owned storage that
   stays valid until this call handle records again, or until the call
   ends, whichever comes first -- the record is freed on hangup. A
   frontend that wants to show the path after the call ends must copy
   it before then.

   This is what a frontend should call. sp_call_record_start stays for
   a caller that genuinely has a path of its own, which is the CLI
   taking one from the operator. */
sp_status sp_call_record_start_auto(sp_core *core, sp_call_id call,
                                    const char **path_out);
sp_status sp_call_record_stop(sp_core *core, sp_call_id call);
sp_status sp_call_get_info(sp_core *core, sp_call_id call,
                           sp_call_info *out);

/* The most recent audio-quality snapshot for this call. Returns SP_OK
   with out->valid == false when the call has no media yet, and
   SP_ERR_NOT_FOUND for an unknown or retired handle. Sampling is
   cheap: it reads counters the media stack already keeps. */
sp_status sp_call_get_quality(sp_core *core, sp_call_id call,
                              sp_call_quality *out);

sp_status sp_call_list(sp_core *core, sp_call_id *ids, size_t cap,
                       size_t *n_out);

/* --- playback --------------------------------------------------------
 *
 * Plays a recording back through pjmedia, on the same conference
 * bridge slot 0 that the ringtone and ringback already use: whatever
 * output device and volume the operator chose in Settings applies to
 * a played-back recording exactly as it does to those.
 *
 * One recording at a time. A call and playback never coexist:
 * starting one while a call is up is refused rather than mixed in,
 * and a call arriving or being placed while a recording is already
 * playing stops it first, on the same rule. A recording out of the
 * speaker reaches the far end through the microphone, and either
 * accident would play a previous caller's voice down a live line.
 */
typedef enum {
    SP_PLAY_STOPPED,
    SP_PLAY_PLAYING,
    SP_PLAY_PAUSED
} sp_play_state;

typedef struct {
    sp_play_state state;
    uint32_t      position_ms;
    uint32_t      duration_ms;
    const char   *path;   /* what is loaded; NULL when stopped */
    /* Appended, never inserted: this number crosses the ABI to the
       Android build, on the same rule as SP_REC_OPUS above. False for
       a loaded OggOpus recording, since sp_playback_seek refuses it;
       true for WAV. A frontend ANDs this into whatever else decides
       whether to enable a seek control, rather than deciding by
       extension itself. */
    bool          seekable;
    /* Appended, never inserted: same ABI rule as seekable and
       SP_REC_OPUS above. 0.5, 1.0 or 2.0; 1.0 when nothing is loaded.
       position_ms and duration_ms are both in the recording's own
       timeline regardless of this value -- a 60-second recording
       reports duration_ms 60000 whether played at 1x or 2x, since
       both describe the same audio, just consumed at a different
       wall-clock rate. See sp_playback_set_speed. */
    float         speed;
} sp_playback;

/* SP_ERR_STATE when any call exists, with a detail saying why.
   SP_ERR_INVALID_ARG for a NULL or empty path. SP_ERR_NOT_FOUND when
   the file does not exist. SP_ERR_MEDIA when the file is neither WAV
   nor OggOpus, or when the matching reader refuses it -- which format
   applies is decided by the file's first bytes ("RIFF" or "OggS"),
   never by its extension. Starting while something is already
   playing stops that first. */
sp_status sp_playback_start(sp_core *core, const char *path);

/* SP_ERR_STATE when nothing is loaded. Pause holds the position. */
sp_status sp_playback_pause(sp_core *core);
sp_status sp_playback_resume(sp_core *core);

/* Stopping nothing is not an error: a frontend calls this on close
   and should not have to ask first. */
sp_status sp_playback_stop(sp_core *core);

/* Clamps to the duration rather than failing; a seek past the end is
   a seek to the end. SP_ERR_STATE when nothing is loaded.
   SP_ERR_MEDIA for a loaded OggOpus recording: seeking inside
   compressed Opus is not yet implemented, and play/pause/stop/position
   all still work without it. Also resets the speed wrapper's internal
   WSOLA state, so a seek at 0.5x or 2x does not play a stale or
   discontinuous fragment left over from the position seeked away
   from; the speed itself is unaffected. */
sp_status sp_playback_seek(sp_core *core, uint32_t position_ms);

/* Time-stretches playback without changing pitch (WSOLA under the
   hood, not a resampler -- a resampler would raise the pitch along
   with the tempo). SP_ERR_INVALID_ARG for anything but exactly 0.5,
   1.0 or 2.0. SP_ERR_STATE when nothing is loaded. Applies to
   whatever is currently loaded and takes effect immediately, without
   a click or a stall; switching again is always safe, including back
   to 1.0. A fresh sp_playback_start resets speed to 1.0. */
sp_status sp_playback_set_speed(sp_core *core, float speed);

/* Never fails on a live core; reports SP_PLAY_STOPPED with zeroes
   when nothing is loaded. out->path points into core-owned storage,
   valid until the next sp_playback_start. */
sp_status sp_playback_get(sp_core *core, sp_playback *out);

/* --- media ---------------------------------------------------------- */

/* names points to core-owned storage valid until sp_core_stop. */
sp_status sp_media_enum_codecs(sp_core *core, const char ***names,
                              size_t *n_out);
/* Runtime only: applies now and is forgotten at the next start. The
   CLI drives it for one-shot experiments. sp_media_set_codecs is the
   one that persists. */
sp_status sp_media_set_codec_priority(sp_core *core, const char *codec_id,
                                      int priority);

/* One codec's place in the offer and the settings that are its own.
 *
 * Every parameter is -1 for "the codec's own default", so a frontend
 * can tell "left alone" from "deliberately set to zero" -- writing a
 * default back as a number would turn it into a decision the operator
 * never made.
 *
 * The supports_ flags say what the codec will actually honour, so a
 * frontend greys out a control rather than offering one that does
 * nothing. They are read from the media stack, not guessed from the
 * codec's name.
 */
typedef struct {
    const char *id;                 /* core-owned */
    bool        enabled;
    int         priority;           /* 0..255, 0 == disabled */

    int         bitrate;            /* bps; opus only */
    int         complexity;         /* 0..10 */
    int         expected_loss_pct;  /* 0..100; >0 turns on opus inband FEC */
    int         ptime_ms;
    int         cbr;                /* -1 unset, 0 off, 1 on */

    bool        supports_bitrate;
    bool        supports_complexity;
    bool        supports_expected_loss;
    bool        supports_ptime;
    bool        supports_cbr;
    int         clock_rate;
    int         channel_cnt;
    int         frame_ptime_ms;     /* ptime_ms must be a multiple of it */
} sp_codec_params;

/* Every codec the stack has, in the order they are offered. Points to
   core-owned storage, valid until the next call on this core, the
   same rule as sp_media_enum_codecs. */
sp_status sp_media_get_codecs(sp_core *core, const sp_codec_params **out,
                              size_t *n_out);

/* Applies and persists, like sp_media_set_dsp. The whole list goes at
   once because the order is part of what is being set. Validated
   before anything is applied, so a rejected list leaves both the
   media stack and the config as they were. Takes effect on the next
   call: a codec's parameters are fixed once its stream exists. */
sp_status sp_media_set_codecs(sp_core *core, const sp_codec_params *in,
                              size_t n);

/* "none" selects a null device, which is what lets the phone run
   headless in tests and CI. NULL or "default" selects the system
   default. Sets both directions; sp_media_set_audio_devices is the way
   to set them apart. */
sp_status sp_media_set_audio_device(sp_core *core, const char *name);

/* --- audio devices ---------------------------------------------------
 *
 * Enumerated from the media stack, so a running core is required.
 *
 * Selection is by name rather than by index: pjmedia's indices are not
 * stable across a restart or across a headset being plugged in, and a
 * configuration that silently moved to a different microphone would be
 * worse than one that failed to find the old one.
 */
/* What kind of thing an enumerated entry actually is.
 *
 * ALSA's enumeration is not a list of devices. On an ordinary Debian
 * laptop it returns 64 entries for three sound cards: rate converters,
 * channel maps, software mixers, raw USB streams, HDMI and S/PDIF
 * outputs, and one entry per card per plugin wrapper. A frontend that
 * showed all of them would be showing plumbing, so the core says which
 * is which and which are worth offering. */
typedef enum {
    SP_AUDIO_DEFAULT,   /* the system default, or the default card */
    SP_AUDIO_SERVER,    /* pipewire, pulse, jack */
    SP_AUDIO_CARD,      /* a real card, with format conversion */
    SP_AUDIO_RAW,       /* a real card, no conversion: hw: */
    SP_AUDIO_PLUGIN     /* rate converters, surround maps, mixers */
} sp_audio_kind;

typedef struct {
    int         index;          /* pjmedia's id, valid until sp_core_stop */
    const char *name;           /* pass this back to select it */
    const char *label;          /* human-readable; never empty */
    const char *driver;
    unsigned    input_count;    /* 0 => playback only */
    unsigned    output_count;   /* 0 => capture only */
    unsigned    default_rate_hz;
    sp_audio_kind kind;
    /* True for the handful worth offering without the operator asking
       to see everything: the default, the sound servers, and one usable
       entry per card. A phone at 8 or 16 kHz wants a card entry that
       converts, not a raw one that refuses the rate. */
    bool        recommended;
} sp_audio_device;

/* Points into core-owned storage, valid until the next call to this
   function on this core, matching sp_media_enum_codecs. */
sp_status sp_media_enum_audio_devices(sp_core *core,
                                      const sp_audio_device **out,
                                      size_t *n_out);

/* The names currently in effect. Either may be "default" or "none";
   neither is ever NULL. Core-owned storage. */
sp_status sp_media_get_audio_devices(sp_core *core, const char **capture,
                                     const char **playback);

/* Selects by name and persists to the configuration, so the choice
   survives a restart on both platforms. NULL leaves that side
   unchanged; "default" is the system default and "none" the null
   device. Returns SP_ERR_MEDIA naming the device if no such device
   exists, rather than falling back to the default: a phone that
   quietly used the wrong microphone would be a worse outcome. */
sp_status sp_media_set_audio_devices(sp_core *core, const char *capture,
                                     const char *playback);

/* --- window size -----------------------------------------------------
 *
 * Remembered so the phone comes back the size the operator left it.
 * Kept here rather than in a Qt-specific settings file so there is one
 * configuration file; Android has no window and ignores it.
 */

/* Zero in both when nothing has been remembered yet, which means the
   frontend should use its own default. */
sp_status sp_ui_get_window_size(sp_core *core, int *w, int *h);

/* Persists the size. Values below 1 are rejected rather than stored:
   a zero would read back as "never set" and quietly lose the setting. */
sp_status sp_ui_save_window_size(sp_core *core, int w, int h);

/* Whether the desktop frontend shows its activity pane. Persisted
   alongside the window size, for the same reason: one configuration
   file rather than a Qt-specific second one. */
bool      sp_ui_get_show_activity_log(sp_core *core);
sp_status sp_ui_set_show_activity_log(sp_core *core, bool show);

/* Whether the call log panel is open, persisted for the same
   reason the window size is: opening the panel widens the
   window, so restoring one without the other gives a window
   sized for something that is not there. */
bool      sp_ui_get_show_call_log(sp_core *core);
sp_status sp_ui_set_show_call_log(sp_core *core, bool show);

/* --- levels ----------------------------------------------------------
 *
 * Two independent levels and two mutes: the microphone, which is what
 * the far end hears, and the speaker, which is what you hear.
 *
 * Levels are a multiplier: 1.0 is unity, 0.0 is silence, 2.0 is twice
 * as loud. They persist. Mute does not: a phone that starts muted
 * because it was left muted is a support call, not a feature.
 */
typedef struct {
    float tx_level;   /* microphone, 0.0 .. 2.0 */
    float rx_level;   /* speaker, 0.0 .. 2.0 */
    bool  tx_muted;
    bool  rx_muted;
} sp_audio_levels;

sp_status sp_media_get_levels(sp_core *core, sp_audio_levels *out);

/* Applies at once and persists the two levels. A level outside
   0.0..2.0 is SP_ERR_INVALID_ARG rather than clamped, so a frontend
   bug is visible instead of quietly halving the volume. Muting leaves
   the stored level alone, so unmuting returns to where it was. */
sp_status sp_media_set_levels(sp_core *core, const sp_audio_levels *in);

/* --- sounds ----------------------------------------------------------
 *
 * Local playback: a ringtone, a ringback and a one-shot tone. These go
 * to the speaker only, never into the call, so the far end never hears
 * them.
 *
 * WAV only. pjmedia's file player has no other decoder, and adding one
 * so a ringtone could be an mp3 would be a poor trade.
 */
typedef enum {
    SP_SOUND_RINGTONE,      /* an inbound call is ringing; loops */
    SP_SOUND_RINGBACK,      /* an outbound call is ringing; loops */
    SP_SOUND_NOTIFICATION   /* a call ended unanswered; plays once */
} sp_sound_slot;

typedef struct {
    const char *name;    /* the file name without .wav; what to show */
    const char *path;    /* absolute */
    bool        bundled; /* shipped with the package rather than imported */
} sp_sound_file;

/* What is available for a slot, the operator's imports first where a
   name appears in both. Points into core-owned storage, valid until
   the next call for the same slot. */
sp_status sp_sound_list(sp_core *core, sp_sound_slot slot,
                        const sp_sound_file **out, size_t *n_out);

/* The configured value for a slot: a name, an absolute path, or "none"
   for silence. Never NULL. */
sp_status sp_sound_get(sp_core *core, sp_sound_slot slot,
                       const char **value);

/* Sets and persists. Accepts a name from sp_sound_list, an absolute
   path, or "none". Returns SP_ERR_INVALID_ARG for a name or path that
   resolves to nothing rather than falling back to a default, so a typo
   is visible instead of silently playing the wrong thing. */
sp_status sp_sound_set(sp_core *core, sp_sound_slot slot,
                       const char *value);

/* Plays a slot's sound once, to check it. Stops any preview already
   playing. Silent slots and a missing file return SP_OK having done
   nothing, since neither is an error worth interrupting the operator
   for. */
sp_status sp_sound_preview(sp_core *core, sp_sound_slot slot);
sp_status sp_sound_preview_stop(sp_core *core);

/* Cuts the tone played when a call ends unanswered, and only that: a
   frontend calls it on anything the operator does next, and a ringing
   phone must not be silenced by the same keypress. SP_OK when nothing
   is playing. */
sp_status sp_sound_stop_notification(sp_core *core);

/* Copies a WAV into the operator's sound directory and returns the
   stored path, which is what to hand to sp_sound_set. Rejects
   anything that is not a WAV, and anything the media stack then
   refuses to open: a file that cannot be played is worse stored than
   rejected. */
sp_status sp_sound_import(sp_core *core, sp_sound_slot slot,
                          const char *src_path, const char **stored_out);

/* --- audio processing -------------------------------------------------
 *
 * The signal processing the media stack applies to a call.
 *
 * Defaults are chosen for a measuring instrument rather than for a
 * consumer phone: anything that changes what the operator hears is off
 * unless asked for. Echo cancellation is the exception, because a
 * speakerphone without it howls, and that is not a diagnosis.
 */
typedef struct {
    /* These three take effect immediately, including on a call that is
       already up: they live in the echo canceller, which pjsua can be
       told to rebuild at any time. */
    bool echo_cancel;
    int  echo_tail_ms;              /* 0 => the media stack's default */
    bool noise_suppression;         /* an echo-canceller option */
    bool auto_gain;                 /* likewise: automatic gain control */

    /* These three are codec settings, so they apply to the NEXT call.
       A codec's parameters are fixed once its stream exists. */
    bool silence_suppression;       /* VAD: stop sending during silence */
    bool comfort_noise;             /* CNG; only audible with VAD on */
    /* Packet loss concealment. Leaving it off is a diagnostic
       technique: you hear the loss instead of the stack hiding it. */
    bool packet_loss_concealment;
} sp_audio_dsp;

sp_status sp_media_get_dsp(sp_core *core, sp_audio_dsp *out);

/* Applies and persists. The echo-canceller fields take effect at once;
   the codec fields apply to the next call. Returns SP_ERR_INVALID_ARG
   for an echo_tail_ms outside 0..500. */
sp_status sp_media_set_dsp(sp_core *core, const sp_audio_dsp *in);

/* --- signal level ----------------------------------------------------
 *
 * What the media stack is actually carrying, per direction, right
 * now. Not the volume setting -- that is sp_audio_levels -- but the
 * signal passing through the conference bridge.
 *
 * This answers the question a technician actually asks when a call
 * sounds wrong, which is not "how loud is it" but "is anything
 * arriving at all". A receive level pinned at zero while the far end
 * insists they are talking says the audio never reached us, and no
 * amount of checking the headset will change that.
 *
 * 0..255, as pjmedia reports it. Cheap enough to poll for a meter.
 */
typedef struct {
    unsigned tx;      /* what we are sending  */
    unsigned rx;      /* what we are receiving */
    bool     valid;   /* false before media is up, or after it ends */
} sp_signal_level;

/* SP_ERR_NOT_FOUND for an unknown or retired call. valid comes back
   false, rather than an error, when the call exists but has no media
   yet: that is a normal moment in a call's life, not a fault. */
sp_status sp_call_get_signal(sp_core *core, sp_call_id call,
                             sp_signal_level *out);

/* --- modules ---------------------------------------------------------
 *
 * A module is a named capability that can be switched off. Switched
 * off means inert, not hidden: the work stops, and a call into that
 * module returns SP_ERR_DISABLED naming it rather than quietly doing
 * nothing.
 *
 * Nothing is ever deleted by switching a module off. Recordings made
 * before stay on disk, the call history keeps what it held, and the
 * quality figures already written stay readable. That is a rule of
 * the registry, not a property of the modules that exist today.
 *
 * These are runtime toggles: every module is in the binary either
 * way. What turning one off buys is a simpler phone, not a smaller
 * one.
 */
typedef struct {
    const char *id;         /* config spelling, e.g. "recording" */
    const char *label;      /* for a person, e.g. "Recording" */
    bool        enabled;
    bool        removable;  /* false for the base the rest sits on */
} sp_module;

/* Core-owned, valid until the next call on this core. */
sp_status sp_modules_get(sp_core *core, const sp_module **out, size_t *n_out);

/* Applies and persists. SP_ERR_NOT_FOUND for an unknown id, and
   SP_ERR_INVALID_ARG for switching off something not removable. */
sp_status sp_module_set(sp_core *core, const char *id, bool enabled);

/* For a frontend deciding whether to offer something at all. */
bool sp_module_is_enabled(sp_core *core, const char *id);

/* --- header rules --------------------------------------------------- */

typedef struct {
    const char *name;   /* rule name from config */
    uint64_t    hits;   /* messages this rule has acted on */
} sp_rule_stat;

sp_status sp_rules_stats(sp_core *core, const sp_rule_stat **out,
                         size_t *n_out);

/* --- header presets --------------------------------------------------
 *
 * Named sets of ad-hoc headers, stored in the shared config so both
 * frontends offer the same ones. A preset is applied by handing its
 * headers to sp_call_make as the extra array.
 */
typedef struct {
    const char      *name;
    const sp_header *headers;
    size_t           n_headers;
} sp_header_preset;

/* Points into core-owned storage, valid until the next save or delete
   on this core. */
sp_status sp_presets_list(sp_core *core, const sp_header_preset **out,
                          size_t *n_out);

/* Creates or replaces the preset called name, then persists. Saving
   also remembers the header names it contains, so an editor can offer
   them later without the operator doing anything extra. */
sp_status sp_presets_save(sp_core *core, const char *name,
                          const sp_header *headers, size_t n_headers);

sp_status sp_presets_delete(sp_core *core, const char *name);

/* Header names seen before, for completion in an editor. Points into
   core-owned storage, valid until the next remember. */
sp_status sp_header_names_list(sp_core *core, const char ***out,
                               size_t *n_out);
sp_status sp_header_names_remember(sp_core *core, const char *name);

/* --- log ordering -----------------------------------------------------
 *
 * Applies to the SIP log, the action log and the call history.
 *
 * Done here rather than by reversing the list in a frontend, because
 * the two are not the same thing: every read is capped, so reversing
 * gives the newest N shown backwards, while this gives the oldest N.
 * When you are looking for the first sign of a fault, that is the
 * whole question.
 */
typedef enum {
    SP_ORDER_NEWEST_FIRST,   /* the default */
    SP_ORDER_OLDEST_FIRST
} sp_order;

sp_status sp_log_set_order(sp_core *core, sp_order order);
sp_order  sp_log_get_order(sp_core *core);

/* --- SIP log --------------------------------------------------------- */

typedef struct {
    uint64_t    ts_us;
    const char *direction;    /* "tx" or "rx" */
    const char *method;       /* NULL for a response */
    int         status_code;  /* 0 for a request */
    const char *call_id;
    const char *account;      /* NULL when not account-scoped */
    bool        redacted;
    const char *raw;
} sp_sip_message;

/* Newest first. call_id NULL means every message. Results point to
   core-owned storage, valid until the next sp_siplog_read on this
   core, matching sp_media_enum_codecs. */
sp_status sp_siplog_read(sp_core *core, const char *call_id, size_t limit,
                         const sp_sip_message **out, size_t *n_out);
sp_status sp_siplog_clear(sp_core *core);

/* --- action log ------------------------------------------------------ */

typedef struct {
    uint64_t    ts_us;
    int         level;
    const char *text;
} sp_action_entry;

/* Newest first. Core-owned storage, valid until the next
   sp_actionlog_read on this core. */
sp_status sp_actionlog_read(sp_core *core, size_t limit,
                            const sp_action_entry **out, size_t *n_out);
sp_status sp_actionlog_add(sp_core *core, int level, const char *text);
sp_status sp_actionlog_clear(sp_core *core);

/* --- call history ---------------------------------------------------- */

/* sip_call_id is the SIP Call-ID, which joins this record to the
   messages sp_siplog_read returns. It is not sp_call_id, the core's
   opaque handle: two different things one letter apart. */
typedef struct {
    int64_t     id;
    const char *sip_call_id;
    const char *account;
    bool        inbound;
    const char *remote_uri;
    const char *local_uri;
    uint64_t    start_us;
    uint64_t    answer_us;       /* 0 if never answered */
    uint64_t    end_us;          /* 0 if still up */
    uint32_t    duration_s;
    int         disconnect_code;
    const char *codec;           /* NULL if media never negotiated */
    const char *recording_path;  /* NULL if not recorded */
    /* The final audio-quality figures, all zero when the call never
       carried media. q_rx_packets is what distinguishes "measured
       nothing" from "measured a flawless call": a row with zero
       packets has no measurement, whatever q_mos says. The estimate's
       caveats are the ones on sp_call_quality. */
    double      q_mos;
    int         q_r_factor;
    double      q_rx_loss_pct;
    double      q_tx_loss_pct;
    uint32_t    q_rx_jitter_avg_us;
    uint32_t    q_rx_jitter_max_us;
    uint32_t    q_rtt_avg_us;
    uint32_t    q_rtt_max_us;
    uint32_t    q_rx_packets;
    uint32_t    q_tx_packets;
    sp_transcript_state transcript;
    /* Why, when transcript is FAILED; NULL otherwise. */
    const char *transcript_detail;
} sp_call_record;

/* Newest first. Core-owned storage, valid until the next
   sp_history_read on this core. */
sp_status sp_history_read(sp_core *core, size_t limit,
                          const sp_call_record **out, size_t *n_out);
sp_status sp_history_clear(sp_core *core);

/* --- call transcripts ------------------------------------------------
 *
 * Needs the transcript module and a speech model: a whisper.cpp ggml
 * file that a frontend downloads and hands over. Audio is transcribed
 * on this device and nowhere else, after the call has ended.
 */

/* Whether this build can transcribe at all. A build without a speech
   engine captures nothing, whatever the module says. */
bool sp_transcript_available(sp_core *core);
/* Copied into out, "" when none is set. A copy rather than a pointer:
   a call starting on the SIP stack's thread reads this while the
   operator may be replacing it from another. SP_ERR_INVALID_ARG when
   out cannot hold the whole path -- a truncated path names some other
   file. */
sp_status sp_transcript_get_model(sp_core *core, char *out, size_t outlen);
/* Sets and persists. NULL or "" clears. A relative path is
   SP_ERR_INVALID_ARG; a file that cannot be read is SP_ERR_NOT_FOUND. */
sp_status sp_transcript_set_model(sp_core *core, const char *path);
/* Where a call's audio waits to be transcribed: beside the
   configuration, readable by this user only. At startup everything in
   it goes except the tracks of calls still waiting, which
   sp_core_start queues again. */
sp_status sp_transcript_get_scratch_dir(sp_core *core, const char **dir);

/* A call's transcript, in time order. Core-owned, valid until the next
   sp_transcript_read or sp_history_clear on this core. A call with none is SP_OK with
   *n_out == 0. */
sp_status sp_transcript_read(sp_core *core, int64_t history_id,
                             const sp_transcript_line **out, size_t *n_out);

/*
 * The contact book.
 *
 * Embedded rather than a module. Once a call panel can show a name, a
 * build where it sometimes does is a build whose panel cannot be
 * trusted, and that is not a thing worth making optional.
 */
typedef struct {
    int64_t     id;
    const char *label;      /* "work", "mobile"; NULL when unlabelled */
    const char *uri;        /* as entered, and what gets dialled */
} sp_contact_number;

typedef struct {
    int64_t                  id;       /* 0 for a contact not yet saved */
    const char              *name;
    const char              *company;  /* may be NULL */
    const char              *note;     /* may be NULL */
    bool                     favourite;
    const sp_contact_number *numbers;
    size_t                   n_numbers;

    /* A picture the operator chose, as stored: PNG, never NULL-
       terminated, never more than 512 KB. NULL with a zero length when
       there is none, which is a different answer from a zero-length
       picture and the widgets ask which. */
    const void              *picture;
    size_t                   picture_len;
    const char              *picture_type;   /* "png", or NULL */
} sp_contact;

/* Favourites first, then by name, case-insensitively. search matches
   the name, the company and the numbers; NULL or empty returns
   everything up to limit. Core-owned, valid until the next
   sp_contacts_read on this core. */
sp_status sp_contacts_read(sp_core *core, const char *search, size_t limit,
                           const sp_contact **out, size_t *n_out);

/* SP_ERR_NOT_FOUND for an unknown id. Core-owned, valid until the next
   sp_contact_read_one. */
sp_status sp_contact_read_one(sp_core *core, int64_t id,
                              const sp_contact **out);

/* id 0 inserts, non-zero updates. The number list is replaced whole.
   SP_ERR_INVALID_ARG for an empty name or for a contact with no
   dialable number -- an entry that cannot be dialled is a note, and a
   directory full of notes stops being trusted. */
sp_status sp_contact_save(sp_core *core, const sp_contact *in,
                          int64_t *id_out);

sp_status sp_contact_delete(sp_core *core, int64_t id);

/* Who is this? SP_ERR_NOT_FOUND when nobody.
 *
 * Matching is exact, on a normalised form: the user part of a URI,
 * digits only, with a NANP country code dropped, so +1-418-555-1234
 * and (418) 555-1234 are one number while extension 6046 matches only
 * 6046. Deliberately no suffix matching -- where 4185 is a desk and
 * 418-555-4185 is a citizen, a suffix rule puts the wrong name on an
 * emergency call, and a missing name is the smaller failure.
 *
 * Core-owned, valid until the next sp_contact_lookup, which the call
 * panel calls on every refresh. Copy anything you keep. */
sp_status sp_contact_lookup(sp_core *core, const char *uri,
                            const sp_contact **out);

/* One contact's picture, without the rest of the contact.
 *
 * sp_contacts_read leaves pictures out: it fills a scrolling view, and
 * a book of two hundred would otherwise put a hundred megabytes
 * through one call. A row delegate asks here instead, per row, and
 * caches what it gets.
 *
 * SP_OK with *data NULL and *len 0 when the contact has no picture;
 * SP_ERR_NOT_FOUND for an unknown id. Core-owned, valid until the next
 * sp_contact_picture on this core. */
sp_status sp_contact_picture(sp_core *core, int64_t id, const void **data,
                             size_t *len, const char **type);

#ifdef __cplusplus
}
#endif

#endif /* COMLINK_CORE_H */
