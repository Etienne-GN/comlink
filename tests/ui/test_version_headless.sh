#!/usr/bin/env bash
set -uo pipefail

# --version must answer without a display. It once did not: QApplication
# was constructed before the flag was looked at, so the binary aborted
# over ssh, in a container, and anywhere else without a platform plugin
# -- which is exactly where someone asks a binary what version it is.

APP="${1:?path to the comlink binary}"

fail=0
check() {
    if [ "$2" = "$3" ]; then echo "  ok $1"
    else echo "  FAIL $1: got '$2' want '$3'"; fail=1; fi
}

out="$(QT_QPA_PLATFORM=nonexistent-platform "$APP" --version 2>/dev/null)"
rc=$?
check "answers with no usable platform plugin" "$rc" "0"
check "and says what it is" \
      "$(grep -c '^comlink [0-9]' <<<"$out")" "1"
check "and which pjproject" \
      "$(grep -c '^pjproject ' <<<"$out")" "1"

# The normal path must keep working too.
out="$("$APP" --version 2>/dev/null)"
check "answers normally as well" "$(grep -c '^comlink [0-9]' <<<"$out")" "1"

exit $fail
