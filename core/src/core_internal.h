#ifndef SP_CORE_INTERNAL_H
#define SP_CORE_INTERNAL_H

#include <pthread.h>

#include <pjsua-lib/pjsua.h>

#include "comlink/core.h"
#include "config.h"
#include "events.h"
#include "handles.h"
#include "sipuri.h"
#include "actionlog.h"
#include "contacts.h"
#include "history.h"
#include "playback.h"
#include "siplog.h"
#include "sound.h"
#include "store.h"
#include <stdatomic.h>

#include "thread.h"

typedef enum { SP_STATE_NEW, SP_STATE_RUNNING, SP_STATE_STOPPED } sp_state;

/* How long an unfinished IP change is believed before it is treated
   as over. Longer than a SIP transaction timeout, so a re-REGISTER
   that runs to its own end is not cut short. */
#define SP_IP_CHANGE_TIMEOUT_MS 60000

/* Per-account bookkeeping, held as handle-table data. */
typedef struct {
    char         *id;
    char         *aor;        /* user@domain, to match a message's From */
    char         *domain;     /* for completing a dialled extension */
    pjsua_acc_id  native;
    bool          registered;
    int           reg_code;
    int           reg_expires_s;
    char          detail[256];
} sp_account_rec;

/* Per-call bookkeeping, held as handle-table data. */
typedef struct {
    /* The handle this record belongs to. pjsua fires on_call_state
       during pjsua_call_make_call, before the native id is known, so
       callbacks resolve through pjsua's user_data rather than through
       the native id. Without this the first state event of every
       outgoing call was silently dropped. */
    uint64_t          handle;
    int64_t           history_id;
    pjsua_call_id     native;
    sp_account_id     acc;
    bool              on_hold;
    bool              inbound;
    pjsua_recorder_id recorder;   /* PJSUA_INVALID_ID when not recording
                                      via pjsua_recorder_create (wav,
                                      wav_ulaw) */
    /* SP_REC_OPUS recording, added to the conference bridge by hand
       rather than through pjsua_recorder_create (see call.c). NULL/
       PJSUA_INVALID_ID when not recording; rec_port is what tells the
       two recording paths apart, on the same rule pjsua's own player
       and recorder arrays use. */
    pjmedia_port      *rec_port;
    pj_pool_t         *rec_pool;
    pjsua_conf_port_id rec_slot;
    char              rec_path[512]; /* what record_start_auto chose, for
                                        the caller */
    /* Whether auto-record has had its one go at this call. Once, so a
       recording the operator stops stays stopped through a hold and a
       resume, which bring the media up again. */
    bool              auto_record_tried;
    /* The transcript's capture while the call is up, NULL otherwise;
       and whether it has had its one go, on the same rule. */
    struct sp_cap_ref *cap;   /* see call.c */
    bool              transcript_tried;
    int               last_code;
    /* Filled from pjsua's call info so sp_call_get_info can hand out
       stable pointers; a frontend must not have to parse a SIP URI. */
    char              remote_uri[256];
    char              local_uri[256];
    char              codec[64];
    sp_party          remote;
    /* The last quality sample taken, so a query never has to reach
       into a stream that may already be gone, and so the figures
       written to history survive the stream's destruction. Guarded by
       core->lock. */
    sp_call_quality   quality;
} sp_call_rec;

struct sp_core {
    sp_config       *cfg;
    char            *config_path;
    /* Resolved once: the configured value, or the default beside the
       configuration file. Both absolute. */
    char            *recording_dir;
    char            *trace_export_dir;
    /* What sp_trace_export last wrote, for the caller it just served.
       Fixed-size rather than allocated, on the same rule as
       sp_call_rec.rec_path: cheaper than tracking an owner, and the
       length is bounded by the same composition check that fills
       it. */
    char             trace_export_path[1024];
    sp_store        *store;
    sp_siplog       *siplog;
    sp_actionlog    *actionlog;
    sp_history      *history;
    /* Finished calls waiting to be transcribed, and where their
       audio waits. NULL transcriber: nothing is transcribed. */
    struct sp_transcriber *transcriber;
    char            *transcript_scratch;
    /* flock held on the scratch directory for this core's lifetime;
       -1 when none. Whoever holds it owns the queue in there. */
    int              scratch_lock;
    /* The previous run's pending transcripts were queued again. */
    bool             pending_resumed;
    sp_contacts     *contacts;
    sp_sound_lib    *sounds;
    /* One player per slot, PJSUA_INVALID_ID when not playing. The
       ringtone and ringback loop; the notification does not. */
    pjsua_player_id  players[3];
    /* Cuts the notification short: the bundled busy tone runs far
       longer than it takes to say the call failed, over whatever the
       operator does next. Cancelled by any stop of that slot. */
    pj_timer_entry   notification_timer;
    /* Recording playback: a single slot, since starting a second
       recording stops the first. See playback.h. */
    sp_playback_state playback;
    sp_handle_table *accounts;
    sp_handle_table *calls;
    sp_event_queue  *evq;
    sp_event_cb      cb;
    void            *cb_user;

    sp_rule_stat    *stats;
    size_t           n_stats;

