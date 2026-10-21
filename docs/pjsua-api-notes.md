# pjproject API notes — recorded from the pinned build

Everything here was read out of the built tree, not from memory. Later
work reads this file instead of guessing signatures.

**Pinned tag: 2.17** (highest stable 2.x at time of vendoring).
Both platforms must build from this exact tag.

## configure flags

Actual flags for this tag, read from `./configure --help`:

```
./configure --prefix=<prefix> --disable-video
```

SSL, Opus and libsrtp are **autodetect-and-on-by-default**; the flags
that exist are `--disable-ssl`, `--disable-opus`, `--disable-libsrtp`.
There is no `--enable-ssl` / `--with-opus` needed when the dev packages
are present. `--disable-video` is the only thing we assert.

### What configure actually decided

Verified in the generated headers, not in configure's stdout:

| Setting | Where | Value |
|---|---|---|
| `PJ_HAS_SSL_SOCK` | `pjlib/include/pj/compat/os_auto.h` | 1 |
| `PJMEDIA_HAS_OPUS_CODEC` | `pjmedia/include/pjmedia-codec/config_auto.h` | 1 |
| ALSA backend | `build.mak` link line | `-lasound` present |
| bundled libsrtp | `build.mak` | `-lsrtp` present |

## config_site.h

Two findings that changed what belongs in this file:

1. **`PJSUA_MAX_CALLS` defaults to 4** (`pjsua-lib/pjsua.h:5623`). Left
   alone, concurrent calls would have silently capped at four. Set to 16.
2. **SSL, Opus and ALSA are owned by configure**, not by
   `config_site.h`. `PJMEDIA_AUDIO_DEV_HAS_ALSA` defaults to 0 in
   `pjmedia-audiodev/config.h`, but configure's generated build config
   is what actually enables the backend. Setting it here would be a
   value that silently does nothing, so it is deliberately absent.

Every macro we do set is `#ifndef`-guarded upstream, so the overrides
take effect. Verified individually.

## pkg-config — spec item 13.2: VERIFIED

`libpjproject.pc` is installed and works:

```
$ pkg-config --modversion libpjproject
2.17
```

**`--static` is required.** pjproject installs static archives, so plain
`--libs` omits `Libs.private` and the link fails with
`undefined reference to fmod`. The working invocation is:

```
pkg-config --cflags libpjproject
pkg-config --static --libs libpjproject
```

`--static --libs` adds: `-lopus -lssl -lcrypto -luuid -lm -lrt
-lpthread -lasound`.

Link smoke test (`pjsua_create` / `pjsua_destroy`): **LINK+RUN OK**.

## Recorded signatures

