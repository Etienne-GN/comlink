/*
 * comlink-cli — a kept test instrument, not scaffolding.
 *
 * Its contract is the line protocol on stdin/stdout, which is what
 * makes signalling behaviour scriptable and reproducible without a
 * GUI. Every event prints one "EV " line; every command prints "OK "
 * or "ERR ".
 */
#include "comlink/core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOKENS 64
#define MAX_EXTRA  16

static sp_core *core;
static int auto_answer_code;

static const char *state_name(sp_call_state s) {
    switch (s) {
    case SP_CALL_NULL:         return "NULL";
    case SP_CALL_CALLING:      return "CALLING";
    case SP_CALL_INCOMING:     return "INCOMING";
    case SP_CALL_EARLY:        return "EARLY";
    case SP_CALL_CONNECTING:   return "CONNECTING";
    case SP_CALL_CONFIRMED:    return "CONFIRMED";
    case SP_CALL_DISCONNECTED: return "DISCONNECTED";
    }
    return "UNKNOWN";
}

static void on_event(const sp_event *ev, void *user) {
    (void)user;
    switch (ev->type) {
    case SP_EV_ACCOUNT_STATE:
        printf("EV ACCOUNT_STATE acc=%llu registered=%d code=%d detail=%s\n",
               (unsigned long long)ev->u.account.acc,
               ev->u.account.registered ? 1 : 0, ev->u.account.reg_code,
               ev->u.account.detail ? ev->u.account.detail : "-");
        break;
    case SP_EV_CALL_STATE:
        printf("EV CALL_STATE call=%llu state=%s code=%d\n",
               (unsigned long long)ev->u.call_state.call,
               state_name(ev->u.call_state.state), ev->u.call_state.last_code);
        break;
    case SP_EV_CALL_MEDIA_STATE:
        printf("EV CALL_MEDIA_STATE call=%llu active=%d\n",
               (unsigned long long)ev->u.media.call,
               ev->u.media.active ? 1 : 0);
        break;
    case SP_EV_INCOMING_CALL:
        printf("EV INCOMING_CALL call=%llu acc=%llu from=%s\n",
               (unsigned long long)ev->u.incoming.call,
               (unsigned long long)ev->u.incoming.acc,
               ev->u.incoming.from_uri ? ev->u.incoming.from_uri : "-");
        if (auto_answer_code) {
            sp_status a = sp_call_answer(core, ev->u.incoming.call,
                                         auto_answer_code, NULL, 0);
            printf("%s auto-answer %llu %d\n", a == SP_OK ? "OK" : "ERR",
                   (unsigned long long)ev->u.incoming.call, auto_answer_code);
        }
        break;
    case SP_EV_DTMF_RECEIVED:
        printf("EV DTMF_RECEIVED call=%llu digit=%c\n",
               (unsigned long long)ev->u.dtmf.call, ev->u.dtmf.digit);
        break;
    case SP_EV_XFER_STATUS:
        printf("EV XFER_STATUS call=%llu code=%d reason=%s\n",
               (unsigned long long)ev->u.xfer.call, ev->u.xfer.code,
               ev->u.xfer.reason ? ev->u.xfer.reason : "-");
        break;
    case SP_EV_TRANSPORT_ERROR:
        printf("EV TRANSPORT_ERROR acc=%llu detail=%s\n",
               (unsigned long long)ev->u.transport.acc,
               ev->u.transport.detail ? ev->u.transport.detail : "-");
        break;
    case SP_EV_SIP_MESSAGE:
        printf("EV SIP_MESSAGE %s %s%d call-id=%s\n",
               ev->u.sip.outbound ? "tx" : "rx",
               ev->u.sip.method[0] ? ev->u.sip.method : "",
               ev->u.sip.status_code,
               ev->u.sip.call_id[0] ? ev->u.sip.call_id : "-");
        break;
    case SP_EV_LOG:
        printf("EV LOG level=%d msg=%s\n", ev->u.log.level,
               ev->u.log.msg ? ev->u.log.msg : "-");
        break;
    case SP_EV_CALL_QUALITY:
        printf("EV CALL_QUALITY call=%llu mos=%.2f loss=%.2f jitter=%u "
               "rtt=%u final=%d\n",
               (unsigned long long)ev->u.quality.call, ev->u.quality.mos,
               ev->u.quality.rx_loss_pct, ev->u.quality.rx_jitter_us,
               ev->u.quality.rtt_us, ev->u.quality.final ? 1 : 0);
        break;
    case SP_EV_TRANSCRIPT:
        printf("EV TRANSCRIPT record=%lld state=%d detail=%s\n",
               (long long)ev->u.transcript.history_id,
               (int)ev->u.transcript.state,
               ev->u.transcript.detail ? ev->u.transcript.detail : "-");
        break;
    }
}

/* Three printf calls, one line. The event callback runs on the core's
   thread and prints from there, and stdio locks per call, not per
   line -- so an event lands between the "OK " and the rest of the
   response often enough that the loopback acceptance test fails on
   it. flockfile holds the stream across all three. */
static void report(sp_status st, const char *ok_fmt, ...) {
    if (st == SP_OK) {
        va_list ap;
        va_start(ap, ok_fmt);
        flockfile(stdout);
        printf("OK ");
        vprintf(ok_fmt, ap);
        printf("\n");
        funlockfile(stdout);
        va_end(ap);
    } else {
        printf("ERR %s: %s\n", sp_strerror(st), sp_last_error_detail(core));
    }
}

