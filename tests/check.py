# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later
# Portions of this file may derive from GNUstep; see THIRD-PARTY.md.

#!/usr/bin/env python3
"""Read the window server's own screenshot and assert what the two test clients put there.

THE ASSERTION IS AN ORDER, NOT A PICTURE. Two clients draw flat colours and OVERLAP, and THE NORMAL-LEVEL ONE IS
ON TOP OF THE DESKTOP-LEVEL ONE — while being the one that was created FIRST. That disagreement is deliberate:
with the levels agreeing with the creation order this would pass whether the server ordered by level or not, which
is the difference between a test and a decoration. AND THE LOWER ONE IS AT A DESKTOP LEVEL, so what is shown is
that a window below every other level stays below a normal window created before it.

AND A THIRD CLIENT IS NOT A WINDOW AT ALL: it is a STATUS ITEM, which the server PLACES — the client asks for a
frame at (0,0) as well, ON PURPOSE, so every assertion about where it ended up is the server's answer winning.
"""
import sys
from PIL import Image

BACKGROUND = (168, 222, 217)      # aws_output.c's AWS_BACKGROUND — aqua BY REQUEST, not the mockup's amber
MENU_BAR = (235, 235, 235)       # server.h's AWS_MENU_BAR_COLOUR — Argentum's bar, drawn by the SERVER
MENU_BAR_HEIGHT = 24
SYSTEM_MENU_MID = 17             # a column THROUGH the glyph: server.h places it at 10..25 (AWS_SYSTEM_MENU_LEFT_MARGIN)
GLYPH = (0, 0, 0)                # server.h's AWS_SYSTEM_MENU_GLYPH_COLOUR — Argentum's ink
TOP_AT = (100, 80)               # the first client's frame, asked for with agora_window.set_frame
BOTTOM_AT = (150, 110)           # the second's — created LATER and at the DESKTOP level
WINDOW = (320, 200)


