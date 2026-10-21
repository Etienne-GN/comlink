#!/usr/bin/env bash
# The CLI's contract is its line protocol, so this drives the binary
# and asserts on stdout rather than linking against it.
set -uo pipefail
CLI="${1:?path to comlink-cli}"
TMP="${2:?scratch dir}"
fail=0
check() {
    if [ "$2" = "$3" ]; then echo "  ok $1"
    else echo "  FAIL $1: got '$2' want '$3'"; fail=1; fi
}

mkdir -p "$TMP/cli"
cat > "$TMP/cli/cli.yaml" <<'YAML'
log_level: 0
accounts: []
header_rules:
  - name: probe
    direction: tx
    message: request
    method: [INVITE]
    actions:
      - set: {name: X-Sp-Test, value: yes}
YAML

# The sound library the core reads. Derived from this script's own
# location rather than taken as an argument, so a manual run and the
# ctest run agree: without it the CLI resolves the bundled directory to
# whatever is installed on the machine, and the assertions below would
# be testing the last package rather than this tree.
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && cd .. && pwd)"
export COMLINK_SOUND_DIR="$SRC/packaging/sounds"

run() { printf '%s\n' "$@" | "$CLI" --config "$TMP/cli/cli.yaml" --no-audio 2>&1; }

out="$(run rules codecs quit)"

check "reports ready" \
      "$(grep -c '^OK ready' <<<"$out")" "1"
check "rules lists the configured rule" \
      "$(grep -c '^OK rule probe hits=0' <<<"$out")" "1"
check "codecs lists at least one codec" \
      "$([ "$(grep -c '^OK codec ' <<<"$out")" -ge 1 ] && echo yes)" "yes"
check "codecs includes PCMU" \
      "$(grep -c '^OK codec PCMU' <<<"$out")" "1"
check "codecs reports each one's settings, not just its name" \
      "$(grep -c '^OK codec PCMU.* bitrate=default .*cbr=default' <<<"$out")" "1"

# Ordering and per-codec settings, through the same ABI the GUI uses.
cout="$(run 'codec-set PCMA first' 'codec-set opus bitrate 24000' \
            'codec-set PCMU enabled off' codecs quit)"
check "a codec can be moved to the front" \
      "$(grep -c '^OK codec PCMA.* prio=254' <<<"$cout")" "1"
check "a codec can be switched off" \
      "$(grep -c '^OK codec PCMU.* enabled=no' <<<"$cout")" "1"
check "opus takes a bitrate" \
      "$(grep -c '^OK codec opus.* bitrate=24000' <<<"$cout")" "1"
check "a bitrate elsewhere is refused, not ignored" \
      "$(run 'codec-set PCMA bitrate 24000' quit | grep -c '^ERR ')" "1"
check "an unknown codec is reported" \
      "$(run 'codec-set nosuchcodec first' quit | grep -c '^ERR no such codec')" "1"
check "an unknown setting is reported" \
      "$(run 'codec-set PCMU frobnicate 1' quit | grep -c '^ERR unknown codec setting')" "1"

# Modules: off has to mean inert, not hidden.
mout="$(run modules quit)"
check "modules lists the base as not removable" \
      "$(grep -c '^OK module sip_core enabled=yes removable=no' <<<"$mout")" "1"
check "modules lists the optional ones as removable" \
      "$(grep -c '^OK module recording .*removable=yes' <<<"$mout")" "1"
check "the base cannot be switched off" \
      "$(run 'module-set sip_core off' quit | grep -c 'cannot be switched off')" "1"
check "an unknown module is reported" \
      "$(run 'module-set teleporter off' quit | grep -c '^ERR ')" "1"
check "a module switched off stays off" \
      "$(run 'module-set recording off' modules quit \
         | grep -c '^OK module recording enabled=no')" "1"
check "and switching it back on takes" \
      "$(run 'module-set recording on' modules quit \
         | grep -c '^OK module recording enabled=yes')" "1"

check "unknown command is an error, not a crash" \
      "$(run frobnicate quit | grep -c '^ERR unknown')" "1"
check "hangup on unknown call reports not found" \
      "$(run 'hangup 999999' quit | grep -c 'ERR handle not found')" "1"
check "dtmf with a bad alphabet is an argument error" \
      "$(run 'dtmf 1 12QZ' quit | grep -c 'ERR invalid argument')" "1"
check "quality on an unknown call reports not found" \
      "$(run 'quality 999999' quit | grep -c 'ERR handle not found')" "1"
