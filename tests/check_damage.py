#!/usr/bin/env python3
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

"""Read the window server's screenshot and assert WHAT A PARTIAL FRAME PAINTED — by the poison, not by the picture.

THE PROBLEM THIS SOLVES IS THAT A DAMAGE BUG IS INVISIBLE IN A CLIENT THAT REPAINTED EVERYTHING. A compositor that
recomposites the whole window and one that honours the client's rectangles draw the SAME picture, because the buffer it
reads is the whole window either way. So the test client (`tests/client.c`, `--damage strip`) writes a colour no other
test uses — magenta — into every pixel OUTSIDE the strip it damages. Then:

  * the strip's rows show the strip's colour,
  * the rest of the window still shows the picture it had (the base colour), and
  * NO MAGENTA IS ON THE SCREEN. If it is, the compositor read pixels the commit did not claim, which is exactly the
    over-paint this work exists to stop.

`whole` is the other direction: a commit that attaches a buffer and damages NOTHING means the WHOLE BUFFER (Wayland's
own rule), so the third picture must replace everything — poison and strip both.
`move` is the full-redraw trigger: `set_frame` moves the window and damages nothing, so the frame has to repaint BOTH
the window's new place and the ground it left, which is the pixels the damage rectangles could never have named.
"""
import sys
from PIL import Image

BACKGROUND = (168, 222, 217)      # server.h's AWS_BACKGROUND, the same aqua check.py asserts
MENU_BAR = (235, 235, 235)       # server.h's AWS_MENU_BAR_COLOUR, drawn by the SERVER
POISON = (255, 0, 255)           # tests/client.c's POISON_COLOUR — must never reach the glass
MENU_BAR_HEIGHT = 24
OUTPUT = (1280, 720)


def rgb(value):
    number = int(value, 16)
    return ((number >> 16) & 0xFF, (number >> 8) & 0xFF, number & 0xFF)


def main():
    if len(sys.argv) < 8:
        print("usage: check_damage.py SHOT MODE BASE-RRGGBB STRIP-RRGGBB THIRD-RRGGBB WxH X,Y[ X,Y])")
        return 2
    path, mode = sys.argv[1], sys.argv[2]
    base = rgb(sys.argv[3])
    strip = rgb(sys.argv[4])
    third = rgb(sys.argv[5])
    width, height = (int(part) for part in sys.argv[6].lower().split("x"))
    old = tuple(int(part) for part in sys.argv[7].split(","))
    new = tuple(int(part) for part in sys.argv[8].split(",")) if len(sys.argv) > 8 else old
    at = new if mode == "move" else old

    im = Image.open(path).convert("RGB")
    px = im.load()
    # THE STRIP IS IN BUFFER PIXELS AND THE BUFFER IS PLACED 1:1, so the rows are the window's own offset plus these.
    STRIP_Y0, STRIP_Y1 = 40, 60
    checks = []

    def add(name, ok):
        checks.append((name, ok))

    def inside(x, y):
        return at[0] <= x < at[0] + width and at[1] <= y < at[1] + height

    add(f"the output is {OUTPUT[0]}x{OUTPUT[1]}", im.size == OUTPUT)
    # **THE BAR IS STILL THERE AND FULLY PAINTED**, which a partial frame that clipped only the window could break by
    # narrowing the clear to a box that misses rows the bar belongs to.
    add("the SERVER's menu bar is still the top strip, whole", px[640, 10] == MENU_BAR and px[1000, 10] == MENU_BAR)
    add("and it is exactly the bar's height", px[640, MENU_BAR_HEIGHT + 1] != MENU_BAR)

    if mode == "strip":
        # **THE PARTIAL COMMIT: the strip arrived and the rest of the window is the colour it already had.**
        add("the window is at its frame, in the base colour above the strip",
            px[at[0] + 10, at[1] + 10] == base and px[at[0] + width - 10, at[1] + STRIP_Y0 - 10] == base)
        add("the strip the commit named is the strip colour",
            px[at[0] + 10, at[1] + STRIP_Y0 + 5] == strip and
            px[at[0] + width - 10, at[1] + STRIP_Y1 - 5] == strip)
        add("and below it the window is the base colour again", px[at[0] + 10, at[1] + STRIP_Y1 + 5] == base)
        add("the row above the strip is NOT the strip (the box is the client's rows, not the window's)",
            px[at[0] + 10, at[1] + STRIP_Y0 - 1] == base)
        add("and the row below is not either", px[at[0] + 10, at[1] + STRIP_Y1] == base)
    elif mode == "whole":
        # **THE WHOLE-BUFFER RULE: a commit with a buffer and NO damage means every pixel.** The third picture covers the
        # poison and the strip, so the window is one colour from corner to corner.
        add("a commit that damaged NOTHING repainted the WHOLE window",
            all(px[x, y] == third
                for (x, y) in [(at[0] + 10, at[1] + 10), (at[0] + 10, at[1] + STRIP_Y0 + 5),
                               (at[0] + width - 10, at[1] + height - 10), (at[0] + width // 2, at[1] + height // 2)]))
        add("and the strip's colour is gone with it", px[at[0] + 10, at[1] + STRIP_Y0 + 5] != strip)
    elif mode == "move":
        # **THE MOVE IS A FULL REDRAW: the new place has the window, and the old place has the desktop back.**
        add("the window moved to its new frame, with the picture it had",
            px[at[0] + 10, at[1] + 10] == base and px[at[0] + 10, at[1] + STRIP_Y0 + 5] == strip)
        # **A FEW ROWS IN FROM THE OLD CORNER, BECAUSE THE WINDOW'S SHAPE IS ROUNDED AND ITS SHADOW FALLS OUTSIDE IT.**
        # What is asserted is that nothing of the window is left where it was — the shadow belongs to the new place.
        add("and NOTHING of it is left where it was",
            px[old[0] + 10, old[1] + 10] == BACKGROUND and
            px[old[0] + 10, old[1] + STRIP_Y0 + 5] == BACKGROUND and
            px[old[0] + width - 10, old[1] + height - 10] == BACKGROUND)
        add("nor is the base colour anywhere in the vacated rectangle",
            not any(px[x, y] == base for x in range(old[0] + 8, old[0] + width - 8, 16)
                    for y in range(old[1] + 8, old[1] + height - 8, 16)))
    else:
        print(f"unknown mode: {mode}")
        return 2

    # **AND THE ONE ASSERTION THAT MAKES THE OTHERS MEAN ANYTHING.** The poison is written into every pixel outside the
    # strip and no commit ever names it, so a single magenta pixel is the compositor having read the whole buffer on a
    # frame the client said was a strip.
    poison_pixels = 0
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            if px[x, y] == POISON:
                poison_pixels += 1
    add(f"NO POISON IS ON THE GLASS ({poison_pixels} magenta pixel(s))", poison_pixels == 0)

    add("the desktop around the window is the compositor's own background",
        px[1000, 600] == BACKGROUND and px[5, 715] == BACKGROUND)

    failed = 0
    for name, ok in checks:
        print(f"  [{'ok' if ok else 'FAIL'}] {name}")
        failed += 0 if ok else 1
    print(f"check: {len(checks)} checks, {failed} failed" + (" -- all checks passed" if not failed else ""))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
