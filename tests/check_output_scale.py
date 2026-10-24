#!/usr/bin/env python3
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later
"""Does `--scale 2` make an ordinary client TWICE THE SIZE rather than leaving it tiny?

usage: check_output_scale.py SHOT LOGICAL_W LOGICAL_H BAR_H [SCALE]

The screenshot is a 2x output: LOGICAL_W x LOGICAL_H logical pixels in a framebuffer twice that in each
direction. Two clients are on it — an ordinary one (1x buffer) and one that renders at 2x itself — and
what is asserted is the difference between them:

  1. THE FRAMEBUFFER IS 2x THE LOGICAL SCREEN. A compositor that ignored the scale would photograph
     1x the size, and every window on it would be half the size it should be — the reported symptom.
  2. THE MENU BAR IS STILL THE SAME SIZE TO A PERSON. `AWS_MENU_BAR_HEIGHT` is logical, so it must
     cover 2x as many device rows; a bar still 24 rows tall in a 2x frame is the strip being shrunk.
  3. AN ORDINARY CLIENT IS PLACED AT 2x ITS LOGICAL RECT — that is the whole point of the slice.
  4. A 2x CLIENT IS PLACED AT THE SAME DEVICE RECT and its four buffer quarters land one-per-quarter,
     which is the crisp case: 2x buffer on a 2x output is a 1:1 copy, not a blur and not a downscale.
"""
import sys


def read_ppm(path):
    with open(path, "rb") as handle:
        data = handle.read()
    fields, offset = [], 0
    while len(fields) < 4:
        while offset < len(data) and data[offset:offset + 1].isspace():
            offset += 1
        if data[offset:offset + 1] == b"#":
            while offset < len(data) and data[offset:offset + 1] != b"\n":
                offset += 1
            continue
        start = offset
        while offset < len(data) and not data[offset:offset + 1].isspace():
            offset += 1
        fields.append(data[start:offset])
    offset += 1
    width, height = int(fields[1]), int(fields[2])
    return width, height, data[offset:offset + width * height * 3]


def near(got, want, tolerance=36):
    return all(abs(a - b) <= tolerance for a, b in zip(got, want))


def main():
    if len(sys.argv) not in (5, 6):
        print("usage: check_output_scale.py SHOT LOGICAL_W LOGICAL_H BAR_H [SCALE]", file=sys.stderr)
        return 2
    shot, lw, lh, bar_h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    scale = float(sys.argv[5]) if len(sys.argv) == 6 else 2.0

    # THE SAME ROUNDING THE COMPOSITOR USES: an absolute logical edge times the scale, rounded.
    def dev(v):
        return int(v * scale + 0.5)
    width, height, pixels = read_ppm(shot)
    failures = 0

    def at(x, y):
        base = (y * width + x) * 3
        return tuple(pixels[base:base + 3])

    def check(ok, what):
        nonlocal failures
        print(("  ok    " if ok else "  FAIL  ") + what)
        if not ok:
            failures += 1

    # (1) the framebuffer.
    check(width == dev(lw) and height == dev(lh),
          "the framebuffer is %dx%d device pixels for a %dx%d logical screen at scale %g — got %dx%d"
          % (dev(lw), dev(lh), lw, lh, scale, width, height))

    # (2) the bar still covers the same logical height, in however many device rows that is.
    bar_bottom = dev(bar_h)
    bar = at(lw, bar_bottom - 2)     # a device column in the middle, just above the bar's device bottom
    below = at(lw, bar_bottom + 4)   # a few device rows below it: the desktop
    check(not near(bar, below), "the menu bar covers %d device rows, the logical %d times %g"
          % (bar_bottom, bar_h, scale))

    # (3) the ordinary 1x client, placed at logical (40,40) 80x40.
    check(near(at(dev(40) + dev(60), dev(40) + dev(20)), (0xFF, 0x00, 0x00)),
          "an ORDINARY client is drawn at its logical rect times %g — red at device (%d,%d)"
          % (scale, dev(40) + dev(60), dev(40) + dev(20)))
    check(not near(at(dev(40) - 6, dev(40) + dev(20)), (0xFF, 0x00, 0x00)),
          "and not a few device pixels to the left of it")

    # (4) the 2x client at logical (200,40) 80x40 -> the same device rect, four quarters.
    # THE FOUR QUARTER CENTRES, in LOGICAL points: the window is at (200,40) and 80x40, so they are 20 in from each
    # side. Converted to device once, by the same rounding the compositor uses — the previous version of this
    # arithmetic mixed the two spaces and sampled the window's centre four times.
    quad = [("top-left RED", dev(220), dev(50), (0xFF, 0x00, 0x00)),
            ("top-right GREEN", dev(260), dev(50), (0x00, 0xFF, 0x00)),
            ("bottom-left BLUE", dev(220), dev(70), (0x00, 0x00, 0xFF)),
            ("bottom-right WHITE", dev(260), dev(70), (0xFF, 0xFF, 0xFF))]
    for label, x, y, want in quad:
        got = at(x, y)
        check(near(got, want), "a 2x client is composited 1:1 at scale %g: %s at (%d,%d) — got %s"
              % (scale, label, x, y, got))

    print("check_output_scale: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
