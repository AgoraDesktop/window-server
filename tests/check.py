#!/usr/bin/env python3
"""Read the window server's own screenshot and assert what the two test clients put there.

THE ASSERTION IS AN ORDER, NOT A PICTURE. Two clients draw flat colours and OVERLAP, and THE ONE THAT ASKED FOR
THE HIGHER LEVEL IS ON TOP — while being the one that was created FIRST. That disagreement is deliberate: with
the levels agreeing with the creation order this would pass whether the server ordered by level or not, which is
the difference between a test and a decoration.
"""
import sys
from PIL import Image

BACKGROUND = (48, 51, 56)        # aws_output.c's AWS_BACKGROUND
MENU_BAR = (216, 216, 216)       # server.h's AWS_MENU_BAR_COLOUR — the strip the SERVER draws
MENU_BAR_HEIGHT = 24
TOP_AT = (100, 80)               # the first client's frame, asked for with agora_window.set_frame
BOTTOM_AT = (150, 110)           # the second's — created LATER and at a LOWER level
WINDOW = (320, 200)


def main():
    if len(sys.argv) < 5:
        print("usage: check.py SHOT.ppm TOP-RRGGBB BOTTOM-RRGGBB WxH")
        return 2
    path, top, bottom, size = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), sys.argv[4]
    ctop = ((top >> 16) & 0xFF, (top >> 8) & 0xFF, top & 0xFF)
    cbot = ((bottom >> 16) & 0xFF, (bottom >> 8) & 0xFF, bottom & 0xFF)
    w, h = (int(v) for v in size.lower().split("x"))

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
        (f"the SERVER's menu bar strip is the top {MENU_BAR_HEIGHT} rows",
         all(px[x, strip_row] == MENU_BAR for x in range(0, 1280, 32))),
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
