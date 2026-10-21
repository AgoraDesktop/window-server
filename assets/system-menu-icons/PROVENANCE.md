# System menu icon provenance

THE CODE IS MIT; THE OS MARKS ARE NOT. The path data in the OS `.icon` files comes from Simple Icons and is
CC0 1.0, but the marks themselves are trademarks of their projects. The hamburger is original work dedicated to
CC0 1.0. The marks are used here to identify the operating system the compositor is running on, not to imply
endorsement. A downstream that cannot redistribute a mark can replace its `.icon` file with the hamburger and
remove the corresponding entry from the tables, then regenerate.

## Upstream

- Project: [Simple Icons](https://github.com/simple-icons/simple-icons)
- Revision: `c71c69e3a27e4436936b831c72643e6513b9f49b`
- License: [CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/) for the path data
- Source SVGs: checked in under `svg/`, one per mark, exactly as fetched from that revision
- Conversion: `tools/svg2icon.py`; SVG arcs are converted to cubic Béziers, coordinates are kept in the source
  24-unit grid in the `.icon` file, and no shape is redrawn. The generator normalizes to `[0,1]` for the rasterizer.

## Marks

| icon id | source | notes |
|---|---|---|
| `debian` | `icons/debian.svg` | Debian swirl |
| `ubuntu` | `icons/ubuntu.svg` | Ubuntu circle of friends |
| `linuxmint` | `icons/linuxmint.svg` | Linux Mint |
| `fedora` | `icons/fedora.svg` | Fedora |
| `redhat` | `icons/redhat.svg` | Red Hat |
| `archlinux` | `icons/archlinux.svg` | Arch Linux |
| `opensuse` | `icons/opensuse.svg` | openSUSE |
| `alpinelinux` | `icons/alpinelinux.svg` | Alpine Linux |
| `freebsd` | `icons/freebsd.svg` | FreeBSD |
| `openbsd` | `icons/openbsd.svg` | OpenBSD blowfish |
| `netbsd` | `icons/netbsd.svg` | NetBSD |
| `hamburger` | none | Hand-written original: three bars, CC0 1.0, the fallback for unknown systems |

## The hamburger

The hamburger is original to this project. Its path is hand-written in `hamburger.icon` (three bars in the same
16-unit box the old compositor drew) and is dedicated to the public domain under
[CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/). There is no third-party source for it.

`dragonfly` and `dragonflybsd` deliberately resolve to the `freebsd` mark for now. There is no DragonFly BSD artwork
in this set; the FreeBSD mark is the temporary stand-in recorded in `distros.tsv`, `aliases.tsv` and
`families.tsv`.

## The Simple Icons URLs

Each source SVG was fetched from:

```
https://raw.githubusercontent.com/simple-icons/simple-icons/c71c69e3a27e4436936b831c72643e6513b9f49b/icons/<slug>.svg
```

where `<slug>` is the icon id above. The exact URL is also recorded in the first line of each `.icon` file.

## Regenerating

```sh
python3 tools/svg2icon.py assets/system-menu-icons/svg/debian.svg \
    assets/system-menu-icons/debian.icon --id debian \
    --source https://raw.githubusercontent.com/simple-icons/simple-icons/<revision>/icons/debian.svg

python3 tools/gen-icon-table.py
```

`tests/run-icon-table-check.sh` fails if the generated `src/systemicon-table.c`, `src/systemicon-data.c` or
`src/systemicon-data.h` is stale relative to the TSVs and `.icon` files. Regenerating an `.icon` from its `svg/`
source is a separate, deliberate step.
