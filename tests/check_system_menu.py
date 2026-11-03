#!/usr/bin/env python3
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

"""Read the window server's screenshot and measure the System menu's highlight chip.

THE CLAIM IS A RECTANGLE AROUND THE MARK, WITH ONE NUMBER FOR ITS SIDE AIR AND ANOTHER FOR ITS HEAD AND FOOT. An
application's menu title lights up with a chip that has air either side of its text; the System menu's square is a bar
title, so it wears the same shape around its mark: `PADDING` each side, `INSET` above and below. The mark itself is
NOT centred in the square — it sits at `LEFT_MARGIN` so the air before it equals the air after the square — so a chip
that filled the square would be flush against the mark's right edge, which is the bug this test exists for.

Every number here is a copy of `server.h`'s, and that is deliberate: the test states the rule and fails when the code
stops following it. `CHECK_SCALE` is the only thing that varies, and the arithmetic is rounded the way the server rounds
a device edge — `round(logical * scale)`.
"""
import sys
from PIL import Image

LEFT_MARGIN = 10        # server.h: AWS_SYSTEM_MENU_LEFT_MARGIN
GLYPH = 16              # server.h: AWS_SYSTEM_MENU_GLYPH_WIDTH
GLYPH_TOP = 4           # server.h: AWS_SYSTEM_MENU_GLYPH_TOP, ((24 - 16) / 2)
CHIP_PADDING = 6        # server.h: AWS_SYSTEM_MENU_CHIP_PADDING
CHIP_INSET = 2          # server.h: AWS_SYSTEM_MENU_CHIP_INSET
ACCENT = (164, 154, 255)    # server.h: AWS_SYSTEM_MENU_CHIP_COLOUR
BAR_HEIGHT = 24         # server.h: AWS_MENU_BAR_HEIGHT


def edge(logical, scale):
    return int(logical * scale + 0.5)


def main():
    if len(sys.argv) < 3:
        print("usage: check_system_menu.py SHOT.ppm SCALE")
        return 2
    path, scale = sys.argv[1], float(sys.argv[2])

    im = Image.open(path).convert("RGB")
    px = im.load()

    # THE MARK'S BOX AND THE CHIP'S, both in device pixels, from the same constants the server uses.
    mark_x = edge(LEFT_MARGIN, scale)
    mark_y = edge(GLYPH_TOP, scale)
    mark = edge(GLYPH, scale)
    chip_x = mark_x - edge(CHIP_PADDING, scale)
    chip_y = mark_y - edge(CHIP_INSET, scale)
    chip_w = mark + 2 * edge(CHIP_PADDING, scale)
    chip_h = mark + 2 * edge(CHIP_INSET, scale)

    accent = [(x, y) for y in range(0, edge(BAR_HEIGHT, scale) + 8) for x in range(0, chip_x + chip_w + 8)
              if px[(x, y)] == ACCENT]
    ink = [(x, y) for y in range(0, edge(BAR_HEIGHT, scale)) for x in range(0, chip_x + chip_w + 8)
           if sum(px[(x, y)]) < 200]

    checks = []
    if not accent:
        checks.append(("the square wears the accent colour while its menu is up", False))
    else:
        ax0, ax1 = min(p[0] for p in accent), max(p[0] for p in accent)
        ay0, ay1 = min(p[1] for p in accent), max(p[1] for p in accent)

        checks += [
            ("the chip's rectangle is the one the server's constants name",
             (ax0, ay0, ax1 - ax0 + 1, ay1 - ay0 + 1) == (chip_x, chip_y, chip_w, chip_h)),
            ("it is a RECTANGLE — wider than it is tall",
             (ax1 - ax0) > (ay1 - ay0)),
            ("the mark's ink is inside it", bool(ink) and min(p[0] for p in ink) >= ax0
             and max(p[0] for p in ink) <= ax1),
            ("the air either side of the mark is EQUAL",
             mark_x - ax0 == ax1 - (mark_x + mark - 1)),
            ("and the air above and below it is equal",
             mark_y - ay0 == ay1 - (mark_y + mark - 1)),
        ]
        print(f"  chip {ax1 - ax0 + 1}x{ay1 - ay0 + 1} at ({ax0},{ay0}); the mark's box is "
              f"{mark_x}..{mark_x + mark - 1} x {mark_y}..{mark_y + mark - 1}")
        print(f"  air: left {mark_x - ax0}, right {ax1 - (mark_x + mark - 1)}, "
              f"top {mark_y - ay0}, bottom {ay1 - (mark_y + mark - 1)}")

    failures = 0
    for what, ok in checks:
        print(f"{'  ok  ' if ok else '  FAIL'} {what}")
        failures += 0 if ok else 1
    print(f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
