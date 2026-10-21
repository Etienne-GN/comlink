#!/usr/bin/env bash
# Regenerates every installed icon size from the master artwork: the
# desktop's, and the Android launcher's.
#
# The sizes are committed rather than generated at build time, so
# ImageMagick is not a build dependency of either package. Run this by
# hand when the artwork changes.
#
# The crop-and-recentre step is not cosmetic: the master has about ten
# per cent transparent padding, all of it off-centre, which wasted
# pixels at 16 and 22 where there are none to spare. It crops to what
# can be seen -- alpha above five per cent -- rather than using -trim:
# the master carries near-invisible pixels out to its left edge, and
# -trim kept them, so every icon was centred on a box wider than the
# artwork and sat visibly right of centre.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

IM=magick
command -v magick > /dev/null || IM=convert

RES=../../frontend/quick/android/res

ART="$(mktemp -p . comlink-art-XXXXXX.png)"
SQUARE="$(mktemp -p . comlink-square-XXXXXX.png)"
trap 'rm -f "$ART" "$SQUARE"' EXIT

VISIBLE="$("$IM" comlink-master.png -alpha extract -threshold 5% \
           -format '%@' info:)"
"$IM" comlink-master.png -crop "$VISIBLE" +repage "$ART"
"$IM" "$ART" -background none -gravity center -extent 1180x1180 "$SQUARE"

for s in 16 22 24 32 48 64 128 256 512; do
    "$IM" "$SQUARE" -resize "${s}x${s}" -strip \
          -define png:compression-level=9 "comlink-${s}.png"
    echo "comlink-${s}.png"
done

# Before API 26 the launcher draws the whole bitmap, so it is the
# desktop's square at each density's 48dp.
for d in mdpi:48 hdpi:72 xhdpi:96 xxhdpi:144 xxxhdpi:192; do
    "$IM" "$SQUARE" -resize "${d#*:}x${d#*:}" -strip \
          -define png:compression-level=9 "$RES/mipmap-${d%%:*}/ic_launcher.png"
    echo "mipmap-${d%%:*}/ic_launcher.png"
done

# From API 26 the launcher masks a 108dp canvas to its own shape and
# only the inner 66dp is sure to show. The artwork's longer side is 54
# per cent of the canvas, which keeps the octagon's corners inside
# that circle; the rest is the background colour.
for d in mdpi:108 hdpi:162 xhdpi:216 xxhdpi:324 xxxhdpi:432; do
    s="${d#*:}"
    a=$(( (s * 54 + 50) / 100 ))
    "$IM" "$ART" -resize "${a}x${a}" -background none -gravity center \
          -extent "${s}x${s}" -strip -define png:compression-level=9 \
          "$RES/drawable-${d%%:*}/ic_launcher_foreground.png"
    echo "drawable-${d%%:*}/ic_launcher_foreground.png"
done
