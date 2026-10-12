#!/usr/bin/env python3
"""Read the window server's own screenshot and assert what the test client put there.

The assertion is a PLACE rather than a picture: the client drew one flat colour, so what is checked is that the
colour is where the compositor's cascade says the first window goes, that it has the size the client asked for,
and that everything else is the compositor's own background.
"""
import sys
from PIL import Image

BACKGROUND = (48, 51, 56)      # aws_output.c's AWS_BACKGROUND
CASCADE = (40, 40)             # surface.c's first-window placement


def main():
    if len(sys.argv) < 4:
        print("usage: check.py SHOT.ppm EXPECTED-RRGGBB WxH")
        return 2
    path, colour, size = sys.argv[1], int(sys.argv[2], 16), sys.argv[3]
    want = ((colour >> 16) & 0xFF, (colour >> 8) & 0xFF, colour & 0xFF)
    w, h = (int(v) for v in size.lower().split("x"))

    im = Image.open(path).convert("RGB")
    px = im.load()
    checks = []

    checks.append((f"the output is {im.size[0]}x{im.size[1]}", im.size == (1280, 720)))
    checks.append((f"the client's colour is at the cascade ({CASCADE[0]},{CASCADE[1]})",
                   px[CASCADE[0] + 2, CASCADE[1] + 2] == want))
    checks.append((f"the window is {w}x{h}",
                   px[CASCADE[0] + w - 2, CASCADE[1] + h - 2] == want))
    checks.append(("nothing of the client is outside it", px[CASCADE[0] + w + 5, CASCADE[1] + 5] == BACKGROUND))
    checks.append(("and the rest is the compositor's own background", px[5, 5] == BACKGROUND and
                   px[1275, 715] == BACKGROUND))

    failed = 0
    for name, ok in checks:
        print(f"  [{'ok' if ok else 'FAIL'}] {name}")
        failed += 0 if ok else 1
    print(f"check: {len(checks)} checks, {failed} failed" + (" -- all checks passed" if not failed else ""))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
