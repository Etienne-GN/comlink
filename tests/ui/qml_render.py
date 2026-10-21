#!/usr/bin/env python3
"""What the QML window actually drew.

The smoke test says a pane loaded without a QML warning. That is not
the same as a pane that rendered: a pane whose content failed to lay
out is an empty rectangle, and an empty rectangle raises nothing.

Deliberately not a pixel baseline. A baseline breaks on a font
update, gets regenerated, and stops being evidence. What is asserted
here is structural and survives a font: the window is the size it
should be, the background does not cover the whole of it, and each
band that should carry content carries more than one colour.
"""
import os
import re
import sys
import zlib
import struct


def read_png(path):
    """Enough of a PNG reader to get RGB rows. Qt writes 8-bit
    RGBA or RGB, non-interlaced, which is all this handles -- and if
    that ever changes, this says so rather than guessing."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s is not a PNG" % path)
    pos = 8
    width = height = depth = colour = None
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(
                ">IIBBBBB", body)
            if depth != 8 or colour not in (2, 6) or interlace != 0:
                raise ValueError("%s: unsupported PNG (depth %d colour %d "
                                 "interlace %d)"
                                 % (path, depth, colour, interlace))
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + length

    channels = 3 if colour == 2 else 4
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    rows = []
    prev = bytearray(stride)
    at = 0
    for _ in range(height):
        filt = raw[at]
        line = bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if filt == 1:
                line[i] = (line[i] + a) & 0xFF
            elif filt == 2:
                line[i] = (line[i] + b) & 0xFF
            elif filt == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        rows.append([tuple(line[x * channels:x * channels + 3])
                     for x in range(width)])
        prev = line
    return width, height, rows


def pane_band(logpath):
    """The pane's y and height, as the window printed them.

    The last line, not the first: the window reports on every change
    and the first is the geometry before the layout settles."""
    found = (None, None)
    try:
        for line in open(logpath):
            if "PROBE quick pane " in line:
                bits = line.split("PROBE quick pane ", 1)[1].split()
                y = int(bits[0].split("=")[1])
                h = int(bits[1].split("=")[1])
                found = (y, y + h)
    except OSError:
        pass
    return found


def colours_in(rows, top, bottom):
    seen = {}
    for y in range(max(0, top), min(len(rows), bottom)):
        for px in rows[y]:
            seen[px] = seen.get(px, 0) + 1
    return seen


def main():
    if len(sys.argv) < 2:
        print("usage: qml_render.py <directory of probe PNGs>")
        return 1
    here = sys.argv[1]
    fails = []
    checked = 0

    for name in sorted(os.listdir(here)):
        if not name.endswith(".png"):
            continue
        path = os.path.join(here, name)
        w, h, rows = read_png(path)
        checked += 1

        # A probe shot at another size says so in its name:
        # overlay-x-270x560.png, from COMLINK_QUICK_SHOT_SIZE.
        sized = re.search(r"-(\d+)x(\d+)\.png$", name)
        want = (int(sized.group(1)), int(sized.group(2))) if sized \
            else (380, 660)
        if (w, h) != want:
            fails.append("%s: %dx%d, expected %dx%d"
                         % (name, w, h, want[0], want[1]))
            continue

        whole = colours_in(rows, 0, h)
        total = w * h
        dominant = max(whole.values())

        # An overlay covers the window, so the main layout's bands are
        # not there to check -- and an empty SIP trace is legitimately
        # almost all background. What still has to hold is that
        # something was drawn, and that the overlay's own title row
        # carries its heading and its buttons.
        overlay = name.startswith("overlay-")
        limit = 0.99 if overlay else 0.92
        if dominant > total * limit:
            fails.append("%s: one colour covers %.0f%% of the window"
                         % (name, 100.0 * dominant / total))

        # The title along the top and, for a pane, the strip along the
        # bottom and the pane's own body. Any one of them flat is a
        # layout that collapsed -- which is exactly what a pane that
        # failed to lay out looks like.
        bands = [("title", 0, 34)]
        if not overlay:
            # Where the pane is, as the window itself reported it. A
            # band guessed here drifts the moment a row is added
            # above it, and then this passes on a pane that drew
            # nothing.
            top, bottom = pane_band(os.path.join(here, name[:-4] + ".log"))
            if top is None:
                fails.append("%s: the window did not report the pane's "
                             "geometry" % name)
            else:
                bands += [("strip", h - 34, h), ("body", top, bottom)]
        for label, top, bottom in bands:
            seen = colours_in(rows, top, bottom)
            if len(seen) < 3:
                fails.append("%s: the %s band has %d colour(s)"
                             % (name, label, len(seen)))

    for f in fails:
        print("FAIL " + f)
    print("%d image(s) checked, %d problem(s)" % (checked, len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
