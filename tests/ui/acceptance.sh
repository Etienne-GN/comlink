#!/usr/bin/env bash
# The acceptance test: the same window, on both platforms, side by
# side.
#
# "I want to put my cellphone with comlink opened beside my debian
# screen where comlink is open, and I want to see the same on both."
# The question this answers is whether you can tell which is which.
#
# The desktop half is captured here. The handset half is captured with
# adb -- the phone has no environment to set, so its panes are reached
# by tapping the strip, which is the honest way round: it proves the
# strip works as well as the pane does.
#
#   adb shell input tap <x> 2214      # 117 350 540 750 960 on a 1080
#   adb shell screencap -p /sdcard/s.png
#   adb pull /sdcard/s.png emu-<n>.png
#
# Then this composes each pair. Both sides must be showing the same
# account, the same contact and the same theme, or the differences you
# see are the data's rather than the interface's.
set -uo pipefail
BIN="${1:?comlink-quick binary}"
OUT="${2:?output directory}"
PHONE="${3:-}"           # directory holding emu-0.png .. emu-4.png

mkdir -p "$OUT"
export QT_QPA_PLATFORM=offscreen

names=(keypad contacts quality activity settings)
for i in 0 1 2 3 4; do
    COMLINK_QUICK_PANE="$i" COMLINK_QUICK_SHOT="$OUT/desk-$i.png" \
        timeout 60 "$BIN" > "$OUT/desk-$i.log" 2>&1
    [ -s "$OUT/desk-$i.png" ] && echo "  ok   ${names[$i]} (Debian)" \
        || { echo "  FAIL ${names[$i]} (Debian)"; exit 1; }
done

[ -n "$PHONE" ] || { echo "no handset captures given; desktop half only"
                     exit 0; }

python3 - "$OUT" "$PHONE" <<'PY'
from PIL import Image, ImageDraw
import sys, os
out, phone = sys.argv[1], sys.argv[2]
names = ['keypad', 'contacts', 'quality', 'activity', 'settings']
for i, n in enumerate(names):
    p = os.path.join(phone, 'emu-%d.png' % i)
    if not os.path.exists(p):
        print('  skip %s: no handset capture' % n)
        continue
    d = Image.open(os.path.join(out, 'desk-%d.png' % i)).convert('RGB')
    e = Image.open(p).convert('RGB')
    # The status bar and the navigation bar are the system's, not
    # ours, so they come off before the two are compared.
    e = e.crop((0, 96, e.width, e.height - 128))
    e = e.resize((int(e.width * d.height / e.height), d.height),
                 Image.LANCZOS)
    gap = 16
    c = Image.new('RGB', (d.width + e.width + gap * 3, d.height + 40),
                  (24, 24, 28))
    c.paste(d, (gap, 32))
    c.paste(e, (gap * 2 + d.width, 32))
    dr = ImageDraw.Draw(c)
    dr.text((gap, 10), '%s: Debian' % n, fill=(200, 200, 210))
    dr.text((gap * 2 + d.width, 10), '%s: Android' % n,
            fill=(200, 200, 210))
    c.save(os.path.join(out, 'side-%s.png' % n))
    print('  wrote side-%s.png' % n)
PY
