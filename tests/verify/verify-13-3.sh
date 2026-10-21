#!/usr/bin/env bash
# Spec item 13.3: does an rx rule take effect before pjsua's dialog
# layer consumes the message?
#
# A received-message rewrite is not visible in a capture of what we
# sent, so this is a differential: the same call placed to a callee
# with and without an rx rule that strips the inbound Contact header.
# Contact is required to form the dialog, so if our rewrite runs first
# the stripped case must behave differently from the control.
set -uo pipefail
CLI="${1:?cli}"; TMP="${2:?scratch}"; SRC="${3:?source dir}"
WORK="$TMP/verify-13-3"
rm -rf "$WORK"; mkdir -p "$WORK"

run_case() {   # name callee-config aport bport
    local name="$1" cfg="$2" aport="$3" bport="$4"
    sed "s/@PORT@/$bport/" "$SRC/tests/verify/$cfg" > "$WORK/$name-bob.yaml"
    sed -e "s/@PORT@/$aport/" -e 's/id: b$/id: a/' -e 's/user: bob/user: alice/' \
        "$SRC/tests/verify/rx-control.yaml" > "$WORK/$name-alice.yaml"

    sleep 12 | "$CLI" --config "$WORK/$name-bob.yaml" --no-audio \
        --auto-answer 200 > "$WORK/$name-bob.out" 2>&1 &
    local bob=$!
    sleep 2
    { echo "call 1 sip:bob@127.0.0.1:$bport"
      sleep 6
      echo "calls"
      echo "hangup 1 200"
      sleep 1
      echo "rules"
      echo "quit"
    } | "$CLI" --config "$WORK/$name-alice.yaml" --no-audio \
        > "$WORK/$name-alice.out" 2>&1
    wait $bob 2>/dev/null

    printf '%-9s caller: confirmed=%s disconnected=%s | callee: incoming=%s answered=%s\n' \
        "$name" \
        "$(grep -c 'EV CALL_STATE.*CONFIRMED' "$WORK/$name-alice.out")" \
        "$(grep -c 'EV CALL_STATE.*DISCONNECTED' "$WORK/$name-alice.out")" \
        "$(grep -c 'EV INCOMING_CALL' "$WORK/$name-bob.out")" \
        "$(grep -c '^OK auto-answer' "$WORK/$name-bob.out")"
}

echo "=== spec 13.3: rx rule ordering (differential) ==="
run_case control  rx-control.yaml 5084 5085
run_case stripped rx-strip.yaml   5086 5087
echo
echo "Identical outcomes mean rx rules run after pjsua has already"
echo "parsed the message, so they cannot influence its own handling."
echo "Different outcomes mean they run first and can."
echo "outputs: $WORK"