static size_t tokenise(char *line, char **tok, size_t max) {
    size_t n = 0;
    char *save = NULL;
    for (char *t = strtok_r(line, " \t\r\n", &save); t && n < max;
         t = strtok_r(NULL, " \t\r\n", &save))
        tok[n++] = t;
    return n;
}

/* Trailing ad-hoc headers, given as "Name: value" or "Name:value".
   Values may not contain spaces; this is a test instrument, not a
   shell. Returns -1 on a malformed header. */
static int parse_headers(char **tok, size_t n, size_t from,
                         sp_header *out, size_t *n_out) {
    size_t k = 0;
    for (size_t i = from; i < n; i++) {
        size_t len = strlen(tok[i]);
        if (len == 0)
            return -1;
        if (k >= MAX_EXTRA)
            return -1;
        if (tok[i][len - 1] == ':') {
            if (i + 1 >= n)
                return -1;
            tok[i][len - 1] = '\0';
            out[k].name = tok[i];
            out[k].value = tok[++i];
        } else {
            char *colon = strchr(tok[i], ':');
            if (!colon || colon == tok[i] || colon[1] == '\0')
                return -1;
            *colon = '\0';
            out[k].name = tok[i];
            out[k].value = colon + 1;
        }
        k++;
    }
    *n_out = k;
    return 0;
}

static void cmd_siplog(const char *call_id, size_t limit) {
    const sp_sip_message *m = NULL;
    size_t n = 0;
    sp_status st = sp_siplog_read(core, call_id, limit, &m, &n);
    if (st != SP_OK) {
        printf("ERR %s: %s\n", sp_strerror(st), sp_last_error_detail(core));
        return;
    }
    for (size_t i = 0; i < n; i++)
        printf("OK sip %zu %s %s%d call-id=%s redacted=%d bytes=%zu\n",
               i, m[i].direction,
               m[i].method ? m[i].method : "",
               m[i].status_code,
               m[i].call_id ? m[i].call_id : "-",
               m[i].redacted ? 1 : 0,
               m[i].raw ? strlen(m[i].raw) : (size_t)0);
    printf("OK siplog total=%zu\n", n);
}

/* A byte count is useless when the point is reading the message, so
   dump prints one verbatim. */
static void cmd_siplog_dump(size_t index) {
    const sp_sip_message *m = NULL;
    size_t n = 0;
    sp_status st = sp_siplog_read(core, NULL, index + 1, &m, &n);
    if (st != SP_OK) {
        printf("ERR %s: %s\n", sp_strerror(st), sp_last_error_detail(core));
        return;
    }
    if (index >= n) {
        printf("ERR no message at index %zu (have %zu)\n", index, n);
        return;
    }
    printf("OK sip-dump %zu %s %s%d redacted=%d\n", index, m[index].direction,
           m[index].method ? m[index].method : "",
           m[index].status_code, m[index].redacted ? 1 : 0);
    fputs(m[index].raw ? m[index].raw : "", stdout);
    printf("\nOK sip-dump end\n");
}

static void cmd_actions(size_t limit) {
    const sp_action_entry *e = NULL;
    size_t n = 0;
    sp_status st = sp_actionlog_read(core, limit, &e, &n);
    if (st != SP_OK) {
        printf("ERR %s: %s\n", sp_strerror(st), sp_last_error_detail(core));
        return;
    }
    for (size_t i = 0; i < n; i++)
        printf("OK action %llu %d %s\n", (unsigned long long)e[i].ts_us,
               e[i].level, e[i].text);
    printf("OK actions total=%zu\n", n);
}

static const char *audio_kind_name(sp_audio_kind k) {
    switch (k) {
    case SP_AUDIO_DEFAULT: return "default";
    case SP_AUDIO_SERVER:  return "server";
    case SP_AUDIO_CARD:    return "card";
    case SP_AUDIO_RAW:     return "raw";
    case SP_AUDIO_PLUGIN:  return "plugin";
    }
    return "?";
}

/* all == 0 lists only what the core recommends. ALSA reports 64
   entries for three sound cards on an ordinary laptop, most of them
   rate converters and channel maps, so the short list is the default
   and "audio all" is the escape hatch. */
static void cmd_audio(int all) {
    const char *cap = NULL, *play = NULL;
    sp_media_get_audio_devices(core, &cap, &play);
    printf("OK audio capture=%s playback=%s\n", cap ? cap : "?",
           play ? play : "?");

    const sp_audio_device *d = NULL;
    size_t n = 0;
    sp_status st = sp_media_enum_audio_devices(core, &d, &n);
    if (st != SP_OK) {
        printf("ERR %s: %s\n", sp_strerror(st), sp_last_error_detail(core));
        return;
    }
    size_t shown = 0, hidden = 0;
    for (size_t i = 0; i < n; i++) {
        if (!all && !d[i].recommended) {
            hidden++;
            continue;
        }
        printf("OK audio-device %d %s kind=%s label=\"%s\" driver=%s "
               "rate=%u in=%u out=%u\n",
               d[i].index, d[i].name, audio_kind_name(d[i].kind),
               d[i].label, d[i].driver, d[i].default_rate_hz,
               d[i].input_count, d[i].output_count);
        shown++;
    }
    printf("OK audio-devices shown=%zu hidden=%zu total=%zu\n",
           shown, hidden, n);
}

static const char *sound_slot_name(sp_sound_slot s) {
    switch (s) {
    case SP_SOUND_RINGTONE:     return "ringtone";
    case SP_SOUND_RINGBACK:     return "ringback";
    case SP_SOUND_NOTIFICATION: return "notification";
    }
    return "?";
}

