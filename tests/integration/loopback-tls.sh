#!/usr/bin/env bash
# TLS must carry a real call, not merely compile. Evidence is the
# capture: a handshake must be present and the SIP must NOT be
# readable as cleartext, so a silent downgrade to UDP fails the test.
set -uo pipefail
CLI="${1:?cli}"; TMP="${2:?scratch}"; SRC="${3:?source dir}"
fail=0
check() {
    if [ "$2" = "$3" ]; then echo "  ok $1"
    else echo "  FAIL $1: got '$2' want '$3'"; fail=1; fi
}

APORT=5082; BPORT=5083
WORK="$TMP/loopback-tls"
rm -rf "$WORK"; mkdir -p "$WORK"
CERTDIR="$("$SRC/scripts/make-test-certs.sh" "$WORK/certs")"

for who in alice bob; do
    port=$APORT; [ "$who" = bob ] && port=$BPORT
    sed -e "s|@PORT@|$port|" -e "s|@CERTDIR@|$CERTDIR|" \
        "$SRC/tests/integration/tls-$who.yaml" > "$WORK/$who.yaml"
done

CAP="$WORK/call.pcap"
sudo tcpdump -i lo -s0 -w "$CAP" "tcp port $APORT or tcp port $BPORT" \
    > /dev/null 2>&1 &
TD=$!
sleep 1

sleep 16 | "$CLI" --config "$WORK/bob.yaml" --no-audio --auto-answer 200 \
    > "$WORK/bob.out" 2>&1 &
BOB=$!
sleep 3

{ echo "call 1 sip:bob@127.0.0.1:$BPORT;transport=tls X-Sp-Adhoc: over-tls"
  sleep 7
  echo "calls"
  echo "hangup 1 200"
  sleep 1
  echo "quit"
} | "$CLI" --config "$WORK/alice.yaml" --no-audio > "$WORK/alice.out" 2>&1

wait $BOB 2>/dev/null
sleep 1
sudo kill $TD 2>/dev/null; wait $TD 2>/dev/null
sudo chmod a+r "$CAP" 2>/dev/null

A="$WORK/alice.out"; B="$WORK/bob.out"
count() { tshark -r "$CAP" -Y "$1" 2>/dev/null | wc -l; }

check "no transport error on startup" \
      "$(grep -c 'EV TRANSPORT_ERROR' "$A")" "0"
check "the call was placed" \
      "$(grep -cx 'OK call 1' "$A")" "1"
check "the call confirmed over TLS" \
      "$([ "$(grep -c 'EV CALL_STATE.*CONFIRMED' "$A")" -ge 1 ] && echo yes)" "yes"
check "bob saw the incoming call" \
      "$(grep -c 'EV INCOMING_CALL' "$B")" "1"
check "media went active" \
      "$([ "$(grep -c 'EV CALL_MEDIA_STATE.*active=1' "$A")" -ge 1 ] && echo yes)" "yes"

# The assertions that matter.
check "a TLS handshake happened" \
      "$([ "$(count 'tls.handshake')" -ge 1 ] && echo yes)" "yes"
check "no cleartext SIP on the signalling ports" \
      "$(count 'sip')" "0"

echo
echo "  tls records            : $(count 'tls')"
echo "  tls handshake messages : $(count 'tls.handshake')"
echo "  cleartext sip packets  : $(count 'sip')"
echo "  capture                : $CAP"
exit $fail
