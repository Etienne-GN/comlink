#!/usr/bin/env bash
# Every pane and every overlay, loaded offscreen, asserting no QML
# warning.
#
# A QML warning is not an error: a missing property, a type error or a
# failed binding is reported and the pane carries on, so a pane that
# half-loaded looks exactly like one that rendered. COMLINK_QUICK_STRICT
# makes the process exit non-zero on any of them, which is what turns
# "it looked fine" into a result.
#
# Each surface is also written to a PNG, so a render regression has a
# picture beside the verdict.
set -uo pipefail
BIN="${1:?comlink-quick binary}"
OUT="${2:?output directory}"

mkdir -p "$OUT"
export QT_QPA_PLATFORM=offscreen
export COMLINK_QUICK_STRICT=1
# A package build runs under the C locale, and Qt says so on stderr
# every time. Harmless, but noise in a log somebody reads only when
# something has gone wrong.
export LC_ALL="${LC_ALL:-C.UTF-8}"
# Its own configuration, so the probe touches nothing the operator
# owns and two runs cannot disagree about what is in the book.
export XDG_CONFIG_HOME="$OUT/home"
rm -rf "$XDG_CONFIG_HOME"
mkdir -p "$XDG_CONFIG_HOME"
# And the asoundrc every other test runs with: this home replaces
# tests/alsa-home, so without it ALSA's default is the pipewire plugin,
# which hangs when there is no session -- dpkg-buildpackage's case.
# See docs/known-issues.md.
cp -r "$(dirname "${BASH_SOURCE[0]}")/../alsa-home/alsa" "$XDG_CONFIG_HOME/"

fail=0
run() {
    local name="$1"; shift
    local log="$OUT/$name.log"
    if env "$@" COMLINK_QUICK_SHOT="$OUT/$name.png" \
        timeout 60 "$BIN" > "$log" 2>&1; then
        if [ -s "$OUT/$name.png" ]; then
            echo "  ok   $name"
        else
            echo "  FAIL $name: no image written"
            fail=1
        fi
    else
        echo "  FAIL $name: exit $? -- see $log"
        sed -n '1,8p' "$log" | sed 's/^/        /'
        fail=1
    fi
}

echo "panes"
i=0
for pane in keypad quality activity settings contacts; do
    run "pane-$pane" COMLINK_QUICK_PANE="$i"
    i=$((i + 1))
done

echo "overlays"
for ov in logs headers contact account accountlist picture settings calllog menu; do
    run "overlay-$ov" COMLINK_QUICK_OVERLAY="$ov"
done

echo "transcript"
# The dialog itself rather than the window behind it, at the desktop's
# size and at the handset's.
run "dialog-settings-transcript-560x460" COMLINK_QUICK_OVERLAY="settings:7" \
    COMLINK_QUICK_DIALOG=1 COMLINK_QUICK_SHOT_SIZE=560x460
run "dialog-settings-transcript-270x560" COMLINK_QUICK_OVERLAY="settings:7" \
    COMLINK_QUICK_DIALOG=1 COMLINK_QUICK_SHOT_SIZE=270x560

run "dialog-logs-transcript-640x480" COMLINK_QUICK_OVERLAY="transcript" \
    COMLINK_QUICK_DIALOG=1 COMLINK_QUICK_SHOT_SIZE=640x480
run "dialog-logs-transcript-270x560" COMLINK_QUICK_OVERLAY="transcript" \
    COMLINK_QUICK_DIALOG=1 COMLINK_QUICK_SHOT_SIZE=270x560

if [ "$fail" = 0 ]; then
    echo "every pane and overlay loaded with no QML warning"
else
    echo "QML PANE CHECKS FAILED"
fi
exit "$fail"