static int sound_slot_of(const char *name, sp_sound_slot *out) {
    if (!name) return 0;
    if (!strcmp(name, "ringtone"))     { *out = SP_SOUND_RINGTONE; return 1; }
    if (!strcmp(name, "ringback"))     { *out = SP_SOUND_RINGBACK; return 1; }
    if (!strcmp(name, "notification")) { *out = SP_SOUND_NOTIFICATION; return 1; }
    return 0;
}

static void cmd_sounds(void) {
    for (int i = 0; i < 3; i++) {
        sp_sound_slot slot = (sp_sound_slot)i;
        const char *cur = NULL;
        sp_sound_get(core, slot, &cur);
        printf("OK sound %s = %s\n", sound_slot_name(slot),
               cur ? cur : "?");
        const sp_sound_file *f = NULL;
        size_t n = 0;
        if (sp_sound_list(core, slot, &f, &n) != SP_OK)
            continue;
        for (size_t j = 0; j < n; j++)
            printf("OK sound-file %s %s %s\n", sound_slot_name(slot),
                   f[j].bundled ? "bundled" : "imported", f[j].name);
        printf("OK sound-files %s total=%zu\n", sound_slot_name(slot), n);
    }
}

static int truthy(const char *v) {
    return v && (!strcmp(v, "on") || !strcmp(v, "true") ||
                 !strcmp(v, "1") || !strcmp(v, "yes"));
}

static void print_version(void) {
    const sp_build_info *b = sp_build();
    printf("comlink %s (%s, configured %s)\npjproject %s\n",
           b->version, b->git_commit, b->configured_date, b->pjproject_version);
}

static void cmd_levels(void) {
    sp_audio_levels l;
    if (sp_media_get_levels(core, &l) != SP_OK) {
        printf("ERR could not read the levels\n");
        return;
    }
    printf("OK levels tx=%d%% rx=%d%% tx_muted=%d rx_muted=%d\n",
           (int)(l.tx_level * 100.0f + 0.5f),
           (int)(l.rx_level * 100.0f + 0.5f),
           l.tx_muted ? 1 : 0, l.rx_muted ? 1 : 0);
}

/* volume <tx%> <rx%> */
static void cmd_volume(const char *txs, const char *rxs) {
    sp_audio_levels l;
    if (sp_media_get_levels(core, &l) != SP_OK) {
        printf("ERR could not read the levels\n");
        return;
    }
    l.tx_level = (float)strtol(txs, NULL, 10) / 100.0f;
    l.rx_level = (float)strtol(rxs, NULL, 10) / 100.0f;
    sp_status st = sp_media_set_levels(core, &l);
    report(st, "volume %s %s", txs, rxs);
}

/* mute <tx|rx|both> <on|off> */
static void cmd_mute(const char *which, const char *val) {
    sp_audio_levels l;
    if (sp_media_get_levels(core, &l) != SP_OK) {
        printf("ERR could not read the levels\n");
        return;
    }
    const int on = truthy(val);
    if (!strcmp(which, "tx"))        l.tx_muted = on;
    else if (!strcmp(which, "rx"))   l.rx_muted = on;
    else if (!strcmp(which, "both")) { l.tx_muted = on; l.rx_muted = on; }
    else {
        printf("ERR usage: mute <tx|rx|both> <on|off>\n");
        return;
    }
    sp_status st = sp_media_set_levels(core, &l);
    report(st, "mute %s %s", which, val ? val : "");
}

static void cmd_dsp(void) {
    sp_audio_dsp d;
    if (sp_media_get_dsp(core, &d) != SP_OK) {
        printf("ERR could not read the audio processing settings\n");
        return;
    }
    /* Grouped by when a change takes effect, because that is the
       thing an operator gets wrong. */
    printf("OK dsp echo_cancel=%d tail_ms=%d noise_suppression=%d "
           "auto_gain=%d  (immediate)\n",
           d.echo_cancel, d.echo_tail_ms, d.noise_suppression, d.auto_gain);
    printf("OK dsp silence_suppression=%d comfort_noise=%d "
           "packet_loss_concealment=%d  (next call)\n",
           d.silence_suppression, d.comfort_noise,
           d.packet_loss_concealment);
}

static void cmd_dsp_set(const char *key, const char *val) {
    sp_audio_dsp d;
    if (sp_media_get_dsp(core, &d) != SP_OK) {
        printf("ERR could not read the audio processing settings\n");
        return;
    }
    const int on = truthy(val);
    if (!strcmp(key, "echo_cancel"))                  d.echo_cancel = on;
    else if (!strcmp(key, "noise_suppression"))       d.noise_suppression = on;
    else if (!strcmp(key, "auto_gain"))               d.auto_gain = on;
    else if (!strcmp(key, "silence_suppression"))     d.silence_suppression = on;
    else if (!strcmp(key, "comfort_noise"))           d.comfort_noise = on;
    else if (!strcmp(key, "packet_loss_concealment"))
        d.packet_loss_concealment = on;
    else if (!strcmp(key, "echo_tail_ms"))
        d.echo_tail_ms = (int)strtol(val ? val : "", NULL, 10);
    else {
        printf("ERR unknown audio processing setting: %s\n", key);
        return;
    }
    sp_status st = sp_media_set_dsp(core, &d);
    report(st, "dsp-set %s %s", key, val ? val : "");
}

