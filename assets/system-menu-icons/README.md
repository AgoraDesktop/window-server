# System menu icon data

This directory is the source of truth for the System menu's OS mark. The C table and vector arrays in
`src/systemicon-table.c`, `src/systemicon-data.c` and `src/systemicon-data.h` are generated from it; edit these
files, then run:

```
python3 tools/gen-icon-table.py
```

## Files

- `distros.tsv` — `os-release` ID, packaging family, exact icon (`-` means the family supplies the mark).
- `aliases.tsv` — alternate IDs and names that should resolve to a mark.
- `families.tsv` — the family fallback, including families that resolve to the hamburger.
- `*.icon` — vector path data, one file per mark, in a 24-unit grid (the hamburger uses 16).
- `svg/` — the exact Simple Icons SVG each generated `.icon` came from.
- `PROVENANCE.md` — source revision, license, trademark note, and regeneration steps. The OS marks are
  CC0 path data over third-party trademarks; the hamburger is original and CC0 1.0.

## Rules

- IDs are lower case `[a-z0-9._-]`, matching the `os-release` spec.
- `ID_LIKE` order is preserved; the resolver tries each token in order.
- An exact mark beats an alias; an alias beats `ID_LIKE`; `ID_LIKE` beats the family fallback; the hamburger is
  last.
- A family named in `distros.tsv` must exist in `families.tsv`. The generator rejects an unknown family.
- A duplicate alias pointing at different icons is an error. The generator rejects it.

See `SYSTEM-MENU-ICON-PLAN.md` for the full plan and `PROVENANCE.md` for the source and license of each mark.
