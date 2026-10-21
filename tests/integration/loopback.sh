#!/usr/bin/env bash
# Credential-free acceptance test: two instances place a direct
# IP-to-IP call, so SDP, RTP, codec order and header rewriting are all
# proven with no registrar and no credentials.
#
# Acceptance evidence is the packet capture, not a return code.
set -uo pipefail
CLI="${1:?cli}"; TMP="${2:?scratch}"; SRC="${3:?source dir}"
fail=0
check() {
    if [ "$2" = "$3" ]; then echo "  ok $1"
    else echo "  FAIL $1: got '$2' want '$3'"; fail=1; fi
}

APORT=5080; BPORT=5081
WORK="$TMP/loopback"
rm -rf "$WORK"; mkdir -p "$WORK"
sed "s/@PORT@/$APORT/" "$SRC/tests/integration/alice.yaml" > "$WORK/alice.yaml"
sed "s/@PORT@/$BPORT/" "$SRC/tests/integration/bob.yaml"   > "$WORK/bob.yaml"

CAP="$WORK/call.pcap"
sudo tcpdump -i lo -s0 -w "$CAP" udp > /dev/null 2>&1 &
TD=$!
sleep 1

# Bob answers whatever arrives and exits when stdin closes.
sleep 14 | "$CLI" --config "$WORK/bob.yaml" --no-audio --auto-answer 200 \
    > "$WORK/bob.out" 2>&1 &
BOB=$!
sleep 2

{ echo "call 1 sip:bob@127.0.0.1:$BPORT X-Sp-Adhoc: from-alice"
  sleep 6
  echo "calls"
  echo "rules"
  echo "siplog - 100"
  echo "actions 100"
  echo "history 20"
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

check "alice placed the call" \
      "$(grep -cx 'OK call 1' "$A")" "1"
check "alice saw the call confirm" \
      "$([ "$(grep -c 'EV CALL_STATE.*CONFIRMED' "$A")" -ge 1 ] && echo yes)" "yes"
check "bob saw an incoming call" \
      "$(grep -c 'EV INCOMING_CALL' "$B")" "1"
check "bob auto-answered" \
      "$(grep -c '^OK auto-answer' "$B")" "1"
check "media went active on alice" \
      "$([ "$(grep -c 'EV CALL_MEDIA_STATE.*active=1' "$A")" -ge 1 ] && echo yes)" "yes"
check "media went active on bob" \
      "$([ "$(grep -c 'EV CALL_MEDIA_STATE.*active=1' "$B")" -ge 1 ] && echo yes)" "yes"
check "alice saw the call disconnect" \
      "$([ "$(grep -c 'EV CALL_STATE.*DISCONNECTED' "$A")" -ge 1 ] && echo yes)" "yes"
check "the rule fired exactly once" \
      "$(grep -c '^OK rule strip-ua hits=1' "$A")" "1"

# What actually went on the wire.
check "an INVITE was sent" \
      "$(count 'sip.Method == "INVITE"')" "1"
check "ad-hoc header reached the wire" \
      "$(count 'sip.Method == "INVITE" && sip contains "X-Sp-Adhoc: from-alice"')" "1"
check "rule-injected header reached the wire" \
      "$(count 'sip.Method == "INVITE" && sip contains "X-Sp-Rule: applied"')" "1"
check "rule-removed header is absent from the wire" \
      "$(count 'sip.Method == "INVITE" && sip contains "User-Agent"')" "0"
check "the call was answered 200 on the wire" \
      "$([ "$(count 'sip.Status-Code == 200')" -ge 1 ] && echo yes)" "yes"
# Both offer and answer must lead with payload 0 (PCMU), which is what
# codec_priority asked for. Computed in a variable to keep the tshark
# filter clear of nested quoting.
audio_m="$(tshark -r "$CAP" -Y sdp -T fields -e sdp.media 2>/dev/null \
           | tr ',' '\n' | grep -c -E '^audio [0-9]+ RTP/AVP 0( |$)')"
check "offer and answer both lead with PCMU" "$audio_m" "2"

no_text="$(tshark -r "$CAP" -Y sdp -T fields -e sdp.media 2>/dev/null \
           | tr ',' '\n' | grep -c '^text ')"
check "no text stream is offered" "$no_text" "0"
check "a BYE ended the dialog" \
      "$([ "$(count 'sip.Method == "BYE"')" -ge 1 ] && echo yes)" "yes"
check "the sip log captured the outgoing INVITE" \
      "$([ "$(grep -cE '^OK sip [0-9]+ tx INVITE' "$A")" -ge 1 ] && echo yes)" "yes"
check "the sip log captured a received response" \
      "$([ "$(grep -cE '^OK sip [0-9]+ rx ' "$A")" -ge 1 ] && echo yes)" "yes"
check "captured messages are redacted" \
      "$(grep -cE '^OK sip [0-9]+ .* redacted=0' "$A")" "0"
check "the action log recorded the call" \
      "$([ "$(grep -cE '^OK action .* call ' "$A")" -ge 1 ] && echo yes)" "yes"
check "the call was recorded in history" \
      "$([ "$(grep -cE '^OK call-record .* out ' "$A")" -ge 1 ] && echo yes)" "yes"
check "the recorded call carries a sip call-id" \
      "$(grep -cE '^OK call-record .* call-id=- ' "$A")" "0"
check "media packets flowed both ways" \
      "$([ "$(count 'udp && !sip')" -gt 40 ] && echo yes)" "yes"

echo
echo "  rtp decoded by tshark : $(count 'rtp')"
echo "  non-sip udp packets   : $(count 'udp && !sip')"
echo "  m= lines seen         : $(tshark -r "$CAP" -Y sdp -T fields -e sdp.media 2>/dev/null | tr '\n' ' ')"
echo "  capture               : $CAP"
exit $fail