static void cmd_quality(const char *arg) {
    sp_call_id call = 0;
    if (arg && *arg) {
        call = (sp_call_id)strtoull(arg, NULL, 10);
    } else {
        /* No argument: the only call there is, which is what a tech
           watching a single call actually wants to type. */
        sp_call_id ids[32];
        size_t n = 0;
        if (sp_call_list(core, ids, 32, &n) != SP_OK || n == 0) {
            printf("ERR no calls\n");
            return;
        }
        call = ids[0];
    }

    sp_call_quality q;
    sp_status st = sp_call_get_quality(core, call, &q);
    if (st != SP_OK) {
        printf("ERR %s: %s\n", sp_strerror(st), sp_last_error_detail(core));
        return;
    }
    if (!q.valid) {
        printf("OK quality %llu no-media\n", (unsigned long long)call);
        return;
    }

    /* One field per line, greppable: the integration test asserts on
       counters, and a table would make that awkward. */
    unsigned long long c = (unsigned long long)call;
    printf("OK quality %llu final=%d\n", c, q.final ? 1 : 0);
    printf("OK mos %llu %.2f\n", c, q.mos);
    printf("OK r-factor %llu %d\n", c, q.r_factor);
    printf("OK rx-packets %llu %u\n", c, q.rx_packets);
    printf("OK tx-packets %llu %u\n", c, q.tx_packets);
    printf("OK rx-bytes %llu %llu\n", c, (unsigned long long)q.rx_bytes);
    printf("OK tx-bytes %llu %llu\n", c, (unsigned long long)q.tx_bytes);
    printf("OK rx-loss %llu %u %.2f\n", c, q.rx_lost, q.rx_loss_pct);
    printf("OK tx-loss %llu %u %.2f\n", c, q.tx_lost, q.tx_loss_pct);
    printf("OK rx-discard %llu %u\n", c, q.rx_discarded);
    printf("OK rx-reorder %llu %u\n", c, q.rx_reordered);
    printf("OK rx-dup %llu %u\n", c, q.rx_duplicated);
    printf("OK rx-jitter-us %llu cur=%u min=%u max=%u avg=%u\n", c,
           q.rx_jitter.cur_us, q.rx_jitter.min_us,
           q.rx_jitter.max_us, q.rx_jitter.avg_us);
    printf("OK tx-jitter-us %llu cur=%u min=%u max=%u avg=%u\n", c,
           q.tx_jitter.cur_us, q.tx_jitter.min_us,
           q.tx_jitter.max_us, q.tx_jitter.avg_us);
    printf("OK rtt-us %llu cur=%u min=%u max=%u avg=%u\n", c,
           q.rtt.cur_us, q.rtt.min_us, q.rtt.max_us, q.rtt.avg_us);
    printf("OK jb-frames %llu size=%u prefetch=%u\n", c,
           q.jb_size_frames, q.jb_prefetch_frames);
    printf("OK jb-delay-ms %llu avg=%u min=%u max=%u\n", c,
           q.jb_avg_delay_ms, q.jb_min_delay_ms, q.jb_max_delay_ms);
    printf("OK jb-events %llu lost=%u discard=%u empty=%u\n", c,
           q.jb_lost, q.jb_discarded, q.jb_empty);
}

static void cmd_history(size_t limit) {
    const sp_call_record *r = NULL;
    size_t n = 0;
    sp_status st = sp_history_read(core, limit, &r, &n);
    if (st != SP_OK) {
        printf("ERR %s: %s\n", sp_strerror(st), sp_last_error_detail(core));
        return;
    }
    for (size_t i = 0; i < n; i++)
        printf("OK call-record %lld %s %s call-id=%s start=%llu dur=%us "
               "code=%d codec=%s rec=%s\n",
               (long long)r[i].id,
               r[i].inbound ? "in" : "out",
               r[i].remote_uri ? r[i].remote_uri : "-",
               r[i].sip_call_id ? r[i].sip_call_id : "-",
               (unsigned long long)r[i].start_us,
               r[i].duration_s, r[i].disconnect_code,
               r[i].codec ? r[i].codec : "-",
               r[i].recording_path ? r[i].recording_path : "-");
    /* A separate line rather than more fields on the first: the
       quality figures are absent for most rows, and a row of dashes
       in the middle of a record is harder to read than a row that is
       simply not there. */
    for (size_t i = 0; i < n; i++) {
        if (r[i].q_rx_packets == 0)
            continue;
        printf("OK call-quality %lld mos=%.2f r=%d loss=%.2f/%.2f "
               "jitter=%u/%u rtt=%u/%u pkt=%u/%u\n",
               (long long)r[i].id, r[i].q_mos, r[i].q_r_factor,
               r[i].q_rx_loss_pct, r[i].q_tx_loss_pct,
               r[i].q_rx_jitter_avg_us, r[i].q_rx_jitter_max_us,
               r[i].q_rtt_avg_us, r[i].q_rtt_max_us,
               r[i].q_rx_packets, r[i].q_tx_packets);
    }
    printf("OK history total=%zu\n", n);
}

/* Prints the resolved values, which is how the Android port will be
   checked for drift against Debian. */
static void cmd_theme(void) {
    sp_theme t;
    sp_status st = sp_theme_get(core, &t);
    if (st != SP_OK) {
        printf("ERR %s: %s\n", sp_strerror(st), sp_last_error_detail(core));
        return;
    }
    printf("OK theme preset=%d scale=%d mult=%.2f dark=%d\n",
           (int)t.preset, (int)t.text_scale, (double)t.text_multiplier,
           t.dark ? 1 : 0);
    printf("OK theme accent=%s background=%s surface=%s\n",
           t.accent_hex, t.background_hex, t.surface_hex);
    printf("OK theme foreground=%s muted=%s font=%s\n",
           t.foreground_hex, t.muted_hex,
           t.font_family[0] ? t.font_family : "-");
}

