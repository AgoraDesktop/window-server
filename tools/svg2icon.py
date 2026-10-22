#!/usr/bin/env python3
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later
# Portions of this file may derive from GNUstep; see THIRD-PARTY.md.

"""Convert a monochrome SVG path into the System menu's `.icon` format.

THE INPUT IS A SIMPLE ICONS SVG (one `<path>`, a 24-unit viewBox, no fill-rule attribute). The output is the
line-oriented format `gen-icon-table.py` reads: an id, a grid, a fill rule, and absolute M/L/Q/C/Z commands in the
SVG's own units. The generator normalizes the grid to [0,1]; keeping the source units here makes the file readable
and makes a regeneration diff mean something.

SVG path commands that this tool accepts: M/m, L/l, H/h, V/v, C/c, S/s, Q/q, T/t, A/a, Z/z. Arcs are converted to
cubic Béziers with the endpoint-to-center parameterization from the SVG specification, split at 90 degrees. The
result is still a vector, not a polyline.
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from pathlib import Path

PARAM_COUNTS = {
    "M": 2,
    "L": 2,
    "H": 1,
    "V": 1,
    "C": 6,
    "S": 4,
    "Q": 4,
    "T": 2,
    "A": 7,
    "Z": 0,
}


class SvgError(Exception):
    pass


def _elliptical_point(cx, cy, rx, ry, cos_phi, sin_phi, theta):
    return (
        cx + rx * math.cos(theta) * cos_phi - ry * math.sin(theta) * sin_phi,
        cy + rx * math.cos(theta) * sin_phi + ry * math.sin(theta) * cos_phi,
    )


def _elliptical_derivative(rx, ry, cos_phi, sin_phi, theta):
    return (
        -rx * math.sin(theta) * cos_phi - ry * math.cos(theta) * sin_phi,
        -rx * math.sin(theta) * sin_phi + ry * math.cos(theta) * cos_phi,
    )


NUMBER_RE = re.compile(r"[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")


class PathScanner:
    """A character scanner, because SVG arc flags may be written without separators.

    `A 3 3 0 01-1-.5` is legal SVG: the `0` and `1` are the large-arc and sweep flags, not the number `01`.
    A number-token splitter cannot see that, so the parser reads numbers and flags from the text itself.
    """

    def __init__(self, text: str):
        self.text = text
        self.index = 0

    def _skip_separators(self):
        while self.index < len(self.text) and (self.text[self.index].isspace() or self.text[self.index] == ","):
            self.index += 1

    def at_end(self) -> bool:
        self._skip_separators()
        return self.index >= len(self.text)

    def peek_command(self) -> str | None:
        self._skip_separators()
        if self.index < len(self.text) and self.text[self.index].isalpha():
            return self.text[self.index]
        return None

    def take_command(self) -> str:
        command = self.peek_command()
        if command is None:
            raise SvgError("expected an SVG command")
        self.index += 1
        return command

    def read_number(self) -> float:
        self._skip_separators()
        match = NUMBER_RE.match(self.text, self.index)
        if match is None:
            raise SvgError(f"expected a number at offset {self.index}")
        self.index = match.end()
        return float(match.group(0))

    def read_flag(self) -> bool:
        self._skip_separators()
        if self.index < len(self.text) and self.text[self.index] in "01":
            value = self.text[self.index] == "1"
            self.index += 1
            return value
        raise SvgError(f"expected an arc flag at offset {self.index}")


class PathParser:
    def __init__(self):
        self.commands: list[tuple] = []
        self.current = (0.0, 0.0)
        self.start = (0.0, 0.0)
        self.previous_cubic_control: tuple[float, float] | None = None
        self.previous_quad_control: tuple[float, float] | None = None

    def move_to(self, x, y):
        self.commands.append(("M", x, y))
        self.current = (x, y)
        self.start = (x, y)
        self.previous_cubic_control = None
        self.previous_quad_control = None

    def line_to(self, x, y):
        self.commands.append(("L", x, y))
        self.current = (x, y)
        self.previous_cubic_control = None
        self.previous_quad_control = None

    def quad_to(self, cx, cy, x, y):
        self.commands.append(("Q", cx, cy, x, y))
        self.current = (x, y)
        self.previous_quad_control = (cx, cy)
        self.previous_cubic_control = None

    def cubic_to(self, c1x, c1y, c2x, c2y, x, y):
        self.commands.append(("C", c1x, c1y, c2x, c2y, x, y))
        self.current = (x, y)
        self.previous_cubic_control = (c2x, c2y)
        self.previous_quad_control = None

    def close(self):
        self.commands.append(("Z",))
        self.current = self.start
        self.previous_cubic_control = None
        self.previous_quad_control = None

    def arc_to(self, rx, ry, rotation, large_arc, sweep, x, y):
        x1, y1 = self.current
        if rx == 0.0 or ry == 0.0 or (x1 == x and y1 == y):
            self.line_to(x, y)
            return

        phi = math.radians(rotation % 360.0)
        cos_phi = math.cos(phi)
        sin_phi = math.sin(phi)
        dx2 = (x1 - x) / 2.0
        dy2 = (y1 - y) / 2.0
        x1p = cos_phi * dx2 + sin_phi * dy2
        y1p = -sin_phi * dx2 + cos_phi * dy2

        rx = abs(rx)
        ry = abs(ry)
        scale = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
        if scale > 1.0:
            factor = math.sqrt(scale)
            rx *= factor
            ry *= factor

        numerator = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
        denominator = rx * rx * y1p * y1p + ry * ry * x1p * x1p
        sign = 1.0 if (large_arc != sweep) else -1.0
        coefficient = sign * math.sqrt(max(0.0, numerator / denominator)) if denominator != 0.0 else 0.0

        cxp = coefficient * (rx * y1p / ry)
        cyp = coefficient * (-ry * x1p / rx)
        cx = cos_phi * cxp - sin_phi * cyp + (x1 + x) / 2.0
        cy = sin_phi * cxp + cos_phi * cyp + (y1 + y) / 2.0

        ux = (x1p - cxp) / rx
        uy = (y1p - cyp) / ry
        vx = (-x1p - cxp) / rx
        vy = (-y1p - cyp) / ry
        theta1 = math.atan2(uy, ux)
        delta = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        if not sweep and delta > 0.0:
            delta -= 2.0 * math.pi
        if sweep and delta < 0.0:
            delta += 2.0 * math.pi

        segments = max(1, int(math.ceil(abs(delta) / (math.pi / 2.0))))
        step = delta / segments
        for index in range(segments):
            t1 = theta1 + index * step
            t2 = t1 + step
            alpha = (4.0 / 3.0) * math.tan(step / 4.0)

            p1 = _elliptical_point(cx, cy, rx, ry, cos_phi, sin_phi, t1)
            p2 = _elliptical_point(cx, cy, rx, ry, cos_phi, sin_phi, t2)
            d1 = _elliptical_derivative(rx, ry, cos_phi, sin_phi, t1)
            d2 = _elliptical_derivative(rx, ry, cos_phi, sin_phi, t2)

            self.cubic_to(
                p1[0] + alpha * d1[0],
                p1[1] + alpha * d1[1],
                p2[0] - alpha * d2[0],
                p2[1] - alpha * d2[1],
                p2[0],
                p2[1],
            )
        self.current = (x, y)

    def parse(self, path_data: str):
        scanner = PathScanner(path_data)
        command = None
        last_command = None

        while not scanner.at_end():
            next_command = scanner.peek_command()
            if next_command is not None:
                command = scanner.take_command()
                if command in ("Z", "z"):
                    self.close()
                    last_command = command
                    continue
                if command.upper() not in PARAM_COUNTS:
                    raise SvgError(f"unsupported SVG command {command!r}")
            elif command is None:
                raise SvgError("path data starts with a number, not a command")

            upper = command.upper()
            relative = command.islower()
            cx, cy = self.current

            if upper == "A":
                rx = scanner.read_number()
                ry = scanner.read_number()
                rotation = scanner.read_number()
                large_arc = scanner.read_flag()
                sweep = scanner.read_flag()
                x = scanner.read_number()
                y = scanner.read_number()
                if relative:
                    x += cx
                    y += cy
                self.arc_to(rx, ry, rotation, large_arc, sweep, x, y)
            else:
                count = PARAM_COUNTS[upper]
                params = [scanner.read_number() for _ in range(count)]

                if upper == "M":
                    x = cx + params[0] if relative else params[0]
                    y = cy + params[1] if relative else params[1]
                    self.move_to(x, y)
                    # Further coordinate pairs after M are implicit L (or l) commands.
                    command = "l" if relative else "L"
                elif upper == "L":
                    x = cx + params[0] if relative else params[0]
                    y = cy + params[1] if relative else params[1]
                    self.line_to(x, y)
                elif upper == "H":
                    x = cx + params[0] if relative else params[0]
                    self.line_to(x, cy)
                elif upper == "V":
                    y = cy + params[0] if relative else params[0]
                    self.line_to(cx, y)
                elif upper == "C":
                    c1x = cx + params[0] if relative else params[0]
                    c1y = cy + params[1] if relative else params[1]
                    c2x = cx + params[2] if relative else params[2]
                    c2y = cy + params[3] if relative else params[3]
                    x = cx + params[4] if relative else params[4]
                    y = cy + params[5] if relative else params[5]
                    self.cubic_to(c1x, c1y, c2x, c2y, x, y)
                elif upper == "S":
                    if last_command is not None and last_command.upper() in ("C", "S") and self.previous_cubic_control:
                        c1x = 2.0 * cx - self.previous_cubic_control[0]
                        c1y = 2.0 * cy - self.previous_cubic_control[1]
                    else:
                        c1x, c1y = cx, cy
                    c2x = cx + params[0] if relative else params[0]
                    c2y = cy + params[1] if relative else params[1]
                    x = cx + params[2] if relative else params[2]
                    y = cy + params[3] if relative else params[3]
                    self.cubic_to(c1x, c1y, c2x, c2y, x, y)
                elif upper == "Q":
                    qx = cx + params[0] if relative else params[0]
                    qy = cy + params[1] if relative else params[1]
                    x = cx + params[2] if relative else params[2]
                    y = cy + params[3] if relative else params[3]
                    self.quad_to(qx, qy, x, y)
                elif upper == "T":
                    if last_command is not None and last_command.upper() in ("Q", "T") and self.previous_quad_control:
                        qx = 2.0 * cx - self.previous_quad_control[0]
                        qy = 2.0 * cy - self.previous_quad_control[1]
                    else:
                        qx, qy = cx, cy
                    x = cx + params[0] if relative else params[0]
                    y = cy + params[1] if relative else params[1]
                    self.quad_to(qx, qy, x, y)
                else:
                    raise SvgError(f"unsupported SVG command {command!r}")

            last_command = command

        return self.commands

def extract_path_data(svg_text: str) -> tuple[list[str], str, float]:
    view_box = re.search(r'viewBox="([^"]+)"', svg_text)
    if view_box is None:
        raise SvgError("the SVG has no viewBox")
    values = [float(value) for value in re.findall(r"[-+]?(?:\d*\.\d+|\d+\.?)", view_box.group(1))]
    if len(values) != 4:
        raise SvgError("the viewBox is not four numbers")
    if values[0] != 0.0 or values[1] != 0.0 or values[2] != values[3]:
        raise SvgError("only a square viewBox starting at 0,0 is supported")
    paths = re.findall(r"<path\b[^>]*\bd=\"([^\"]*)\"", svg_text)
    if not paths:
        raise SvgError("the SVG has no <path d=\"...\">")
    fill_rules = re.findall(r"fill-rule=\"([^\"]+)\"", svg_text)
    fill = "evenodd" if any(rule == "evenodd" for rule in fill_rules) else "nonzero"
    return paths, fill, values[2]


def format_number(value: float) -> str:
    text = f"{value:.6f}".rstrip("0").rstrip(".")
    if text in ("", "-0"):
        return "0"
    return text


def emit_icon(commands, icon_id: str, grid: float, fill: str, source: str) -> str:
    lines = [
        f"# Generated from {source}",
        "# by tools/svg2icon.py; edit the source SVG and regenerate, not this file.",
        f"id {icon_id}",
        f"grid {format_number(grid)}",
        f"fill {fill}",
    ]
    for command in commands:
        if command[0] == "M":
            lines.append(f"M {format_number(command[1])} {format_number(command[2])}")
        elif command[0] == "L":
            lines.append(f"L {format_number(command[1])} {format_number(command[2])}")
        elif command[0] == "Q":
            lines.append(
                "Q "
                + " ".join(format_number(value) for value in command[1:])
            )
        elif command[0] == "C":
            lines.append(
                "C "
                + " ".join(format_number(value) for value in command[1:])
            )
        elif command[0] == "Z":
            lines.append("Z")
        else:
            raise SvgError(f"unexpected output command {command[0]!r}")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="source SVG")
    parser.add_argument("output", type=Path, help="destination .icon")
    parser.add_argument("--id", required=True, help="icon id, e.g. debian")
    parser.add_argument("--source", default="", help="source URL recorded in the header")
    args = parser.parse_args()

    try:
        svg_text = args.input.read_text(encoding="utf-8")
        paths, fill, grid = extract_path_data(svg_text)
        commands = []
        for path_data in paths:
            commands.extend(PathParser().parse(path_data))
        args.output.write_text(
            emit_icon(commands, args.id, grid, fill, args.source or args.input.name),
            encoding="utf-8",
        )
        print(f"{args.output}: {len(commands)} command(s), grid {format_number(grid)}, fill {fill}")
        return 0
    except (OSError, SvgError) as error:
        print(f"svg2icon: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
