#!/usr/bin/env python3
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later
"""Does a scaled buffer land in the frame it described, whole and downsampled?

usage: check_scale.py SHOT PPM_X PPM_Y FRAME_W FRAME_H

The screenshot holds one window: a `--buffer-scale 2 --quadrants` client that asked for a
FRAME_W x FRAME_H frame at (PPM_X, PPM_Y) with a buffer twice that in each direction. Three
things are asserted, and each is a different way of getting HiDPI wrong:

  1. THE WINDOW IS THE FRAME IT ASKED FOR. A compositor that ignored `set_buffer_scale` would
     draw the buffer at its own size and the window would be twice as wide and twice as tall
     as the frame — the reported overrun.
  2. THE WHOLE BUFFER REACHED THE SCREEN. Four colours in the buffer must appear as four
     quarters of the frame; a compositor that sampled only the buffer's top-left quarter would
     show one colour, and one that took the first pixel of every block would show the wrong
     four (the block's corner, not its average).
  3. NOTHING SPILLED. Outside the frame the compositor's own background is untouched.
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


def pixel(pixels, width, x, y):
    base = (y * width + x) * 3
    return pixels[base], pixels[base + 1], pixels[base + 2]


def near(got, want, tolerance=36):
    return all(abs(a - b) <= tolerance for a, b in zip(got, want))


def main():
    if len(sys.argv) != 6:
        print("usage: check_scale.py SHOT X Y W H", file=sys.stderr)
        return 2
    shot, x, y, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
    width, height, pixels = read_ppm(shot)
    failures = 0

    def check(ok, what):
        nonlocal failures
        print(("  ok    " if ok else "  FAIL  ") + what)
        if not ok:
            failures += 1

    # (1) THE QUADRANTS, sampled at the centre of each quarter of the FRAME.
    quadrants = [
        ("top-left is RED", w // 4, h // 4, (0xFF, 0x00, 0x00)),
        ("top-right is GREEN", 3 * w // 4, h // 4, (0x00, 0xFF, 0x00)),
        ("bottom-left is BLUE", w // 4, 3 * h // 4, (0x00, 0x00, 0xFF)),
        ("bottom-right is WHITE", 3 * w // 4, 3 * h // 4, (0xFF, 0xFF, 0xFF)),
    ]
    for label, dx, dy, want in quadrants:
        got = pixel(pixels, width, x + dx, y + dy)
        check(near(got, want), "%s at (%d,%d) — got %s" % (label, x + dx, y + dy, got))

    # (2) THE WHOLE BUFFER REACHED THE SCREEN: no quarter may be uniform with its neighbour.
    seen = {pixel(pixels, width, x + dx, y + dy) for _, dx, dy, _ in quadrants}
    check(len(seen) == 4, "all four quarters of the buffer are on the screen (saw %d colour(s))" % len(seen))

    # (3) NOTHING SPILLED: a pixel just outside the frame is not a quadrant colour, and the far
    #     corner of the output is untouched — a window drawn at twice its frame would paint over it.
    outside = pixel(pixels, width, x + w + 8, y + h // 2) if x + w + 8 < width else pixel(pixels, width, x - 8, y + h // 2)
    check(not near(outside, (0xFF, 0x00, 0x00)) and not near(outside, (0x00, 0xFF, 0x00))
          and not near(outside, (0x00, 0x00, 0xFF)),
          "nothing bled past the frame: %s outside it" % (outside,))
    far = pixel(pixels, width, min(width - 1, x + 2 * w + 40), min(height - 1, y + 2 * h + 40))
    check(not near(far, (0xFF, 0xFF, 0xFF)) or (x + 2 * w + 40 >= width),
          "and the output beyond the window is the compositor's, not the client's: %s" % (far,))

    print("check_scale: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