```c
PJ_DECL(pj_status_t) pjsua_create(void);
PJ_DECL(pj_status_t) pjsua_init(const pjsua_config *ua_cfg,
                                const pjsua_logging_config *log_cfg,
                                const pjsua_media_config *media_cfg);
PJ_DECL(pj_status_t) pjsua_start(void);
PJ_DECL(pj_status_t) pjsua_destroy(void);

PJ_DECL(pj_status_t) pjsua_transport_create(pjsip_transport_type_e type,
                                            const pjsua_transport_config *cfg,
                                            pjsua_transport_id *p_id);

PJ_DECL(pj_status_t) pjsua_acc_add(const pjsua_acc_config *acc_cfg,
                                   pj_bool_t is_default,
                                   pjsua_acc_id *p_acc_id);
PJ_DECL(pj_status_t) pjsua_acc_set_registration(pjsua_acc_id acc_id,
                                                pj_bool_t renew);

PJ_DECL(pj_status_t) pjsua_call_make_call(pjsua_acc_id acc_id,
                                          const pj_str_t *dst_uri,
                                          const pjsua_call_setting *opt,
                                          void *user_data,
                                          const pjsua_msg_data *msg_data,
                                          pjsua_call_id *p_call_id);
PJ_DECL(pj_status_t) pjsua_call_answer2(pjsua_call_id call_id,
                                        const pjsua_call_setting *opt,
                                        unsigned code, const pj_str_t *reason,
                                        const pjsua_msg_data *msg_data);
PJ_DECL(pj_status_t) pjsua_call_hangup(pjsua_call_id call_id, unsigned code,
                                       const pj_str_t *reason,
                                       const pjsua_msg_data *msg_data);
PJ_DECL(pj_status_t) pjsua_call_set_hold(pjsua_call_id call_id,
                                         const pjsua_msg_data *msg_data);
PJ_DECL(pj_status_t) pjsua_call_xfer(pjsua_call_id call_id,
                                     const pj_str_t *dest,
                                     const pjsua_msg_data *msg_data);
PJ_DECL(pj_status_t) pjsua_call_xfer_replaces(pjsua_call_id call_id,
                                              pjsua_call_id dest_call_id,
                                              unsigned options,
                                              const pjsua_msg_data *msg_data);
PJ_DECL(pj_status_t) pjsua_call_dial_dtmf(pjsua_call_id call_id,
                                          const pj_str_t *digits);

PJ_DECL(pj_status_t) pjsua_recorder_create(const pj_str_t *filename,
                                           unsigned enc_type, void *enc_param,
                                           pj_ssize_t max_size,
                                           unsigned options,
                                           pjsua_recorder_id *p_id);
PJ_DECL(pj_status_t) pjsua_conf_connect(pjsua_conf_port_id source,
                                        pjsua_conf_port_id sink);

PJ_DECL(pj_status_t) pjsua_enum_codecs(pjsua_codec_info id[], unsigned *count);
PJ_DECL(pj_status_t) pjsua_codec_set_priority(const pj_str_t *codec_id,
                                              pj_uint8_t priority);

PJ_DECL(void) pjsua_msg_data_init(pjsua_msg_data *msg_data);

PJ_DECL(pj_status_t) pjsip_endpt_register_module(pjsip_endpoint *endpt,
                                                 pjsip_module *module);
PJ_DECL(void) pjsip_tx_data_invalidate_msg(pjsip_tx_data *tdata);

PJ_DECL(pj_status_t) pj_thread_register(const char *thread_name,
                                        pj_thread_desc desc,
                                        pj_thread_t **p_thread);
PJ_DECL(pj_bool_t) pj_thread_is_registered(void);
```

## pjsip_module callbacks

Exactly five, confirmed in `pjsip/sip_module.h`:

```
on_rx_request  on_rx_response  on_tx_request  on_tx_response  on_tsx_state
```

Available priorities, lowest number first (lower runs closer to the
wire on tx):

```
PJSIP_MOD_PRIORITY_TRANSPORT_LAYER
PJSIP_MOD_PRIORITY_TSX_LAYER
PJSIP_MOD_PRIORITY_UA_PROXY_LAYER
PJSIP_MOD_PRIORITY_DIALOG_USAGE
PJSIP_MOD_PRIORITY_APPLICATION
```

`TRANSPORT_LAYER` is the candidate for header rewriting, since it sees
messages last on the way out. To be confirmed by the Task 11 capture.

## pjsua_callback fields we rely on

```
on_call_state              -> SP_EV_CALL_STATE
on_incoming_call           -> SP_EV_INCOMING_CALL
on_call_media_state        -> SP_EV_CALL_MEDIA_STATE
on_reg_state2              -> SP_EV_ACCOUNT_STATE
on_dtmf_digit2             -> SP_EV_DTMF_RECEIVED
on_call_transfer_status    -> SP_EV_XFER_STATUS
on_ip_change_progress      -> relevant to sp_core_network_changed (Plan 3)
```

## Media statistics (Plan 7)

Read out of the pinned submodule. Verbatim.

`pjsip/include/pjsua-lib/pjsua.h`:

