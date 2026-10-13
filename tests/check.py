#!/usr/bin/env python3
"""Read the window server's own screenshot and assert what the two test clients put there.

THE ASSERTION IS AN ORDER, NOT A PICTURE. Two clients draw flat colours and OVERLAP, and THE ONE THAT ASKED FOR
THE HIGHER LEVEL IS ON TOP — while being the one that was created FIRST. That disagreement is deliberate: with
the levels agreeing with the creation order this would pass whether the server ordered by level or not, which is
the difference between a test and a decoration.

AND A THIRD CLIENT IS NOT A WINDOW AT ALL: it is a STATUS ITEM, which the server PLACES — the client asks for a
frame at (0,0) as well, ON PURPOSE, so every assertion about where it ended up is the server's answer winning.
"""
import sys
from PIL import Image

BACKGROUND = (42, 29, 13)        # aws_output.c's AWS_BACKGROUND — Argentum's wallpaper, flattened
MENU_BAR = (235, 235, 235)       # server.h's AWS_MENU_BAR_COLOUR — Argentum's bar, drawn by the SERVER
MENU_BAR_HEIGHT = 24
SYSTEM_MENU_MID = 13             # the middle of the glyph in the System menu's square (server.h)
GLYPH = (0, 0, 0)                # server.h's AWS_SYSTEM_MENU_GLYPH_COLOUR — Argentum's ink
TOP_AT = (100, 80)               # the first client's frame, asked for with agora_window.set_frame
BOTTOM_AT = (150, 110)           # the second's — created LATER and at a LOWER level
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
        (f"the high-level client is at its frame ({TOP_AT[0]},{TOP_AT[1]})",
         px[TOP_AT[0] + 2, TOP_AT[1] + 2] == ctop),
        (f"the low-level client is at ITS frame ({BOTTOM_AT[0]},{BOTTOM_AT[1]})",
         px[BOTTOM_AT[0] + w - 2, BOTTOM_AT[1] + h - 2] == cbot),
        (f"THE HIGHER LEVEL IS ON TOP at {over}, and it was created FIRST",
         px[over[0], over[1]] == ctop),
        ("and it is not the later window showing through", px[over[0], over[1]] != cbot),
        # **THE ORIGIN IS NO LONGER THE SERVER'S TO LEAVE ALONE**: the strip the server draws now owns the top
        # rows, so this samples where a cascade would have put a WINDOW and the strip cannot reach — and the
        # check is stronger for it, since a window at (0,0) would once have been visible and is now covered.
        ("nothing is at the origin, where a cascade would have put them",
         px[2, MENU_BAR_HEIGHT + 2] == BACKGROUND and px[42, MENU_BAR_HEIGHT + 2] == BACKGROUND),
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