def main():
    if len(sys.argv) < 7:
        print("usage: check.py SHOT.ppm TOP-RRGGBB BOTTOM-RRGGBB WxH STATUS-RRGGBB STATUS-LENGTH")
        return 2
    path, top, bottom, size = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), sys.argv[4]
    ctop = ((top >> 16) & 0xFF, (top >> 8) & 0xFF, top & 0xFF)
    cbot = ((bottom >> 16) & 0xFF, (bottom >> 8) & 0xFF, bottom & 0xFF)
    w, h = (int(v) for v in size.lower().split("x"))
    status = int(sys.argv[5], 16)
    status_len = int(sys.argv[6])
    cstatus = ((status >> 16) & 0xFF, (status >> 8) & 0xFF, status & 0xFF)

    im = Image.open(path).convert("RGB")
    px = im.load()
    # a point inside both windows: the second client's frame is offset into the first's
    over = (BOTTOM_AT[0] + 10, BOTTOM_AT[1] + 10)
    # the strip the server draws, over the full width and no taller than the reserve
    strip_row = MENU_BAR_HEIGHT - 1

    checks = [
        (f"the output is {im.size[0]}x{im.size[1]}", im.size == (1280, 720)),
        # **INSIDE THE CORNER AND NOT AT IT, BECAUSE A WINDOW'S CORNER IS ROUNDED AND NOT BY THE CLIENT.** A window
        # travels opaque (`XRGB8888`), so the client cannot remove the square of its own pixels around its corner;
        # the compositor clips the shape. Eight points in is clear of that curve and two points in is what the
        # curve cuts, which is why the probe moved and why the two checks below exist.
        (f"the high-level client is at its frame ({TOP_AT[0]},{TOP_AT[1]})",
         px[TOP_AT[0] + 8, TOP_AT[1] + 8] == ctop),
        (f"the low-level client is at ITS frame ({BOTTOM_AT[0]},{BOTTOM_AT[1]})",
         px[BOTTOM_AT[0] + w - 8, BOTTOM_AT[1] + h - 8] == cbot),
        ("and its CORNER is cut, so the shape is the compositor's and not the client's",
         px[TOP_AT[0], TOP_AT[1]] != ctop and px[BOTTOM_AT[0] + w - 1, BOTTOM_AT[1] + h - 1] != cbot),
        (f"THE NORMAL WINDOW IS ON TOP OF THE DESKTOP WINDOW at {over}, and the desktop window was created LAST",
         px[over[0], over[1]] == ctop),
        ("and it is not the later window showing through", px[over[0], over[1]] != cbot),
        # **THE ORIGIN IS NO LONGER THE SERVER'S TO LEAVE ALONE**: the strip the server draws now owns the top
        # rows, so this samples where a cascade would have put a WINDOW and the strip cannot reach — and the
        # check is stronger for it, since a window at (0,0) would once have been visible and is now covered.
        # **AND THE SAMPLE CLEARS THE BAR'S OWN SHADOW, WHICH IS NEW.** The strip now casts a semitransparent
        # shadow onto the wallpaper below it, so the first rows under the bar are no longer the plain background —
        # asserting they were was asserting that the bar has no shadow, which is the opposite of what it now does.
        # The depth is `AWS_SHADOW_DEPTH` in output.c and this starts one row past it.
        ("nothing is at the origin, where a cascade would have put them",
         px[2, MENU_BAR_HEIGHT + 12] == BACKGROUND and px[42, MENU_BAR_HEIGHT + 12] == BACKGROUND),
        # **THE STRIP IS THE SERVER'S; WHAT IS IN IT IS A CLIENT'S.** So the strip is sampled only to the LEFT of
        # the item — a sample taken through the item would be asserting the server's pixels were never covered,
        # which is the opposite of what a status area is.
        (f"the SERVER's menu bar strip is the top {MENU_BAR_HEIGHT} rows, left of the item",
         all(px[x, strip_row] == MENU_BAR for x in range(0, 1280 - status_len, 32))),
        # **AND THE ITEM, PLACED BY THE SERVER AT THE RIGHT EDGE.** It asked for a frame at (0,0) as well, so
        # each of these is the server's placement winning rather than a client that never asked to be anywhere.
        (f"the status item is at the RIGHT EDGE of the strip, {status_len} wide",
         px[1280 - 2, MENU_BAR_HEIGHT // 2] == cstatus and
         px[1280 - status_len + 2, MENU_BAR_HEIGHT // 2] == cstatus),
        # **AND WHAT IS NOT COVERED HERE IS THE OTHER CASE OF THE SAME RULE.** A surface inside the bar casts no
        # shadow either — that is what stopped the application's own menu titles smudging the strip — and the check
        # above reaches it only through the ITEM, which both the old rule ("is it a status item?") and the new one
        # ("does it lie inside the bar?") catch, so it cannot tell them apart. The case that CAN is an ordinary
        # window given the bar's rows, and this client has only one window: it is named as owed rather than faked
        # with a check that would pass either way.
        ("and it does not reach past its own left edge, where the strip shows",
         px[1280 - status_len - 3, MENU_BAR_HEIGHT // 2] == MENU_BAR),
        # Its buffer is 320x200 — BIGGER than the 64x24 box the server put it in — so this is the clipping, not
        # the buffer, that keeps a status item inside the menu bar.
        (f"and it is the menu bar's height and no taller — row {MENU_BAR_HEIGHT} below it is not the item",
         px[1280 - 2, MENU_BAR_HEIGHT + 4] != cstatus),
        # **THE SYSTEM MENU, WHICH IS THE SERVER'S OWN AND THE ONLY THING HERE IT DRAWS RATHER THAN FILLS.** Its
        # glyph is the hamburger — three bars — so three bars with two gaps between them is the whole assertion,
        # and a solid block, two bars or four would each fail it.
        ("the SERVER's System menu is at the left of the strip, and is THREE bars",
         px[SYSTEM_MENU_MID, 5] == GLYPH and px[SYSTEM_MENU_MID, 11] == GLYPH and
         px[SYSTEM_MENU_MID, 17] == GLYPH),
        ("and the gaps between the bars are the bar, so it is not a block",
         px[SYSTEM_MENU_MID, 8] == MENU_BAR and px[SYSTEM_MENU_MID, 14] == MENU_BAR),
        (f"and it ends exactly there — row {MENU_BAR_HEIGHT} is not strip",
         px[640, MENU_BAR_HEIGHT + 1] != MENU_BAR),
        ("and the rest is the compositor's own background", px[5, 715] == BACKGROUND and
         px[1275, 715] == BACKGROUND),
    ]

    failed = 0
    for name, ok in checks:
        print(f"  [{'ok' if ok else 'FAIL'}] {name}")
        failed += 0 if ok else 1
    print(f"check: {len(checks)} checks, {failed} failed" + (" -- all checks passed" if not failed else ""))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
