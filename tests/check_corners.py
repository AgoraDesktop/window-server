#!/usr/bin/env python3
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

"""Read the window server's screenshot and say which of two surfaces at the same y owns its corner.

THE SAME y IS THE POINT. A window at the top of the work area and a menu hanging from the bar both begin at
AWS_MENU_BAR_HEIGHT, so the corner is a question about KIND, not about geometry: the window is rounded (the corner
pixel is the desktop showing through, darkened by the shadow) and the menu is square (the corner pixel is the menu's
own colour). A compositor that read the y alone squares both, which is the bug this test exists for.
"""
import sys
from PIL import Image

WINDOW_AT = (100, 24, 200, 150)     # x, y, w, h — run-corners.sh's first client
MENU_AT = (360, 24, 200, 150)       # the second, at NSPopUpMenuWindowLevel
RADIUS = 6                          # server.h's AWS_SHADOW_RADIUS
TOLERANCE = 24                      # the shadow darkens the corner, so "not this colour" is the claim, not a value


def main():
    if len(sys.argv) < 5:
        print("usage: check_corners.py SHOT.ppm WINDOW-RRGGBB MENU-RRGGBB Y")
        return 2
    path = sys.argv[1]
    window = int(sys.argv[2], 16)
    menu = int(sys.argv[3], 16)
    y = int(sys.argv[4])
    cwindow = ((window >> 16) & 0xFF, (window >> 8) & 0xFF, window & 0xFF)
    cmenu = ((menu >> 16) & 0xFF, (menu >> 8) & 0xFF, menu & 0xFF)

    im = Image.open(path).convert("RGB")
    px = im.load()
    (wx, _, ww, wh) = WINDOW_AT
    (mx, _, mw, _) = MENU_AT

    def near(a, b):
        return all(abs(x - y) <= TOLERANCE for x, y in zip(a, b))

    # A PIXEL INSIDE THE ARC IS THE WINDOW'S; the corner pixel itself is outside the curve, so what is there is the
    # desktop (and the shadow over it) — anything but the window's colour.
    checks = [
        (f"the output is 640x400", im.size == (640, 400)),
        (f"the window's corner at ({wx},{y}) is NOT the window's colour (rounded)",
         not near(px[(wx, y)], cwindow)),
        (f"the window's bottom corner at ({wx},{y + wh - 1}) is NOT the window's colour (rounded)",
         not near(px[(wx, y + wh - 1)], cwindow)),
        (f"the window's inside at ({wx + RADIUS},{y + RADIUS}) IS the window's colour",
         near(px[(wx + RADIUS, y + RADIUS)], cwindow)),
        (f"the menu's corner at ({mx},{y}) IS the menu's colour (square, it hangs from the bar)",
         near(px[(mx, y)], cmenu)),
        (f"the menu's inside at ({mx + RADIUS},{y + RADIUS}) IS the menu's colour",
         near(px[(mx + RADIUS, y + RADIUS)], cmenu)),
        (f"the menu's width is its own to the last column ({mx + mw - 1})",
         near(px[(mx + mw - 1, y + RADIUS)], cmenu)),
    ]

    failures = 0
    for what, ok in checks:
        print(f"{'  ok  ' if ok else '  FAIL'} {what}")
        failures += 0 if ok else 1
    print(f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
