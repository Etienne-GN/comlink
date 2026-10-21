#!/usr/bin/env bash
# SRTP is only meaningful if MANDATORY actually refuses an unprotected
# peer. Two cases over plain UDP, so the SDP is readable in the capture:
#   1. both mandatory      -> the call connects and the SDP offers SAVP
#   2. mandatory vs disabled -> the call MUST NOT connect
set -uo pipefail
CLI="${1:?cli}"; TMP="${2:?scratch}"; SRC="${3:?source dir}"
fail=0
check() {
    if [ "$2" = "$3" ]; then echo "  ok $1"
    else echo "  FAIL $1: got '$2' want '$3'"; fail=1; fi
}

WORK="$TMP/loopback-srtp"
rm -rf "$WORK"; mkdir -p "$WORK"

write_cfg() {   # name id user port srtp-mode
    cat > "$WORK/$1.yaml" <<YAML
log_level: 0
sip_port: $4
audio_device: none
codec_priority: [PCMU]
accounts:
  - id: $2
    user: $3
    domain: 127.0.0.1
    transport: udp
    register: false
    srtp: $5
YAML
}

# Returns "confirmed:<n>" for the caller.
run_case() {    # name callee-srtp caller-srtp aport bport
    local name="$1" bsrtp="$2" asrtp="$3" aport="$4" bport="$5"
    write_cfg "$name-bob" b bob "$bport" "$bsrtp"
    write_cfg "$name-alice" a alice "$aport" "$asrtp"

    local cap="$WORK/$name.pcap"
    sudo tcpdump -i lo -s0 -w "$cap" "udp port $aport or udp port $bport" \
        > /dev/null 2>&1 &
    local td=$!
    sleep 1

    sleep 12 | "$CLI" --config "$WORK/$name-bob.yaml" --no-audio \
        --auto-answer 200 > "$WORK/$name-bob.out" 2>&1 &
    local bob=$!
    sleep 2
    { echo "call 1 sip:bob@127.0.0.1:$bport"
      sleep 6
      echo "hangup 1 200"
      sleep 1
      echo "quit"
    } | "$CLI" --config "$WORK/$name-alice.yaml" --no-audio \
        > "$WORK/$name-alice.out" 2>&1
    wait $bob 2>/dev/null
    sleep 1
    sudo kill $td 2>/dev/null; wait $td 2>/dev/null
    sudo chmod a+r "$cap" 2>/dev/null
}

echo "=== case 1: both mandatory ==="
run_case both mandatory mandatory 5092 5093
check "the protected call connected" \
      "$([ "$(grep -c 'EV CALL_STATE.*CONFIRMED' "$WORK/both-alice.out")" -ge 1 ] && echo yes)" "yes"
savp="$(tshark -r "$WORK/both.pcap" -Y sdp -T fields -e sdp.media 2>/dev/null | grep -c 'SAVP')"
check "offer and answer both use the secure profile" \
      "$([ "$savp" -ge 2 ] && echo yes)" "yes"
plain="$(tshark -r "$WORK/both.pcap" -Y sdp -T fields -e sdp.media 2>/dev/null \
         | tr ',' '\n' | grep -c 'RTP/AVP')"
check "no unprotected profile was offered" "$plain" "0"
crypto="$(tshark -r "$WORK/both.pcap" -Y 'sdp' 2>/dev/null | wc -l)"
check "sdp was exchanged" "$([ "$crypto" -ge 2 ] && echo yes)" "yes"

echo "=== case 2: caller mandatory, callee disabled ==="
run_case mismatch disabled mandatory 5094 5095
check "the mismatched call did NOT connect" \
      "$(grep -c 'EV CALL_STATE.*CONFIRMED' "$WORK/mismatch-alice.out")" "0"
check "the mismatched call was reported as disconnected" \
      "$([ "$(grep -c 'EV CALL_STATE.*DISCONNECTED' "$WORK/mismatch-alice.out")" -ge 1 ] && echo yes)" "yes"

echo
echo "  case 1 m-lines : $(tshark -r "$WORK/both.pcap" -Y sdp -T fields -e sdp.media 2>/dev/null | tr '\n' ' ')"
echo "  case 2 states  : $(grep -o 'state=[A-Z]*' "$WORK/mismatch-alice.out" | tr '\n' ' ')"
echo "  captures       : $WORK"
exit $fail
