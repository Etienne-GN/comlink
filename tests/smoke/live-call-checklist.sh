#!/usr/bin/env bash
# Drives the features that only a live PBX and human ears can confirm:
# hold, resume, recording, DTMF and transfer.
#
# These cannot be automated. What this does instead is drive them in a
# fixed order and record a pass or fail for each, so the unproven set
# is a written result rather than an assumption.
#
# Usage:
#   live-call-checklist.sh <cli> <your-config.yaml> <target> [outfile]
#
# Pick a target with unambiguous audio: hold music, an echo test, or a
# speaking-clock extension. Silence proves nothing either way.
set -uo pipefail
CLI="${1:?path to comlink-cli}"
CFG="${2:?your config, with a working registration}"
DEST="${3:?extension or URI to call, e.g. an echo test}"
OUT="${4:-./smoke-result.txt}"

INTERACTIVE=0
[ -t 0 ] && [ -r /dev/tty ] && INTERACTIVE=1

: > "$OUT"
pass() { echo "PASS  $1" >> "$OUT"; }
fails() { echo "FAIL  $1" >> "$OUT"; }
skip() { echo "SKIP  $1" >> "$OUT"; }

# Never block on a terminal that is not there: this script has to be
# runnable non-interactively to check its own plumbing.
ask() {
    if [ "$INTERACTIVE" -eq 0 ]; then
        skip "$1 (not interactive)"
        return
    fi
    printf '\n  %s [y/n] ' "$1"
    local a=""
    read -r a < /dev/tty || a=""
    case "$a" in
        y|Y) pass "$1" ;;
        *)   fails "$1" ;;
    esac
}

FIFO="$(mktemp -u)"
mkfifo "$FIFO"
"$CLI" --config "$CFG" > "$OUT.log" 2>&1 < "$FIFO" &
CLIPID=$!
# Hold the write end open so the CLI does not see EOF and quit.
exec 3> "$FIFO"

cleanup() {
    echo "quit" >&3 2>/dev/null || true
    exec 3>&- 2>/dev/null || true
    sleep 1
    kill "$CLIPID" 2>/dev/null || true
    wait "$CLIPID" 2>/dev/null || true
    rm -f "$FIFO"
}
trap cleanup EXIT

# 35s, not 20: a REGISTER to a host that never answers retransmits for
# about 32 seconds before pjsip gives up, so a shorter window reports
# "not registered" before the stack has actually decided. Against a
# registrar that responds this returns in well under a second.
echo "waiting up to 35s for registration..."
registered=0
for _ in $(seq 1 35); do
    if grep -q 'EV ACCOUNT_STATE.*registered=1' "$OUT.log" 2>/dev/null; then
        registered=1
        break
    fi
    # A definitive rejection is an answer too; stop waiting for it.
    if grep -q 'EV ACCOUNT_STATE.*registered=0.*code=[45]' "$OUT.log" 2>/dev/null; then
        break
    fi
    sleep 1
done
if [ "$registered" -eq 1 ]; then
    pass "registration"
else
    fails "registration"
    echo "  not registered. Last account event:"
    grep 'EV ACCOUNT_STATE' "$OUT.log" | tail -1 || echo "  (none seen)"
    echo "  full log: $OUT.log"
    echo
    echo "=== result ==="
    cat "$OUT"
    exit 1
fi

echo "calling $DEST ..."
echo "call 1 $DEST X-Sp-Smoke: checklist" >&3
sleep 7
if grep -q 'EV CALL_STATE.*CONFIRMED' "$OUT.log"; then
    pass "call connected"
else
    fails "call connected"
fi
ask "Do you hear audio from the far end?"

echo "hold 1 on" >&3
sleep 3
ask "Did the audio stop (call on hold)?"

echo "hold 1 off" >&3
sleep 3
ask "Did the audio come back (call resumed)?"

REC="$(mktemp -u).wav"
echo "rec 1 $REC" >&3
sleep 5
echo "rec-stop 1" >&3
sleep 1
if [ -s "$REC" ]; then
    pass "recording produced a non-empty file ($REC)"
    echo "  recording: $REC  ($(stat -c%s "$REC") bytes)"
    ask "Does that recording play back with audible audio?"
else
    fails "recording produced no file"
fi

echo "dtmf 1 1234" >&3
sleep 2
ask "Did the far end react to the DTMF, or did you hear the tones?"

if [ "$INTERACTIVE" -eq 1 ]; then
    printf '\n  Blind transfer target (blank to skip): '
    XFER=""
    read -r XFER < /dev/tty || XFER=""
    if [ -n "$XFER" ]; then
        echo "xfer 1 $XFER" >&3
        sleep 7
        ask "Did the call transfer to $XFER?"
    else
        skip "blind transfer (no target given)"
    fi
else
    skip "blind transfer (not interactive)"
fi

# Audio quality, while the call is still up. The counters are the
# authoritative figures; the MOS beside them is an estimate.
echo "quality 1" >&3
sleep 2
ask "Does the quality readout show a non-zero rx packet count?"
ask "Is the MOS between 1.0 and 4.5, and plausible for this call?"

echo "hangup 1 200" >&3
sleep 2

# The final snapshot is taken in on_stream_destroyed, which runs before
# DISCONNECTED reaches us. If it were taken any later the counters
# would already be gone and this row would read as zeroes.
echo "history" >&3
sleep 2
ask "Does the history row for that call carry a non-zero MOS?"

echo
echo "=== result ==="
cat "$OUT"
echo
echo "log: $OUT.log"
grep -q '^FAIL' "$OUT" && exit 1
exit 0