    /* One transport per selected type, indexed by sp_transport, so an
       account uses the transport it asked for instead of letting pjsua
       resolve one from the account URI (which defaults to 5060/5061
       and connects somewhere nobody is listening). */
    pjsua_transport_id tp[3];
    /* When the IP change in flight started, or 0 for none. A second
       change started before the first finishes is refused: pjsua only
       logs "still in progress" and carries on, into a state where
       calls are refused with PJSIP_ETPNOTAVAIL.

       Atomic because it is cleared from a pjsip thread and claimed
       from the caller's. A timestamp rather than a flag because
       pjsua reports COMPLETED only when every account has finished,
       and an account added while a change is in flight is never
       counted -- so a flag could latch for the life of the process
       and the phone would stop handing over silently. */
    _Atomic int64_t    ip_change_started_ms;

    const char     **codec_names;
    size_t           n_codec_names;
    /* Backing for sp_media_get_codecs, on the same "valid until the
       next call" rule as codec_names. */
    sp_codec_params *codec_params;
    size_t           n_codec_params;
    sp_module       *module_view;

    /* Enumerated audio devices, and the names currently in effect.
       Names rather than indices, because an index means nothing after
       a restart. */
    sp_audio_device *audio_devices;
    size_t           n_audio_devices;
    char             audio_capture[128];
    char             audio_playback[128];

    /* One quality sampler for the whole core, walking the live calls
       on each fire and rescheduling itself. metrics_on is the flag the
       callback checks before rescheduling, so a stop is seen even if a
       fire is already in flight. Both under core->lock. */
    pj_timer_entry   metrics_timer;
    bool             metrics_on;

    char             err_detail[512];
    sp_order         log_order;
    sp_state         state;
    bool             pj_ready;
    pthread_mutex_t  lock;
};

/* Records the PJSIP message for a failed pj_status_t and maps it. */
sp_status sp_core_fail(sp_core *core, pj_status_t st, sp_status mapped,
                       const char *what);

void sp_core_post(sp_core *core, const sp_event *ev);

/* Records the last-error detail under core->lock. Every write to
   core->err_detail goes through this; writing it directly races
   with sp_last_error_detail and with pjsip worker threads. */
void sp_core_detail(sp_core *core, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* The single core the pjsua and pjsip_module callbacks belong to. */
sp_core *sp_core_active(void);

/* Implemented in account.c / call.c, wired from core.c. */
sp_status sp_account_add_locked(sp_core *core, const sp_account_cfg *cfg,
                                sp_account_id *out, bool persist);
sp_status sp_core_persist(sp_core *core);

/* The transport id for this type, creating it if absent. */
sp_status sp_core_ensure_transport(sp_core *core, sp_transport want,
                                   pjsua_transport_id *out);
/* Schedules the audio-quality sampler. A configured interval of 0
   disables it and returns SP_OK. */
sp_status sp_metrics_start(sp_core *core);

/* Cancels it. Idempotent, and safe when it never started. Must run
   before pjsua_destroy, or a queued fire lands in a freed core. */
void sp_metrics_stop(sp_core *core);

/* Refreshes rec->quality from pjmedia. Silent on failure: a call that
   has no media yet is not an error. Caller holds core->lock. */
void sp_call_sample_quality(sp_core *core, sp_call_rec *rec);

/* Applies the configured audio devices without writing the config
   back. Called once from sp_core_start. */
sp_status sp_media_apply_config(sp_core *core);
void      sp_media_free_devices(sp_core *core);

/* Starts or stops a slot's sound. Loops for the ringtone and the
   ringback, plays once for the notification. Silent when the slot is
   configured to "none" or the file cannot be opened: a missing sound
   must never stop a call from ringing. */
void sp_sound_start(sp_core *core, sp_sound_slot slot);
void sp_sound_stop(sp_core *core, sp_sound_slot slot);
void sp_sound_stop_all(sp_core *core);
void sp_sound_timer_init(sp_core *core);

/* Applies the configured echo canceller and codec settings without
   writing the config back. Called once from sp_core_start. */
sp_status sp_media_apply_dsp(sp_core *core);

/* Writes one codec's own parameters into the media stack. Applies to
   the next call: a codec's parameters are fixed once its stream
   exists. Unset fields (-1) are left as the codec had them. */
sp_status sp_media_apply_codec_params(sp_core *core, const sp_codec_cfg *c);

/* The echo-canceller option word for a configuration. Needed at
   pjsua_init as well as at runtime, so it is not static to media.c. */
unsigned sp_media_ec_options(const sp_config *cfg);

/* Applies the configured volumes without writing the config back. */
sp_status sp_media_apply_levels(sp_core *core);

void sp_account_install_callbacks(pjsua_callback *cb);
void sp_core_install_ip_change_callback(pjsua_callback *cb);
void sp_call_install_callbacks(pjsua_callback *cb);

/* Ends every call's transcript capture now, as switching the module
   off requires: the audio goes, the calls carry on. */
void sp_call_stop_captures(sp_core *core);
/* Tests only: the slot a call's "Me" side listens on, or invalid. */
pjsua_conf_port_id sp_call_capture_mic_slot(sp_core *core, sp_call_id call);

sp_status sp_hdr_module_register(sp_core *core);
void      sp_hdr_module_unregister(void);

#define SP_ENTRY(c) do {                        \
        if (!(c)) return SP_ERR_INVALID_ARG;    \
        if ((c)->pj_ready)                      \
            sp_thread_ensure_registered();      \
    } while (0)

#endif /* SP_CORE_INTERNAL_H */