check "rec with a path on an unknown call reports not found" \
      "$(run "rec 999999 $TMP/cli/x.wav" quit | grep -c 'ERR handle not found')" \
      "1"
check "rec without a path is accepted, not a usage error" \
      "$(run 'rec 999999' quit | grep -c 'ERR handle not found')" "1"
check "rec with no arguments at all still reports usage" \
      "$(run rec quit | grep -c '^ERR unknown or malformed command: rec')" \
      "1"
check "trace-export on an unknown call-id reports not found" \
      "$(run 'trace-export no-such-call-id' quit \
         | grep -c 'ERR handle not found')" "1"
check "trace-export with no arguments at all still reports usage" \
      "$(run trace-export quit \
         | grep -c '^ERR unknown or malformed command: trace-export')" "1"
check "play on a missing file reports not found, not usage" \
      "$(run "play $TMP/cli/nope.wav" quit \
         | grep -c 'ERR handle not found')" "1"
check "play with no arguments at all still reports usage" \
      "$(run play quit \
         | grep -c '^ERR unknown or malformed command: play')" "1"
check "play-stop with nothing loaded is not an error" \
      "$(run play-stop quit | grep -c '^OK play-stop')" "1"
check "quality with no calls says so rather than inventing zeroes" \
      "$(run quality quit | grep -c '^ERR no calls')" "1"
check "audio reports the devices in effect" \
      "$(run audio quit | grep -c '^OK audio capture=.* playback=')" "1"
check "audio enumerates at least one device" \
      "$([ "$(run audio quit | grep -c '^OK audio-device ')" -ge 1 ] \
        && echo yes)" "yes"
check "audio reports what it hid" \
      "$(run audio quit | grep -cE '^OK audio-devices shown=[0-9]+ hidden=[0-9]+ total=[0-9]+')" "1"
check "the short list is not the whole list" \
      "$(run audio quit | awk -F'[= ]' '/^OK audio-devices/ { exit !($3 <= $7) }' \
         && echo yes)" "yes"
check "audio all shows at least as many as audio" \
      "$([ "$(run 'audio all' quit | grep -c '^OK audio-device ')" -ge \
           "$(run audio quit | grep -c '^OK audio-device ')" ] && echo yes)" "yes"
check "every listed device is usable in some direction" \
      "$(run audio quit | grep '^OK audio-device ' \
         | grep -c 'in=0 out=0')" "0"
check "listed devices carry a label and a kind" \
      "$(run audio quit | grep '^OK audio-device ' \
         | grep -vc 'kind=.* label=')" "0"
check "audio-set rejects a device that does not exist" \
      "$(run 'audio-set no-such-mic default' quit \
         | grep -c 'no capture device named')" "1"
check "audio-set without both names is a usage error" \
      "$(run 'audio-set default' quit | grep -c '^ERR usage')" "1"
check "levels start at unity in both directions" \
      "$(run levels quit | grep -c '^OK levels tx=100% rx=100% tx_muted=0 rx_muted=0')" "1"
check "volume sets both directions independently" \
      "$(run 'volume 60 150' levels quit \
         | grep -c '^OK levels tx=60% rx=150%')" "1"
check "muting leaves the level alone, so unmuting restores it" \
      "$(run 'volume 60 150' 'mute tx on' levels quit \
         | grep -c '^OK levels tx=60% rx=150% tx_muted=1')" "1"
check "mute both takes both directions" \
      "$(run 'mute both on' levels quit | grep -c 'tx_muted=1 rx_muted=1')" "1"
check "volume is put back" \
      "$([ "$(run 'volume 100 100' 'mute both off' quit \
             | grep -c '^OK volume')" = 1 ] && echo yes)" "yes"
check "a volume beyond the range is rejected, not clamped" \
      "$(run 'volume 400 100' quit | grep -c 'levels must be 0.0..2.0')" "1"
check "mute rejects an unknown direction" \
      "$(run 'mute sideways on' quit | grep -c '^ERR usage')" "1"
check "volume without both values is a usage error" \
      "$(run 'volume 50' quit | grep -c '^ERR usage')" "1"
check "sounds lists all three slots" \
      "$(run sounds quit | grep -cE '^OK sound (ringtone|ringback|notification) = ')" "3"
check "sounds offers the bundled files" \
      "$([ "$(run sounds quit | grep -c '^OK sound-file ')" -ge 9 ] \
        && echo yes)" "yes"
check "the notification default is a busy tone, not a dial tone" \
      "$(run sounds quit | grep -c '^OK sound notification = .*busy')" "1"
