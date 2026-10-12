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

    checks = [
        (f"the output is {im.size[0]}x{im.size[1]}", im.size == (1280, 720)),
        (f"the high-level client is at its frame ({TOP_AT[0]},{TOP_AT[1]})",
         px[TOP_AT[0] + 2, TOP_AT[1] + 2] == ctop),
        (f"the low-level client is at ITS frame ({BOTTOM_AT[0]},{BOTTOM_AT[1]})",
         px[BOTTOM_AT[0] + w - 2, BOTTOM_AT[1] + h - 2] == cbot),
        (f"THE HIGHER LEVEL IS ON TOP at {over}, and it was created FIRST",
         px[over[0], over[1]] == ctop),
        ("and it is not the later window showing through", px[over[0], over[1]] != cbot),
        ("nothing is at the origin, where a cascade would have put them",
         px[2, 2] == BACKGROUND and px[42, 42] == BACKGROUND),
        ("and the rest is the compositor's own background", px[5, 5] == BACKGROUND and
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