static void cmd_accounts(void) {
    sp_account_id ids[32];
    size_t n = 0;
    if (sp_account_list(core, ids, 32, &n) != SP_OK) {
        printf("ERR could not list accounts\n");
        return;
    }
    for (size_t i = 0; i < n && i < 32; i++) {
        sp_account_state st = {0};
        sp_account_get_state(core, ids[i], &st);
        printf("OK account %llu id=%s registered=%d code=%d\n",
               (unsigned long long)ids[i],
               sp_account_id_of(core, ids[i]) ?: "-",
               st.registered ? 1 : 0, st.reg_code);
    }
    printf("OK accounts total=%zu\n", n);
}

static void cmd_calls(void) {
    sp_call_id ids[32];
    size_t n = 0;
    if (sp_call_list(core, ids, 32, &n) != SP_OK) {
        printf("ERR could not list calls\n");
        return;
    }
    for (size_t i = 0; i < n && i < 32; i++) {
        sp_call_info info = {0};
        if (sp_call_get_info(core, ids[i], &info) != SP_OK)
            continue;
        printf("OK call %llu %s state=%s who=%s hold=%d rec=%d code=%d "
               "dur=%u codec=%s\n",
               (unsigned long long)ids[i],
               info.inbound ? "in" : "out",
               state_name(info.state),
               info.remote_label ? info.remote_label : "-",
               info.on_hold ? 1 : 0, info.recording ? 1 : 0,
               info.last_code, info.duration_s,
               info.codec ? info.codec : "-");
    }
    printf("OK calls total=%zu\n", n);
}

/* -1 is "the codec's own default" throughout the ABI, and printing it
   as -1 invites a reader to think it is a value. */
static const char *codec_num(int v, char *buf, size_t cap) {
    if (v < 0)
        return "default";
    snprintf(buf, cap, "%d", v);
    return buf;
}

static void cmd_modules(void) {
    const sp_module *v = NULL;
    size_t n = 0;
    sp_status st = sp_modules_get(core, &v, &n);
    if (st != SP_OK) {
        printf("ERR %s: %s\n", sp_strerror(st), sp_last_error_detail(core));
        return;
    }
    for (size_t i = 0; i < n; i++)
        printf("OK module %s enabled=%s removable=%s label=%s\n",
               v[i].id, v[i].enabled ? "yes" : "no",
               v[i].removable ? "yes" : "no", v[i].label);
    printf("OK modules total=%zu\n", n);
}

static void cmd_module_set(const char *id, const char *val) {
    const bool on = !strcmp(val, "on") || !strcmp(val, "yes")
                    || !strcmp(val, "true") || !strcmp(val, "1");
    sp_status st = sp_module_set(core, id, on);
    if (st != SP_OK)
        printf("ERR %s: %s\n", sp_strerror(st), sp_last_error_detail(core));
    else
        printf("OK module-set %s %s\n", id, on ? "on" : "off");
}

static void cmd_codecs(void) {
    const sp_codec_params *v = NULL;
    size_t n = 0;
    sp_status st = sp_media_get_codecs(core, &v, &n);
    if (st != SP_OK) {
        printf("ERR %s: %s\n", sp_strerror(st), sp_last_error_detail(core));
        return;
    }
    for (size_t i = 0; i < n; i++) {
        char b1[16], b2[16], b3[16], b4[16];
        printf("OK codec %s prio=%d enabled=%s rate=%d ch=%d frame=%dms "
               "bitrate=%s complexity=%s loss=%s ptime=%s cbr=%s\n",
               v[i].id, v[i].priority, v[i].enabled ? "yes" : "no",
               v[i].clock_rate, v[i].channel_cnt, v[i].frame_ptime_ms,
               codec_num(v[i].bitrate, b1, sizeof b1),
               codec_num(v[i].complexity, b2, sizeof b2),
               codec_num(v[i].expected_loss_pct, b3, sizeof b3),
               codec_num(v[i].ptime_ms, b4, sizeof b4),
               v[i].cbr < 0 ? "default" : (v[i].cbr ? "on" : "off"));
    }
    printf("OK codecs total=%zu\n", n);
}

/* Sets one field on one codec and applies the whole list, because the
   ABI takes the list at once: order is part of what is being set, and
   a setter that sent one codec would silently reorder the rest. */
