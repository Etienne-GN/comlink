#!/usr/bin/env bash
# Blind transfer to a bare extension, the way an operator types it.
# A REFER whose Refer-To is "6043" rather than a SIP URI is accepted
# with 202 by FreeSWITCH and then goes nowhere, so the judgement is
# the Refer-To that alice actually sent.
#
# alice's domain carries a port nothing listens on: bob acts on the
# REFER and dials the target, and that call must reach no one.
set -uo pipefail
CLI="${1:?cli}"; TMP="${2:?scratch}"; SRC="${3:?source dir}"
fail=0
check() {
    if [ "$2" = "$3" ]; then echo "  ok $1"
    else echo "  FAIL $1: got '$2' want '$3'"; fail=1; fi
}

APORT=5082; BPORT=5083; DEAD=5097
WORK="$TMP/loopback-xfer"
rm -rf "$WORK"; mkdir -p "$WORK"
sed -e "s/@PORT@/$APORT/" -e "s/domain: 127.0.0.1/domain: 127.0.0.1:$DEAD/" \
    "$SRC/tests/integration/alice.yaml" > "$WORK/alice.yaml"
sed "s/@PORT@/$BPORT/" "$SRC/tests/integration/bob.yaml" > "$WORK/bob.yaml"

sleep 10 | "$CLI" --config "$WORK/bob.yaml" --no-audio --auto-answer 200 \
    > "$WORK/bob.out" 2>&1 &
BOB=$!
sleep 2

{ echo "call 1 sip:bob@127.0.0.1:$BPORT"
  sleep 3
  echo "xfer 1 6043"
  sleep 2
  for i in $(seq 0 19); do echo "siplog-dump $i"; done
  echo "hangup 1 200"
  sleep 1
  echo "quit"
} | "$CLI" --config "$WORK/alice.yaml" --no-audio > "$WORK/alice.out" 2>&1

wait $BOB 2>/dev/null

A="$WORK/alice.out"
refer_to="$(awk '/^OK sip-dump [0-9]+ tx REFER/{r=1; next}
                 /^OK sip-dump end/{r=0}
                 r && /^Refer-To:/{sub(/\r$/, ""); print; exit}' "$A")"

check "alice placed the call" "$(grep -cx 'OK call 1' "$A")" "1"
check "the transfer was accepted by the core" \
      "$(grep -c '^OK xfer 1 6043' "$A")" "1"
check "a bare extension is sent as a SIP URI on the account's domain" \
      "$refer_to" "Refer-To: sip:6043@127.0.0.1:$DEAD"
check "bob accepted the REFER" \
      "$([ "$(grep -c 'EV XFER_STATUS call=1 ' "$A")" -ge 1 ] && echo yes)" "yes"
exit $fail
