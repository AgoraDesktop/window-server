#!/usr/bin/env python3
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

"""Generate the System menu icon tables and vector data from the TSVs and `.icon` files.

THE DATA IS THE SOURCE. `assets/system-menu-icons/{distros,aliases,families}.tsv` decide which mark an OS gets;
the `.icon` files decide what each mark looks like. This script emits three checked-in files:

  src/systemicon-table.c   the id/alias/family tables and the icon objects
  src/systemicon-data.c    the vector command arrays
  src/systemicon-data.h    the path-array declarations the table refers to

A normal build needs no Python because all three are committed. `tests/run-icon-table-check.sh` regenerates them
into a temporary directory and compares, so a data edit cannot silently disagree with the linked table.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "assets" / "system-menu-icons"
DEFAULT_OUTPUT = ROOT / "src"

ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
SYMBOL_PATTERN = re.compile(r"[^a-zA-Z0-9_]")


class DataError(Exception):
    pass


def read_tsv(path: Path, columns: int) -> list[list[str]]:
    rows: list[list[str]] = []
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        # Columns are aligned with runs of tabs in the checked-in data; a run is one separator.
        parts = [part.strip() for part in re.split(r"\t+", raw.strip())]
        if len(parts) != columns:
            raise DataError(
                f"{path.name}:{line_number}: expected {columns} tab-separated columns, got {len(parts)}"
            )
        if any(part == "" for part in parts):
            raise DataError(f"{path.name}:{line_number}: empty column")
        rows.append(parts)
    return rows


def check_id(path: Path, value: str) -> str:
    if not ID_PATTERN.match(value):
        raise DataError(f"{path.name}: invalid id {value!r}")
    return value


def c_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def c_float(value: float) -> str:
    text = f"{value:.6f}"
    if text == "-0.000000":
        text = "0.000000"
    return text + "f"


def symbol_for(icon_id: str) -> str:
    return "aws_icon_" + SYMBOL_PATTERN.sub("_", icon_id) + "_paths"


def parse_icon(path: Path) -> dict:
    icon_id = None
    grid = None
    fill = "nonzero"
    commands: list[tuple] = []

    def number(parts: list[str], count: int) -> tuple[float, ...]:
        if len(parts) != count + 1:
            raise DataError(f"{path.name}: {parts[0]} needs {count} number(s)")
        try:
            return tuple(float(value) for value in parts[1:])
        except ValueError as error:
            raise DataError(f"{path.name}: {error}") from error

    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        key = parts[0]
        if key == "id":
            if len(parts) != 2:
                raise DataError(f"{path.name}:{line_number}: id needs one value")
            icon_id = check_id(path, parts[1])
        elif key == "grid":
            values = number(parts, 1)
            grid = values[0]
        elif key == "fill":
            if len(parts) != 2 or parts[1] not in ("evenodd", "nonzero"):
                raise DataError(f"{path.name}:{line_number}: fill must be evenodd or nonzero")
            fill = parts[1]
        elif key == "M":
            commands.append(("M",) + number(parts, 2))
        elif key == "L":
            commands.append(("L",) + number(parts, 2))
        elif key == "Q":
            commands.append(("Q",) + number(parts, 4))
        elif key == "C":
            commands.append(("C",) + number(parts, 6))
        elif key == "Z":
            if len(parts) != 1:
                raise DataError(f"{path.name}:{line_number}: Z takes no arguments")
            commands.append(("Z",))
        else:
            raise DataError(f"{path.name}:{line_number}: unknown line {key!r}")

    if icon_id is None:
        raise DataError(f"{path.name}: no id")
    if icon_id != path.stem:
        raise DataError(f"{path.name}: id {icon_id!r} does not match the filename")
    if grid is None or grid <= 0.0:
        raise DataError(f"{path.name}: grid must be positive")
    if not commands or commands[0][0] != "M":
        raise DataError(f"{path.name}: the first command must be M")
    return {"id": icon_id, "grid": grid, "fill": fill, "commands": commands}


def emit_data_h(icons: dict[str, dict], ordered: list[str]) -> list[str]:
    lines = [
        "/*",
        " * systemicon-data.h — GENERATED, DO NOT EDIT.",
        " *",
        " * Generated by tools/gen-icon-table.py from the .icon files.",
        " * Edit the .icon files, then run: python3 tools/gen-icon-table.py",
        " *",
        " * Copyright © 2026 Kyle J. Cardoza.",
        " * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza",
        " * SPDX-License-Identifier: GPL-3.0-or-later",
        " */",
        "",
        "#ifndef AGORA_SYSTEMICON_DATA_H",
        "#define AGORA_SYSTEMICON_DATA_H",
        "",
        '#include "systemicon.h"',
        "",
    ]
    for icon_id in ordered:
        if icon_id in icons:
            lines.append(f"extern const struct aws_icon_path {symbol_for(icon_id)}[1];")
    lines.extend(["", "#endif"])
    return lines


def emit_data_c(icons: dict[str, dict], ordered: list[str]) -> list[str]:
    lines = [
        "/*",
        " * systemicon-data.c — GENERATED, DO NOT EDIT.",
        " *",
        " * Generated by tools/gen-icon-table.py from the .icon files.",
        " * Edit the .icon files, then run: python3 tools/gen-icon-table.py",
        " *",
        " * The generated scaffolding is GPL-3.0-or-later; the path data it carries is CC0 1.0.",
        " * See assets/system-menu-icons/PROVENANCE.md for the per-mark source and trademark notes.",
        " *",
        " * Copyright © 2026 Kyle J. Cardoza.",
        " * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza",
        " * SPDX-License-Identifier: GPL-3.0-or-later AND CC0-1.0",
        " */",
        "",
        '#include "systemicon-data.h"',
        "",
        "#include <stddef.h>",
        "",
    ]
    for icon_id in ordered:
        icon = icons.get(icon_id)
        if icon is None:
            continue
        symbol = symbol_for(icon_id)
        commands = icon["commands"]
        grid = icon["grid"]
        lines.append(f"static const struct aws_icon_command {symbol}_commands[] = {{")
        for command in commands:
            op = command[0]
            if op == "Z":
                lines.append("\t{ .op = AWS_ICON_CLOSE },")
                continue
            points = [c_float(value / grid) for value in command[1:]]
            if op == "M":
                lines.append(f"\t{{ .op = AWS_ICON_MOVE, .point = {{ {{ {points[0]}, {points[1]} }} }} }},")
            elif op == "L":
                lines.append(f"\t{{ .op = AWS_ICON_LINE, .point = {{ {{ {points[0]}, {points[1]} }} }} }},")
            elif op == "Q":
                lines.append(
                    f"\t{{ .op = AWS_ICON_QUAD, .point = {{ "
                    f"{{ {points[0]}, {points[1]} }}, {{ {points[2]}, {points[3]} }} }} }},"
                )
            elif op == "C":
                lines.append(
                    f"\t{{ .op = AWS_ICON_CUBIC, .point = {{ "
                    f"{{ {points[0]}, {points[1]} }}, {{ {points[2]}, {points[3]} }}, "
                    f"{{ {points[4]}, {points[5]} }} }} }},"
                )
        lines.append("};")
        lines.append(f"const struct aws_icon_path {symbol}[1] = {{")
        lines.append(f"\t{{ {symbol}_commands, sizeof({symbol}_commands) / sizeof({symbol}_commands[0]) }},")
        lines.append("};")
        lines.append("")
    return lines


def emit_table_c(
    icons: dict[str, dict],
    ordered: list[str],
    distros: dict[str, tuple[str, str | None]],
    aliases: dict[str, str],
    families: dict[str, str],
) -> list[str]:
    lines = [
        "/*",
        " * systemicon-table.c — GENERATED, DO NOT EDIT.",
        " *",
        " * Generated by tools/gen-icon-table.py from the system-menu icon TSVs and .icon files.",
        " * Edit the data, then run: python3 tools/gen-icon-table.py",
        " *",
        " * Copyright © 2026 Kyle J. Cardoza.",
        " * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza",
        " * SPDX-License-Identifier: GPL-3.0-or-later",
        " */",
        "",
        '#include "systemicon-table.h"',
        '#include "systemicon-data.h"',
        "",
        "#include <stddef.h>",
        "",
        "const struct aws_distro_entry aws_distro_entries[] = {",
    ]
    for distro_id, (family, icon) in sorted(distros.items()):
        icon_literal = "NULL" if icon is None else c_string(icon)
        lines.append(f"\t{{ {c_string(distro_id)}, {c_string(family)}, {icon_literal} }},")
    lines.append("};")
    lines.append("const size_t aws_distro_entry_count =")
    lines.append("\tsizeof(aws_distro_entries) / sizeof(aws_distro_entries[0]);")
    lines.append("")
    lines.append("const struct aws_alias_entry aws_alias_entries[] = {")
    for alias, icon in sorted(aliases.items()):
        lines.append(f"\t{{ {c_string(alias)}, {c_string(icon)} }},")
    lines.append("};")
    lines.append("const size_t aws_alias_entry_count =")
    lines.append("\tsizeof(aws_alias_entries) / sizeof(aws_alias_entries[0]);")
    lines.append("")
    lines.append("const struct aws_family_entry aws_family_entries[] = {")
    for family, icon in sorted(families.items()):
        lines.append(f"\t{{ {c_string(family)}, {c_string(icon)} }},")
    lines.append("};")
    lines.append("const size_t aws_family_entry_count =")
    lines.append("\tsizeof(aws_family_entries) / sizeof(aws_family_entries[0]);")
    lines.append("")
    lines.append(
        "/* THE MARK OBJECTS. THE COVERAGE GRIDS ARE FILLED LAZILY: THE LOGICAL ONE BY "
        "`aws_system_icon_coverage` AND ONE PER"
    )
    lines.append(" * DEVICE SIZE BY `aws_system_icon_coverage_at`. */")
    lines.append("struct aws_system_icon aws_icon_objects[] = {")
    for icon_id in ordered:
        icon = icons.get(icon_id)
        if icon is None:
            lines.append(
                f"\t{{ {c_string(icon_id)}, NULL, 0, AWS_ICON_FILL_EVEN_ODD, {{ 0 }}, 0 }},"
            )
            continue
        symbol = symbol_for(icon_id)
        fill = "AWS_ICON_FILL_EVEN_ODD" if icon["fill"] == "evenodd" else "AWS_ICON_FILL_NONZERO"
        lines.append("\t{")
        lines.append(f"\t\t{c_string(icon_id)},")
        lines.append(f"\t\t{symbol},")
        lines.append(f"\t\tsizeof({symbol}) / sizeof({symbol}[0]),")
        lines.append(f"\t\t{fill},")
        lines.append("\t\t{ 0 },")
        lines.append("\t\t0")
        lines.append("\t},")
    lines.append("};")
    lines.append("const size_t aws_icon_object_count =")
    lines.append("\tsizeof(aws_icon_objects) / sizeof(aws_icon_objects[0]);")
    lines.append("")
    return lines


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT,
        help="directory for the three generated files (default: src)",
    )
    args = parser.parse_args(argv)

    try:
        family_rows = read_tsv(DATA / "families.tsv", 2)
        distro_rows = read_tsv(DATA / "distros.tsv", 3)
        alias_rows = read_tsv(DATA / "aliases.tsv", 2)

        families: dict[str, str] = {}
        for family, icon in family_rows:
            check_id(DATA / "families.tsv", family)
            check_id(DATA / "families.tsv", icon)
            if family in families:
                raise DataError(f"families.tsv: duplicate family {family!r}")
            families[family] = icon

        distros: dict[str, tuple[str, str | None]] = {}
        for distro_id, family, icon in distro_rows:
            check_id(DATA / "distros.tsv", distro_id)
            if family not in families:
                raise DataError(f"distros.tsv: {distro_id!r} names unknown family {family!r}")
            if icon != "-":
                check_id(DATA / "distros.tsv", icon)
            if distro_id in distros:
                raise DataError(f"distros.tsv: duplicate id {distro_id!r}")
            distros[distro_id] = (family, None if icon == "-" else icon)

        aliases: dict[str, str] = {}
        for alias, icon in alias_rows:
            check_id(DATA / "aliases.tsv", alias)
            check_id(DATA / "aliases.tsv", icon)
            if alias in aliases and aliases[alias] != icon:
                raise DataError(
                    f"aliases.tsv: {alias!r} maps to both {aliases[alias]!r} and {icon!r}"
                )
            aliases[alias] = icon

        icon_ids: set[str] = {icon for _, (_, icon) in distros.items() if icon is not None}
        icon_ids.update(aliases.values())
        icon_ids.update(families.values())
        icon_ids.add("hamburger")
        for icon_id in sorted(icon_ids):
            check_id(DATA / "families.tsv", icon_id)

        icon_files = {path.stem: path for path in DATA.glob("*.icon")}
        icons: dict[str, dict] = {}
        for icon_id in sorted(icon_ids):
            path = icon_files.get(icon_id)
            if path is None:
                raise DataError(f"missing {icon_id}.icon")
            icons[icon_id] = parse_icon(path)

        ordered_icons = sorted(icon for icon in icon_ids if icon != "hamburger") + ["hamburger"]

        args.output_dir.mkdir(parents=True, exist_ok=True)
        files = {
            "systemicon-table.c": emit_table_c(icons, ordered_icons, distros, aliases, families),
            "systemicon-data.c": emit_data_c(icons, ordered_icons),
            "systemicon-data.h": emit_data_h(icons, ordered_icons),
        }
        for name, lines in files.items():
            (args.output_dir / name).write_text("\n".join(lines) + "\n", encoding="utf-8")

        print(
            f"wrote {args.output_dir}: {len(distros)} distro(s), {len(aliases)} alias(es), "
            f"{len(families)} family(ies), {len(ordered_icons)} icon(s), "
            f"{sum(len(icon['commands']) for icon in icons.values())} command(s)"
        )
        return 0
    except DataError as error:
        print(f"gen-icon-table: {error}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"gen-icon-table: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