static void cmd_codec_set(const char *id, const char *key, const char *val) {
    const sp_codec_params *cur = NULL;
    size_t n = 0;
    sp_status st = sp_media_get_codecs(core, &cur, &n);
    if (st != SP_OK) {
        printf("ERR %s: %s\n", sp_strerror(st), sp_last_error_detail(core));
        return;
    }

    sp_codec_params *list = calloc(n ? n : 1, sizeof *list);
    char **ids = calloc(n ? n : 1, sizeof *ids);
    if (!list || !ids) {
        free(list); free(ids);
        printf("ERR out of memory\n");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        list[i] = cur[i];
        ids[i] = strdup(cur[i].id);
        list[i].id = ids[i];
    }

    int found = -1;
    for (size_t i = 0; i < n; i++)
        if (!strncasecmp(ids[i], id, strlen(id))) { found = (int)i; break; }
    if (found < 0) {
        printf("ERR no such codec: %s\n", id);
        goto done;
    }

    const long num = strtol(val, NULL, 10);
    const bool on = !strcmp(val, "on") || !strcmp(val, "yes")
                    || !strcmp(val, "true") || !strcmp(val, "1");
    const bool dflt = !strcmp(val, "default");

    if (!strcmp(key, "enabled")) {
        list[found].enabled = on;
    } else if (!strcmp(key, "bitrate")) {
        list[found].bitrate = dflt ? -1 : (int)num;
    } else if (!strcmp(key, "complexity")) {
        list[found].complexity = dflt ? -1 : (int)num;
    } else if (!strcmp(key, "loss")) {
        list[found].expected_loss_pct = dflt ? -1 : (int)num;
    } else if (!strcmp(key, "ptime")) {
        list[found].ptime_ms = dflt ? -1 : (int)num;
    } else if (!strcmp(key, "cbr")) {
        list[found].cbr = dflt ? -1 : (on ? 1 : 0);
    } else if (!strcmp(key, "first")) {
        /* Moving one codec to the front is the only reordering worth
           a command: everything else is easier in the config file. */
        sp_codec_params tmp = list[found];
        for (int i = found; i > 0; i--)
            list[i] = list[i - 1];
        list[0] = tmp;
    } else {
        printf("ERR unknown codec setting: %s\n", key);
        goto done;
    }

    st = sp_media_set_codecs(core, list, n);
    if (st != SP_OK)
        printf("ERR %s: %s\n", sp_strerror(st), sp_last_error_detail(core));
    else if (*val)
        printf("OK codec-set %s %s=%s\n", id, key, val);
    else
        printf("OK codec-set %s %s\n", id, key);

done:
    for (size_t i = 0; i < n; i++)
        free(ids[i]);
    free(ids);
    free(list);
}

static void cmd_rules(void) {
    const sp_rule_stat *stats = NULL;
    size_t n = 0;
    sp_status st = sp_rules_stats(core, &stats, &n);
    if (st != SP_OK) {
        printf("ERR %s: %s\n", sp_strerror(st), sp_last_error_detail(core));
        return;
    }
    for (size_t i = 0; i < n; i++)
        printf("OK rule %s hits=%llu\n", stats[i].name,
               (unsigned long long)stats[i].hits);
    printf("OK rules total=%zu\n", n);
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s --config PATH [--no-audio] [--auto-answer [code]]\n"
        "       %s --version\n"
        "\n"
        "Commands on stdin, one per line:\n"
        "  call <acc> <uri> [Name: value ...]   place a call\n"
        "  answer <call> [code]                 answer (default 200)\n"
        "  hangup <call> [code]                 hang up (default 603)\n"
        "  hold <call> on|off                   hold or resume\n"
        "  xfer <call> <uri>                    blind transfer\n"
        "  xfer-att <call-a> <call-b>           attended transfer\n"
        "  dtmf <call> <digits>                 send DTMF\n"
        "  rec <call> [path]                    start recording; no path "
        "uses the configured settings\n"
        "  rec-stop <call>                      stop recording\n"
        "  trace-export <call-id>               write one call's SIP "
        "trace to a file\n"
        "  play <path>                          play a recording back "
        "(WAV only)\n"
        "  play-stop                            stop playback\n"
        "  reg <acc> on|off                     register or unregister\n"
        "  siplog [call-id|-] [limit]           list captured SIP messages\n"
        "  siplog-dump <index>                  print one message verbatim\n"
        "  siplog-clear                         empty the SIP log\n"
        "  actions [limit]                      list the action history\n"
        "  actions-clear                        empty the action history\n"
        "  audio [all]                          list audio devices\n"
        "  sounds                               list ringtones and tones\n"
        "  sound-set <slot> <name|none>         choose a sound\n"
        "  sound-play <slot> | sound-stop       preview a sound\n"
        "  sound-import <slot> <file.wav>       import a sound\n"
        "  version                              build identity\n"
        "  order [newest|oldest]                sort the logs and history\n"
        "  levels                               show volume and mute\n"
        "  volume <tx%%> <rx%%>                 set volume, 0..200\n"
        "  mute <tx|rx|both> <on|off>           mute a direction\n"
        "  dsp                                  show audio processing\n"
        "  dsp-set <setting> <on|off>           toggle audio processing\n"
        "  audio-set <capture> <playback>       select audio devices\n"
        "  quality [call]                       audio quality of a call\n"
        "  history [limit]                      list past calls\n"
        "  history-clear                        empty the call history\n"
        "  theme                                show the resolved theme\n"
        "  accounts | calls | codecs | rules    list state\n"
        "  codec-set <id> <setting> <value>  order and tune codecs\n"
        "  modules | module-set <id> <on|off>  switch parts off\n"
        "  quit\n", argv0, argv0);
}

