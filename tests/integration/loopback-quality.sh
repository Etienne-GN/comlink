#!/usr/bin/env bash
# Acceptance for the audio quality metrics: two instances place a
# direct IP-to-IP call, hold it up long enough for RTCP to be
# exchanged, and the counters have to move.
#
# The point of this test is that a return code proves nothing about a
# measurement. What is asserted here is RTP that actually flowed, RTCP
# that actually arrived, and figures that match the capture.
set -uo pipefail
CLI="${1:?cli}"; TMP="${2:?scratch}"; SRC="${3:?source dir}"
fail=0
check() {
    if [ "$2" = "$3" ]; then echo "  ok $1"
    else echo "  FAIL $1: got '$2' want '$3'"; fail=1; fi
}

APORT=5084; BPORT=5085
WORK="$TMP/loopback-quality"
rm -rf "$WORK"; mkdir -p "$WORK"

# Sampled four times a second, so a 14-second call produces plenty of
# events without the interval itself being what is under test.
#
# Each instance gets its own database. The default puts it beside the
# config, and both configs live in this one directory, so without this
# the two ends share a history and each one's "history" command lists
# the other's record of the same call.
{ sed "s/@PORT@/$APORT/" "$SRC/tests/integration/alice.yaml"
  echo "metrics_interval_ms: 250"
  echo "db_file: $WORK/alice.db"
} > "$WORK/alice.yaml"
{ sed "s/@PORT@/$BPORT/" "$SRC/tests/integration/bob.yaml"
  echo "db_file: $WORK/bob.db"
} > "$WORK/bob.yaml"

CAP="$WORK/quality.pcap"
sudo tcpdump -i lo -s0 -w "$CAP" udp > /dev/null 2>&1 &
TD=$!
sleep 1

sleep 26 | "$CLI" --config "$WORK/bob.yaml" --no-audio --auto-answer 200 \
    > "$WORK/bob.out" 2>&1 &
BOB=$!
sleep 2

# RTCP goes out roughly every five seconds, so the call is held for
# fourteen: without a couple of received reports, rtt and every tx_
# figure would legitimately be zero and this test would be asserting
# nothing.
{ echo "call 1 sip:bob@127.0.0.1:$BPORT"
  sleep 14
  echo "quality 1"
  sleep 1
  echo "quality"
  sleep 1
  echo "hangup 1 200"
  sleep 2
  echo "history 5"
  sleep 1
  echo "quit"
} | "$CLI" --config "$WORK/alice.yaml" --no-audio > "$WORK/alice.out" 2>&1

wait $BOB 2>/dev/null
sleep 1
sudo kill $TD 2>/dev/null; wait $TD 2>/dev/null
sudo chmod a+r "$CAP" 2>/dev/null

A="$WORK/alice.out"; B="$WORK/bob.out"
count() { tshark -r "$CAP" -Y "$1" 2>/dev/null | wc -l; }
# The value of a single-value quality line, e.g. "OK mos 1 4.41".
val() { awk -v k="$2" '$2 == k { print $4; exit }' "$1"; }

check "alice placed the call" \
      "$(grep -cx 'OK call 1' "$A")" "1"
check "media went active" \
      "$([ "$(grep -c 'EV CALL_MEDIA_STATE.*active=1' "$A")" -ge 1 ] \
        && echo yes)" "yes"

# --- the sampler ---------------------------------------------------
check "sampling produced a stream of events" \
      "$([ "$(grep -c '^EV CALL_QUALITY' "$A")" -ge 10 ] && echo yes)" "yes"
check "the events name the call" \
      "$([ "$(grep -c '^EV CALL_QUALITY call=1' "$A")" -ge 10 ] \
        && echo yes)" "yes"

# --- the snapshot ---------------------------------------------------
check "the snapshot is valid, not no-media" \
      "$(grep -c 'no-media' "$A")" "0"
check "a mos was reported" \
      "$([ -n "$(val "$A" mos)" ] && echo yes)" "yes"
check "rtp actually flowed inbound" \
      "$(awk '$2 == "rx-packets" { exit !($4 > 100) }' "$A" && echo yes)" "yes"
check "rtp actually flowed outbound" \
      "$(awk '$2 == "tx-packets" { exit !($4 > 100) }' "$A" && echo yes)" "yes"
check "a clean loopback loses nothing" \
      "$(awk '$2 == "rx-loss" { exit !($4 == 0) }' "$A" && echo yes)" "yes"
check "mos is inside the model's range" \
      "$(awk '$2 == "mos" { exit !($4 >= 1.0 && $4 <= 4.5) }' "$A" \
        && echo yes)" "yes"
check "a clean loopback scores well" \
      "$(awk '$2 == "mos" { exit !($4 > 4.0) }' "$A" && echo yes)" "yes"
check "the r factor came with it" \
      "$(awk '$2 == "r-factor" { exit !($4 > 80) }' "$A" && echo yes)" "yes"
check "the jitter buffer reported its delays" \
      "$([ "$(grep -c '^OK jb-delay-ms' "$A")" -ge 1 ] && echo yes)" "yes"
check "jitter was measured, not left at the sentinel" \
      "$([ "$(grep -cE '^OK rx-jitter-us 1 cur=[0-9]+ min=[0-9]+ max=[0-9]+ avg=[0-9]+' \
             "$A")" -ge 1 ] && echo yes)" "yes"
check "quality with no argument found the call" \
      "$([ "$(grep -c '^OK quality 1' "$A")" -ge 2 ] && echo yes)" "yes"

# --- the figures survive the call ----------------------------------
# The final snapshot is taken in on_stream_destroyed. Taken any later
# the counters would already be gone and this row would read zero,
# which is the specific bug these two checks exist to catch.
check "history kept the mos" \
      "$(grep -cE '^OK call-quality [0-9]+ mos=[1-4]\.[0-9]+' "$A")" "1"
check "history kept the packet counts" \
      "$(grep -E '^OK call-quality' "$A" \
         | grep -cE 'pkt=[1-9][0-9]*/[1-9][0-9]*')" "1"

# --- what was on the wire ------------------------------------------
check "rtp reached the wire" \
      "$([ "$(count 'udp && !sip')" -gt 400 ] && echo yes)" "yes"
check "rtcp was exchanged, which is what makes rtt real" \
      "$([ "$(count 'rtcp')" -ge 2 ] && echo yes)" "yes"

echo
echo "  quality events        : $(grep -c '^EV CALL_QUALITY' "$A")"
echo "  reported rx packets   : $(val "$A" rx-packets)"
echo "  reported tx packets   : $(val "$A" tx-packets)"
echo "  reported mos          : $(val "$A" mos)"
echo "  non-sip udp packets   : $(count 'udp && !sip')"
echo "  rtcp packets          : $(count 'rtcp')"
echo "  history row           : $(grep -E '^OK call-quality' "$A" | head -1)"
echo "  capture               : $CAP"
exit $fail
