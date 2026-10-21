#!/usr/bin/env bash
# Spec item 13.1: does a pjsip_module tx-path edit reach the wire for
# an initial request, an in-dialog request, and a response?
set -uo pipefail
BIN="${1:?path to verify_tx_mutation}"
TMP="${2:?scratch dir}"
mkdir -p "$TMP/verify"
CFG="$TMP/verify/probe.yaml"
CAP="$TMP/verify/probe.pcap"

cat > "$CFG" <<'YAML'
log_level: 0
sip_port: 5080
codec_priority: [PCMU]
accounts:
  - id: probe
    user: probe
    domain: 127.0.0.1
    transport: udp
    register: false
header_rules:
  - name: probe-requests
    direction: tx
    message: request
    actions:
      - set: {name: X-Sp-Probe-Req, value: "1"}
  - name: probe-responses
    direction: tx
    message: response
    actions:
      - set: {name: X-Sp-Probe-Resp, value: "1"}
  - name: strip-ua
    direction: tx
    message: request
    method: [INVITE]
    actions:
      - remove: User-Agent
YAML

sudo tcpdump -i lo -s0 -w "$CAP" 'udp port 5080' > /dev/null 2>&1 &
TD=$!
sleep 1
"$BIN" "$CFG"
RC=$?
sleep 1
sudo kill "$TD" 2>/dev/null; wait "$TD" 2>/dev/null
sudo chmod a+r "$CAP" 2>/dev/null

count() { tshark -r "$CAP" -Y "$1" 2>/dev/null | wc -l; }

echo
echo "=== spec 13.1 result ==="
printf 'harness exit                       : %s\n' "$RC"
printf 'INVITEs on the wire                : %s\n' "$(count 'sip.Method == "INVITE"')"
printf 'INVITEs carrying rule header       : %s\n' "$(count 'sip.Method == "INVITE" && sip contains "X-Sp-Probe-Req"')"
printf 'INVITEs carrying ad-hoc header     : %s\n' "$(count 'sip.Method == "INVITE" && sip contains "X-Sp-Adhoc"')"
printf 'INVITEs still carrying User-Agent  : %s\n' "$(count 'sip.Method == "INVITE" && sip contains "User-Agent"')"
printf 'BYEs on the wire                   : %s\n' "$(count 'sip.Method == "BYE"')"
printf 'BYEs carrying rule header          : %s\n' "$(count 'sip.Method == "BYE" && sip contains "X-Sp-Probe-Req"')"
printf '200 OKs on the wire                : %s\n' "$(count 'sip.Status-Code == 200')"
printf '200 OKs carrying rule header       : %s\n' "$(count 'sip.Status-Code == 200 && sip contains "X-Sp-Probe-Resp"')"
printf 'RTP packets                        : %s\n' "$(count 'rtp')"
echo "capture: $CAP"
