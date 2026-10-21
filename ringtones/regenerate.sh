#!/usr/bin/env bash
# Converts the supplied sound masters into the WAV files Comlink ships.
#
# pjmedia's file player reads WAV only: there is no MP3 decoder in the
# media stack, and adding one to play a ringtone would be a poor trade.
# So the masters stay here as MP3 and the shipped copies are WAV.
#
# 16 kHz mono 16-bit PCM: the conference bridge runs at 16 kHz by
# default, so anything else is resampled on the way out for no gain,
# and mono because a ringtone has no use for a stereo image.
#
# The WAVs are committed so ffmpeg is not a build dependency. Run this
# by hand when the masters change.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

command -v ffmpeg > /dev/null || { echo "ffmpeg is required" >&2; exit 1; }

for dir in inbound outbound notifications; do
    mkdir -p "packaging/sounds/$dir"
    for src in ringtones/"$dir"/*.mp3; do
        [ -e "$src" ] || continue
        out="packaging/sounds/$dir/$(basename "${src%.mp3}").wav"
        ffmpeg -loglevel error -y -i "$src" \
               -ac 1 -ar 16000 -acodec pcm_s16le "$out"
        echo "$out"
    done
done