```c
typedef struct pjsua_stream_stat
{
    pjmedia_rtcp_stat   rtcp;   /** RTCP statistic. */
    pjmedia_jb_state    jbuf;   /** Jitter buffer statistic. */
} pjsua_stream_stat;

PJ_DECL(pj_status_t) pjsua_call_get_stream_stat(pjsua_call_id call_id,
                                                unsigned med_idx,
                                                pjsua_stream_stat *stat);

/* in pjsua_callback */
void (*on_stream_destroyed)(pjsua_call_id call_id,
                            pjmedia_stream *strm,
                            unsigned stream_idx);

PJ_DECL(pj_status_t) pjsua_schedule_timer(pj_timer_entry *entry,
                                          const pj_time_val *delay);
PJ_DECL(void) pjsua_cancel_timer(pj_timer_entry *entry);

typedef struct pjsua_call_media_info
{
    unsigned                index;   /** Media index in SDP. */
    pjmedia_type            type;
    pjmedia_dir             dir;
    pjsua_call_media_status status;
    union { struct { pjsua_conf_port_id conf_slot; } aud; /* ... */ } stream;
} pjsua_call_media_info;

/* in pjsua_call_info */
unsigned              media_cnt;
pjsua_call_media_info media[PJMEDIA_MAX_SDP_MEDIA];
```

`pjmedia/include/pjmedia/rtcp.h`:

```c
typedef struct pjmedia_rtcp_stream_stat
{
    pj_time_val     update;
    unsigned        update_cnt;
    pj_uint32_t     pkt;        /**< Total number of packets        */
    pj_uint32_t     bytes;      /**< Total number of payload/bytes  */
    unsigned        discard;
    unsigned        loss;
    unsigned        reorder;
    unsigned        dup;
    pj_math_stat    loss_period;/**< in usec */
    struct { unsigned burst:1; unsigned random:1; } loss_type;
    pj_math_stat    jitter;     /**< Jitter statistics (in usec)    */
} pjmedia_rtcp_stream_stat;

typedef struct pjmedia_rtcp_stat
{
    pj_time_val              start;
    pjmedia_rtcp_stream_stat tx;   /**< Encoder stream statistics.  */
    pjmedia_rtcp_stream_stat rx;   /**< Decoder stream statistics.  */
    pj_math_stat             rtt;  /**< Round trip delay (in usec)  */
    pj_uint32_t              rtp_tx_last_ts;
    pj_uint16_t              rtp_tx_last_seq;
    /* ... optional IPDV and raw-jitter members, then peer SDES ... */
} pjmedia_rtcp_stat;
```

`pjlib/include/pj/math.h`:

```c
typedef struct pj_math_stat
{
    int n; int max; int min; int last; int mean;
    /* private members follow */
} pj_math_stat;
```

`pjmedia/include/pjmedia/jbuf.h`:

```c
typedef struct pjmedia_jb_state
{
    unsigned frame_size, min_prefetch, max_prefetch, max_count,
             min_delay_set;                   /* setting   */
    unsigned burst, prefetch, size;           /* status    */
    unsigned avg_delay, min_delay, max_delay, dev_delay,  /* in ms */
             avg_burst, lost, discard, empty; /* statistic */
} pjmedia_jb_state;
```

`pjmedia/include/pjmedia/stream.h`:

```c
PJ_DECL(pj_status_t) pjmedia_stream_get_stat(const pjmedia_stream *stream,
                                             pjmedia_rtcp_stat *stat);
PJ_DECL(pj_status_t) pjmedia_stream_get_stat_jbuf(const pjmedia_stream *stream,
                                                  pjmedia_jb_state *state);
```

Two facts that shape the design:

1. `pj_math_stat` already carries `min`, `max`, `mean` and `last`, so
   the core does not accumulate ranges itself. It reads pjmedia's.
2. The stream is destroyed before `PJSIP_INV_STATE_DISCONNECTED`
   reaches the application, so a snapshot taken there is empty.
   `on_stream_destroyed` is the last moment the counters exist.
