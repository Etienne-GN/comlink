#!/usr/bin/env bash
# Self-signed certificate for the TLS loopback test. Test material
# only: regenerated on demand, never shipped, never reused elsewhere.
set -euo pipefail
OUT="${1:?output directory}"
mkdir -p "$OUT"
if [ ! -f "$OUT/cert.pem" ]; then
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$OUT/key.pem" -out "$OUT/cert.pem" \
        -days 30 -subj "/CN=127.0.0.1" \
        -addext "subjectAltName=IP:127.0.0.1" 2>/dev/null
    chmod 600 "$OUT/key.pem"
fi
echo "$OUT"
