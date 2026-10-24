#!/usr/bin/env python3
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later
"""Does `--scale 2` make an ordinary client TWICE THE SIZE rather than leaving it tiny?

usage: check_output_scale.py SHOT LOGICAL_W LOGICAL_H BAR_H

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
    if len(sys.argv) != 5:
        print("usage: check_output_scale.py SHOT LOGICAL_W LOGICAL_H BAR_H", file=sys.stderr)
        return 2
    shot, lw, lh, bar_h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
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
    check(width == lw * 2 and height == lh * 2,
          "the framebuffer is %dx%d device pixels for a %dx%d logical screen — got %dx%d"
          % (lw * 2, lh * 2, lw, lh, width, height))

    # (2) the bar still covers the same logical height, which is twice as many device rows.
    bar = at(lw, bar_h)          # a device column in the middle, one row above the bar's device bottom
    below = at(lw, bar_h * 2 + 4)  # four device rows below it: the desktop
    check(not near(bar, below), "the menu bar covers %d device rows, twice the logical %d" % (bar_h * 2, bar_h))

    # (3) the ordinary 1x client, placed at logical (40,40) 80x40 -> device (80,80) 160x80.
    check(near(at(40 * 2 + 80, 40 * 2 + 40), (0xFF, 0x00, 0x00)),
          "an ORDINARY client is drawn at twice its logical rect — red at device (160,120)")
    check(not near(at(40 * 2 - 6, 40 * 2 + 40), (0xFF, 0x00, 0x00)),
          "and not one device pixel to the left of it")

    # (4) the 2x client at logical (200,40) 80x40 -> the same device rect, four quarters.
    quad = [("top-left RED", 200 * 2 + 40, 40 * 2 + 20, (0xFF, 0x00, 0x00)),
            ("top-right GREEN", 200 * 2 + 120, 40 * 2 + 20, (0x00, 0xFF, 0x00)),
            ("bottom-left BLUE", 200 * 2 + 40, 40 * 2 + 60, (0x00, 0x00, 0xFF)),
            ("bottom-right WHITE", 200 * 2 + 120, 40 * 2 + 60, (0xFF, 0xFF, 0xFF))]
    for label, x, y, want in quad:
        got = at(x, y)
        check(near(got, want), "a 2x client is 1:1 on a 2x output: %s at (%d,%d) — got %s" % (label, x, y, got))

    print("check_output_scale: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
