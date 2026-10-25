#!/usr/bin/env python3
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

"""Check one forced System menu mark in the server's own screenshot.

THE UNIT TEST ALREADY KNOWS WHAT EVERY COVERAGE GRID CONTAINS. This is the integration half: the server was told
`--system-menu-icon NAME`, a real client committed a frame, and the screenshot must show that mark's ink inside the
mark's box — the 16-point box at (10,4) scaled by the output's scale — and nothing else in the bar. Ink-pixel count
is the cheap cross-check that the right mark was selected at the right resolution; the coverage hash is the unit
test's job.

**A SCALED RUN IS THE POINT OF THE SECOND SET OF NUMBERS.** At 2x the box is 32 device pixels and the mark must be a
32×32 raster: the hamburger's bars then land on exact device rows, so 576 pixels are solid black and NOT ONE is
partially covered. Magnifying the 16×16 grid into the same box would have inked 768 and left a 2×2 block of every
cell — visible as a two-pixel step along every bar, which is exactly the blocky mark that sat beside crisp menubar
text.
"""
import sys
from PIL import Image

BAR = (235, 235, 235)
INK = (0, 0, 0)
BOX_X, BOX_Y, BOX_SIZE = 10, 4, 16  # the mark's box in POINTS, from server.h
BAR_POINTS = 24

# THE MARKS AT 1x. These are the ink counts of the 16x16 golden coverage grid in tests/system_icon.c, drawn over the
# bar's face: a pixel counts as ink when it is not exactly the bar's colour.
EXPECTED_INK = {
    "alpinelinux": 180,
    "archlinux": 99,
    "debian": 91,
    "fedora": 223,
    "freebsd": 204,
    "linuxmint": 175,
    "netbsd": 102,
    "openbsd": 137,
    "opensuse": 155,
    "redhat": 142,
    "ubuntu": 137,
    "hamburger": 192,
}

# AND AT 2x, WHICH ONLY THE HAMBURGER IS RUN AT: the mark that is all straight edges, so a 32x32 raster of it has no
# partial pixel at all and a magnified one cannot be mistaken for it.
EXPECTED_INK_2X = {"hamburger": 576}


def main():
    if len(sys.argv) not in (3, 4):
        print("usage: check_icon.py SHOT.ppm ICON [SCALE]")
        return 2
    path, icon = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) == 4 else 1
    expected = (EXPECTED_INK if scale == 1 else EXPECTED_INK_2X)
    if icon not in expected:
        print(f"check_icon: no {scale}x expectation for {icon!r}")
        return 2

    x0, y0, size = BOX_X * scale, BOX_Y * scale, BOX_SIZE * scale
    bar_rows = BAR_POINTS * scale
    im = Image.open(path).convert("RGB")
    px = im.load()
    ink = 0
    partial = 0
    coloured = 0
    outside = 0

    for y in range(y0, y0 + size):
        for x in range(x0, x0 + size):
            pixel = px[x, y]
            if pixel != BAR:
                ink += 1
                if pixel != INK:
                    partial += 1
                if not (pixel[0] == pixel[1] == pixel[2]):
                    coloured += 1
    for y in range(0, bar_rows):
        for x in range(0, im.size[0]):
            if x0 <= x < x0 + size and y0 <= y < y0 + size:
                continue
            if px[x, y] != BAR:
                outside += 1

    checks = [
        (f"the output is {im.size[0]}x{im.size[1]}",
         im.size == (1280 * scale, 720 * scale)),
        (f"the {scale}x {icon} mark inks {expected[icon]} pixel(s) in its box",
         ink == expected[icon]),
        ("the mark is monochrome", coloured == 0),
        ("nothing else is painted in the bar", outside == 0),
    ]
    if scale == 1:
        checks.insert(2, ("the mark is anti-aliased rather than all-or-nothing", partial > 0))
    else:
        # A 32x32 raster of three straight bars has nothing to anti-alias; a magnified grid has nothing else.
        checks.insert(2, ("and none of that ink is partially covered", partial == 0))

    failed = 0
    for name, ok in checks:
        print(f"  [{'ok' if ok else 'FAIL'}] {name}")
        failed += 0 if ok else 1
    print(f"check_icon({icon} at {scale}x): {len(checks)} checks, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
