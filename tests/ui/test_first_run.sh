#!/usr/bin/env bash
set -uo pipefail

# A fresh install has no configuration. It used to show a box telling
# the operator to copy a file by hand and then exit, which stopped
# every new user at the door -- and the file it pointed at ships an
# account for pbx.example.lan with register: true, so following the
# instructions produced a phone failing to register on first launch.

APP="${1:?path to the comlink binary}"
BUILD="${2:?build directory}"
SRC="${3:?source directory}"

fail=0
check() {
    if [ "$2" = "$3" ]; then echo "  ok $1"
    else echo "  FAIL $1: got '$2' want '$3'"; fail=1; fi
}

TMP="$BUILD/first-run-test"
rm -rf "$TMP"; mkdir -p "$TMP"
# The hermetic ALSA setup lives in XDG_CONFIG_HOME, which is the very
# thing being emptied here, so it has to come along.
cp -r "$SRC/tests/alsa-home/alsa" "$TMP/" 2>/dev/null

cfg="$TMP/comlink/comlink.yaml"
XDG_CONFIG_HOME="$TMP" QT_QPA_PLATFORM=offscreen \
    COMLINK_QUICK_SHOT="$TMP/first.png" timeout 60 "$APP" >/dev/null 2>&1
check "starts with nothing in place" "$([ -f "$TMP/first.png" ] && echo yes)" "yes"
check "and wrote a configuration" "$([ -f "$cfg" ] && echo yes)" "yes"
check "which declares no accounts" \
      "$(grep -c '^accounts: \[\]' "$cfg")" "1"
check "and no example PBX came with it" \
      "$(grep -c 'pbx.example.lan' "$cfg")" "0"

# Second start must reuse it rather than overwrite it.
marker="$TMP/comlink/marker"
touch "$marker"
XDG_CONFIG_HOME="$TMP" QT_QPA_PLATFORM=offscreen \
    COMLINK_QUICK_SHOT="$TMP/second.png" timeout 60 "$APP" >/dev/null 2>&1
check "a second start keeps the same directory" "$([ -f "$marker" ] && echo yes)" "yes"

exit $fail