check "sound-set rejects a name that does not exist" \
      "$(run 'sound-set ringtone no-such-tone' quit \
         | grep -c 'no sound named')" "1"
check "sound-set accepts silence" \
      "$(run 'sound-set ringback none' sounds quit \
         | grep -c '^OK sound ringback = none')" "1"
check "sound-set puts it back" \
      "$([ "$(run 'sound-set ringback freesound_community-cell_phone_tone_ringback_3_times-77262' quit | grep -c '^OK sound-set')" = 1 ] && echo yes)" "yes"
check "sound-set rejects an unknown slot" \
      "$(run 'sound-set frobnicate x' quit | grep -c '^ERR usage')" "1"
check "sound-import refuses a file that is not a wav" \
      "$(run 'sound-import ringtone /etc/hostname' quit \
         | grep -c 'only WAV')" "1"
check "dsp reports both groups of settings" \
      "$(run dsp quit | grep -c '^OK dsp ')" "2"
check "dsp says which settings are immediate and which wait" \
      "$([ "$(run dsp quit | grep -c '(immediate)')" = 1 ] && \
         [ "$(run dsp quit | grep -c '(next call)')" = 1 ] && echo yes)" "yes"
check "echo cancellation is on by default" \
      "$(run dsp quit | grep -c 'echo_cancel=1')" "1"
check "noise suppression is off by default" \
      "$(run dsp quit | grep -c 'noise_suppression=0')" "1"
# dsp-set persists, so this one mutates the config the other checks
# read. Restored immediately rather than left for the next check to
# trip over: the file is recreated per run today, which makes the
# ordering safe by accident rather than on purpose.
check "dsp-set toggles a setting" \
      "$(run 'dsp-set noise_suppression on' dsp quit \
         | grep -c 'noise_suppression=1')" "1"
check "dsp-set toggles it back" \
      "$(run 'dsp-set noise_suppression off' dsp quit \
         | grep -c 'noise_suppression=0')" "1"
check "dsp-set rejects an unknown setting" \
      "$(run 'dsp-set frobnicate on' quit | grep -c '^ERR unknown audio')" "1"
check "dsp-set without a value is a usage error" \
      "$(run 'dsp-set echo_cancel' quit | grep -c '^ERR usage')" "1"
check "attended transfer of a call to itself is rejected" \
      "$(run 'xfer-att 5 5' quit | grep -c 'ERR invalid argument')" "1"
check "malformed ad-hoc header is rejected" \
      "$(run 'call 1 sip:x@127.0.0.1 bogusheader' quit | grep -c 'ERR malformed')" "1"
check "accounts on an empty config reports zero" \
      "$(run accounts quit | grep -c '^OK accounts total=0')" "1"
check "calls with none active reports zero" \
      "$(run calls quit | grep -c '^OK calls total=0')" "1"

out="$(run rules siplog actions quit)"
check "siplog reports a total" \
      "$(grep -c '^OK siplog total=' <<<"$out")" "1"
check "actions reports a total" \
      "$(grep -c '^OK actions total=' <<<"$out")" "1"
check "siplog-clear is accepted" \
      "$(run siplog-clear quit | grep -c '^OK siplog-clear')" "1"
check "actions-clear is accepted" \
      "$(run actions-clear quit | grep -c '^OK actions-clear')" "1"
check "siplog-dump on an empty log is an error, not a crash" \
      "$(run siplog-clear 'siplog-dump 0' quit | grep -c '^ERR')" "1"

check "history reports a total" \
      "$(run history quit | grep -c '^OK history total=')" "1"
check "history-clear is accepted" \
      "$(run history-clear quit | grep -c '^OK history-clear')" "1"

check "theme reports resolved values" \
      "$(run theme quit | grep -c '^OK theme accent=#')" "1"
check "theme reports a multiplier" \
      "$(run theme quit | grep -c 'mult=1.00')" "1"

check "bad config exits nonzero" \
      "$("$CLI" --config /nonexistent/x.yaml --no-audio >/dev/null 2>&1; echo $?)" "1"
check "missing --config exits 2" \
      "$("$CLI" >/dev/null 2>&1; echo $?)" "2"

# A rejected config must name the offending key rather than fail vaguely.
cat > "$TMP/cli/bad.yaml" <<'YAML'
accounts:
  - id: a
    user: u
    domain: d
    passwrod: typo
YAML
check "config typo is reported with the key name" \
      "$("$CLI" --config "$TMP/cli/bad.yaml" --no-audio 2>&1 | grep -c 'passwrod')" "1"

exit $fail
