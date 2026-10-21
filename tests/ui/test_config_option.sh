#!/usr/bin/env bash
set -uo pipefail

# --config must decide where the application reads and writes.
#
# The QML frontend computed the path itself and ignored the option
# entirely, which is the sort of thing nobody notices until an
# operator keeps two configurations, edits one and runs the other.

APP="${1:?path to the comlink binary}"
BUILD="${2:?build directory}"

fail=0
check() {
    if [ "$2" = "$3" ]; then echo "  ok $1"
    else echo "  FAIL $1: got '$2' want '$3'"; fail=1; fi
}

TMP="$BUILD/config-option"
rm -rf "$TMP"; mkdir -p "$TMP"
CFG="$TMP/elsewhere.yaml"

# No file at all: the application must create the one it was told to
# use, not the one in the platform's own location.
QT_QPA_PLATFORM=offscreen COMLINK_QUICK_SHOT="$TMP/shot.png" \
    timeout 60 "$APP" --config "$CFG" > "$TMP/out.log" 2>&1

check "the named file was created" "$([ -f "$CFG" ] && echo yes || echo no)" \
      "yes"
check "and it is the one that was used" \
      "$(grep -c 'accounts:' "$CFG" 2>/dev/null)" "1"
check "the database landed beside it" \
      "$([ -f "$TMP/comlink.db" ] && echo yes || echo no)" "yes"

exit $fail