int main(int argc, char **argv) {
    const char *config = NULL;
    int no_audio = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            config = argv[++i];
        else if (!strcmp(argv[i], "--version")) {
            /* Before anything else: answering this must not need a
               config file or a SIP stack. */
            print_version();
            return 0;
        }
        else if (!strcmp(argv[i], "--no-audio"))
            no_audio = 1;
        else if (!strcmp(argv[i], "--auto-answer"))
            auto_answer_code = (i + 1 < argc && argv[i + 1][0] != '-')
                                   ? atoi(argv[++i]) : 200;
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!config) {
        usage(argv[0]);
        return 2;
    }

    /* Line buffered: the protocol is consumed through a pipe. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    sp_status st = sp_core_create(config, &core);
    if (st != SP_OK) {
        fprintf(stderr, "comlink-cli: cannot load %s\n", config);
        return 1;
    }
    sp_core_set_event_cb(core, on_event, NULL);

    st = sp_core_start(core);
    if (st != SP_OK) {
        fprintf(stderr, "comlink-cli: start failed: %s: %s\n",
                sp_strerror(st), sp_last_error_detail(core));
        sp_core_destroy(core);
        return 1;
    }

    if (no_audio) {
        st = sp_media_set_audio_device(core, "none");
        if (st != SP_OK)
            fprintf(stderr, "comlink-cli: null audio device: %s\n",
                    sp_last_error_detail(core));
    }

    printf("OK ready\n");

    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, stdin) > 0) {
        char *tok[MAX_TOKENS];
        size_t n = tokenise(line, tok, MAX_TOKENS);
        if (n == 0)
            continue;

        if (!strcmp(tok[0], "quit"))
            break;

        if (!strcmp(tok[0], "call") && n >= 3) {
            sp_header extra[MAX_EXTRA];
            size_t n_extra = 0;
            if (parse_headers(tok, n, 3, extra, &n_extra) != 0) {
                printf("ERR malformed ad-hoc header\n");
                continue;
            }
            sp_call_id id = 0;
            st = sp_call_make(core, (sp_account_id)strtoull(tok[1], NULL, 10),
                              tok[2], n_extra ? extra : NULL, n_extra, &id);
            report(st, "call %llu", (unsigned long long)id);
        } else if (!strcmp(tok[0], "answer") && n >= 2) {
            int code = (n >= 3) ? atoi(tok[2]) : 200;
            st = sp_call_answer(core, strtoull(tok[1], NULL, 10), code, NULL, 0);
            report(st, "answer %s %d", tok[1], code);
        } else if (!strcmp(tok[0], "hangup") && n >= 2) {
            int code = (n >= 3) ? atoi(tok[2]) : 603;
            st = sp_call_hangup(core, strtoull(tok[1], NULL, 10), code);
            report(st, "hangup %s %d", tok[1], code);
        } else if (!strcmp(tok[0], "hold") && n >= 3) {
            int on = !strcmp(tok[2], "on");
            st = sp_call_set_hold(core, strtoull(tok[1], NULL, 10), on);
            report(st, "hold %s %s", tok[1], on ? "on" : "off");
        } else if (!strcmp(tok[0], "xfer") && n >= 3) {
            st = sp_call_xfer_blind(core, strtoull(tok[1], NULL, 10), tok[2]);
            report(st, "xfer %s %s", tok[1], tok[2]);
        } else if (!strcmp(tok[0], "xfer-att") && n >= 3) {
            st = sp_call_xfer_attended(core, strtoull(tok[1], NULL, 10),
                                       strtoull(tok[2], NULL, 10));
            report(st, "xfer-att %s %s", tok[1], tok[2]);
        } else if (!strcmp(tok[0], "dtmf") && n >= 3) {
            st = sp_call_send_dtmf(core, strtoull(tok[1], NULL, 10), tok[2]);
            report(st, "dtmf %s %s", tok[1], tok[2]);
        } else if (!strcmp(tok[0], "rec") && n >= 2) {
            if (n >= 3) {
                st = sp_call_record_start(core, strtoull(tok[1], NULL, 10),
                                          tok[2]);
                report(st, "rec %s %s", tok[1], tok[2]);
            } else {
                const char *path = NULL;
                st = sp_call_record_start_auto(core,
                                               strtoull(tok[1], NULL, 10),
                                               &path);
                report(st, "rec %s", path);
            }
        } else if (!strcmp(tok[0], "rec-stop") && n >= 2) {
            st = sp_call_record_stop(core, strtoull(tok[1], NULL, 10));
            report(st, "rec-stop %s", tok[1]);
        } else if (!strcmp(tok[0], "trace-export") && n >= 2) {
            const char *path = NULL;
            st = sp_trace_export(core, tok[1], &path);
            report(st, "trace-export %s", path);
        } else if (!strcmp(tok[0], "play") && n >= 2) {
            st = sp_playback_start(core, tok[1]);
            report(st, "play %s", tok[1]);
        } else if (!strcmp(tok[0], "play-stop")) {
            st = sp_playback_stop(core);
            report(st, "play-stop");
        } else if (!strcmp(tok[0], "reg") && n >= 3) {
            int on = !strcmp(tok[2], "on");
            st = sp_account_set_registration(core, strtoull(tok[1], NULL, 10),
                                             on);
            report(st, "reg %s %s", tok[1], on ? "on" : "off");
        } else if (!strcmp(tok[0], "siplog")) {
            const char *cid = (n >= 2 && strcmp(tok[1], "-")) ? tok[1] : NULL;
            size_t limit = (n >= 3) ? strtoul(tok[2], NULL, 10) : 20;
            cmd_siplog(cid, limit);
        } else if (!strcmp(tok[0], "siplog-dump") && n >= 2) {
            cmd_siplog_dump(strtoul(tok[1], NULL, 10));
        } else if (!strcmp(tok[0], "siplog-clear")) {
            st = sp_siplog_clear(core);
            report(st, "siplog-clear");
        } else if (!strcmp(tok[0], "actions")) {
            cmd_actions((n >= 2) ? strtoul(tok[1], NULL, 10) : 20);
        } else if (!strcmp(tok[0], "actions-clear")) {
            st = sp_actionlog_clear(core);
            report(st, "actions-clear");
        } else if (!strcmp(tok[0], "audio")) {
            cmd_audio(n >= 2 && !strcmp(tok[1], "all"));
        } else if (!strcmp(tok[0], "audio-set")) {
            /* Two names, either "default" or "none" or a device name. */
            if (n < 3) {
                printf("ERR usage: audio-set <capture> <playback>\n");
            } else {
                st = sp_media_set_audio_devices(core, tok[1], tok[2]);
                report(st, "audio-set %s %s", tok[1], tok[2]);
            }
        } else if (!strcmp(tok[0], "sounds")) {
            cmd_sounds();
        } else if (!strcmp(tok[0], "sound-set")) {
            sp_sound_slot slot;
            if (n < 3 || !sound_slot_of(tok[1], &slot)) {
                printf("ERR usage: sound-set "
                       "<ringtone|ringback|notification> <name|none>\n");
            } else {
                st = sp_sound_set(core, slot, tok[2]);
                report(st, "sound-set %s %s", tok[1], tok[2]);
            }
        } else if (!strcmp(tok[0], "sound-play")) {
            sp_sound_slot slot;
            if (n < 2 || !sound_slot_of(tok[1], &slot)) {
                printf("ERR usage: sound-play "
                       "<ringtone|ringback|notification>\n");
            } else {
                st = sp_sound_preview(core, slot);
                report(st, "sound-play %s", tok[1]);
            }
        } else if (!strcmp(tok[0], "sound-stop")) {
            st = sp_sound_preview_stop(core);
            report(st, "sound-stop");
        } else if (!strcmp(tok[0], "sound-import")) {
            sp_sound_slot slot;
            if (n < 3 || !sound_slot_of(tok[1], &slot)) {
                printf("ERR usage: sound-import "
                       "<ringtone|ringback|notification> <file.wav>\n");
            } else {
                const char *stored = NULL;
                st = sp_sound_import(core, slot, tok[2], &stored);
                report(st, "sound-import %s", stored ? stored : tok[2]);
            }
        } else if (!strcmp(tok[0], "version")) {
            const sp_build_info *b = sp_build();
            printf("OK version %s commit=%s configured=%s pjproject=%s\n",
                   b->version, b->git_commit, b->configured_date,
                   b->pjproject_version);
        } else if (!strcmp(tok[0], "order")) {
            if (n < 2) {
                printf("OK order %s\n",
                       sp_log_get_order(core) == SP_ORDER_OLDEST_FIRST
                           ? "oldest" : "newest");
            } else if (!strcmp(tok[1], "oldest")) {
                report(sp_log_set_order(core, SP_ORDER_OLDEST_FIRST),
                       "order oldest");
            } else if (!strcmp(tok[1], "newest")) {
                report(sp_log_set_order(core, SP_ORDER_NEWEST_FIRST),
                       "order newest");
            } else {
                printf("ERR usage: order [newest|oldest]\n");
            }
        } else if (!strcmp(tok[0], "levels")) {
            cmd_levels();
        } else if (!strcmp(tok[0], "volume")) {
            if (n < 3)
                printf("ERR usage: volume <tx%%> <rx%%>\n");
            else
                cmd_volume(tok[1], tok[2]);
        } else if (!strcmp(tok[0], "mute")) {
            if (n < 3)
                printf("ERR usage: mute <tx|rx|both> <on|off>\n");
            else
                cmd_mute(tok[1], tok[2]);
        } else if (!strcmp(tok[0], "dsp")) {
            cmd_dsp();
        } else if (!strcmp(tok[0], "dsp-set")) {
            if (n < 3)
                printf("ERR usage: dsp-set <setting> <on|off>\n");
            else
                cmd_dsp_set(tok[1], tok[2]);
        } else if (!strcmp(tok[0], "quality")) {
            cmd_quality((n >= 2) ? tok[1] : NULL);
        } else if (!strcmp(tok[0], "history")) {
            cmd_history((n >= 2) ? strtoul(tok[1], NULL, 10) : 20);
        } else if (!strcmp(tok[0], "history-clear")) {
            st = sp_history_clear(core);
            report(st, "history-clear");
        } else if (!strcmp(tok[0], "theme")) {
            cmd_theme();
        } else if (!strcmp(tok[0], "accounts")) {
            cmd_accounts();
        } else if (!strcmp(tok[0], "calls")) {
            cmd_calls();
        } else if (!strcmp(tok[0], "modules")) {
            cmd_modules();
        } else if (!strcmp(tok[0], "module-set")) {
            if (n < 3)
                printf("ERR usage: module-set <id> <on|off>\n");
            else
                cmd_module_set(tok[1], tok[2]);
        } else if (!strcmp(tok[0], "codecs")) {
            cmd_codecs();
        } else if (!strcmp(tok[0], "codec-set")) {
            /* "first" is the one setting that is its own value. */
            const bool no_value = n == 3 && !strcmp(tok[2], "first");
            if (n < 4 && !no_value)
                printf("ERR usage: codec-set <id> "
                       "<enabled|bitrate|complexity|loss|ptime|cbr> <value> "
                       "| codec-set <id> first\n");
            else
                cmd_codec_set(tok[1], tok[2], no_value ? "" : tok[3]);
        } else if (!strcmp(tok[0], "rules")) {
            cmd_rules();
        } else {
            printf("ERR unknown or malformed command: %s\n", tok[0]);
        }
    }
    free(line);

    sp_core_stop(core);
    sp_core_destroy(core);
    return 0;
}
