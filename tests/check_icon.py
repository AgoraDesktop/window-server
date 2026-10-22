# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later
# Portions of this file may derive from GNUstep; see THIRD-PARTY.md.

#!/usr/bin/env python3
"""Check one forced System menu mark in the server's own screenshot.

THE UNIT TEST ALREADY KNOWS WHAT EVERY COVERAGE GRID CONTAINS. This is the integration half: the server was told
`--system-menu-icon NAME`, a real client committed a frame, and the screenshot must show that mark's ink inside the
16x16 box at (10,4) — and nothing else in the bar. Ink-pixel count is the cheap cross-check that the right mark was
selected; the coverage hash is the unit test's job.
"""
import sys
from PIL import Image

BAR = (235, 235, 235)
INK = (0, 0, 0)
X0, Y0, SIZE = 10, 4, 16

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


def main():
    if len(sys.argv) != 3:
        print("usage: check_icon.py SHOT.ppm ICON")
        return 2
    path, icon = sys.argv[1], sys.argv[2]
    if icon not in EXPECTED_INK:
        print(f"check_icon: unknown icon {icon!r}")
        return 2

    im = Image.open(path).convert("RGB")
    px = im.load()
    ink = 0
    partial = 0
    coloured = 0
    outside = 0

    for y in range(Y0, Y0 + SIZE):
        for x in range(X0, X0 + SIZE):
            pixel = px[x, y]
            if pixel != BAR:
                ink += 1
                if pixel != INK:
                    partial += 1
                if not (pixel[0] == pixel[1] == pixel[2]):
                    coloured += 1
    for y in range(0, 24):
        for x in range(0, im.size[0]):
            if X0 <= x < X0 + SIZE and Y0 <= y < Y0 + SIZE:
                continue
            if px[x, y] != BAR:
                outside += 1

    checks = [
        (f"the output is {im.size[0]}x{im.size[1]}", im.size == (1280, 720)),
        (f"the {icon} mark inks {EXPECTED_INK[icon]} pixel(s) in the box", ink == EXPECTED_INK[icon]),
        ("the mark is anti-aliased rather than all-or-nothing", partial > 0),
        ("the mark is monochrome", coloured == 0),
        ("nothing else is painted in the bar", outside == 0),
    ]

    failed = 0
    for name, ok in checks:
        print(f"  [{'ok' if ok else 'FAIL'}] {name}")
        failed += 0 if ok else 1
    print(f"check_icon({icon}): {len(checks)} checks, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
