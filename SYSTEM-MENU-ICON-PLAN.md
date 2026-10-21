# System menu OS marks — implementation plan

**Status:** Phase 3 implemented — detection, the eleven distinct exact marks (FreeBSD stands in for DragonFlyBSD
for now), the generated tables and vector data, the coverage rasterizer, the diagnostic commands and the per-mark
screenshot matrix all exist and are tested.

The System menu at the left of the bar is drawn by the window server itself, not by a client. The server identifies
the running OS and draws that OS's mark in the same monochrome vector style and the same 16-pixel box, with the
hamburger as the fallback.

## Decisions

1. **Twelve exact marks to start.** Expand later; do not try to cover every distro on the first pass.
2. **Exact marks are preferred.** Use the official vector marks, with provenance. Do not substitute an
   approximate or generic mark for a distro that has one of the twelve.
3. **No runtime icon packs yet.** Everything ships compiled into the server. The registry is designed so a
   data-pack loader can be added later without changing the resolver.
4. **The hamburger is the only generic fallback.** There is no generic "Linux" penguin or generic "BSD"
   daemon mark. If nothing matches, the bar keeps today's hamburger.
5. **`ID_LIKE` family fallback is enabled.** A Debian derivative with no exact mark gets the Debian swirl,
   an Ubuntu derivative gets the Ubuntu mark, and so on.

## The twelve marks

| # | Icon id | Mark | Primary IDs |
|---|---|---|---|
| 1 | `debian` | Debian swirl | `debian` |
| 2 | `ubuntu` | Ubuntu circle of friends | `ubuntu` |
| 3 | `linuxmint` | Linux Mint | `linuxmint` |
| 4 | `fedora` | Fedora | `fedora` |
| 5 | `redhat` | Red Hat | `rhel`, `centos`, `rocky`, `almalinux`, `oracle` |
| 6 | `archlinux` | Arch Linux | `arch`, `archlinux` |
| 7 | `opensuse` | openSUSE | `opensuse-leap`, `opensuse-tumbleweed`, `sles` |
| 8 | `alpinelinux` | Alpine Linux | `alpine`, `postmarketos` |
| 9 | `freebsd` | FreeBSD | `freebsd`, `ghostbsd`, `hardenedbsd` |
| 10 | `openbsd` | OpenBSD blowfish | `openbsd` |
| 11 | `netbsd` | NetBSD | `netbsd` |
| 12 | `freebsd` | DragonFly BSD — FreeBSD mark for now | `dragonfly`, `dragonflybsd` |

Plus `hamburger`, which is not one of the twelve. DragonFlyBSD deliberately shares the FreeBSD mark until a
DragonFly vector is sourced; there is no `dragonflybsd` icon object, only the alias.

The family coverage this buys:

- **Debian family:** Debian, Kali, Raspberry Pi OS, Devuan, PureOS, Trisquel, MX, Tails, Qubes, Armbian,
  DietPi.
- **Ubuntu family:** Ubuntu, Pop!_OS, elementary, Zorin, KDE neon, Kubuntu, Xubuntu, Lubuntu, Ubuntu MATE,
  Budgie, Edubuntu, Ubuntu Studio.
- **Fedora family:** Fedora, Nobara, Bazzite, Ultramarine.
- **Red Hat family:** RHEL, CentOS, CentOS Stream, Rocky, AlmaLinux, Oracle Linux, Scientific Linux,
  Amazon Linux, CloudLinux, EuroLinux.
- **Arch family:** Arch, Manjaro, EndeavourOS, Garuda, Artix, CachyOS, RebornOS, BlackArch, ArchCraft.
- **SUSE family:** openSUSE Leap, openSUSE Tumbleweed, openSUSE MicroOS, SLES, SLED, GeckoLinux.
- **Alpine family:** Alpine, postmarketOS.
- **BSD:** FreeBSD, GhostBSD, HardenedBSD, NomadBSD; OpenBSD; NetBSD; DragonFly BSD.

Standalone distros outside these families (Gentoo, Void, NixOS, Slackware, Solus, Mageia, OpenMandriva,
Chimera) fall through to the hamburger until they are added in a later expansion. That is deliberate: the
hamburger is the fallback, not a generic Linux mark.

## Detection

### Identity

Replace the single `aws_os_kind` with a parsed identity:

```c
struct aws_os_identity {
    char sysname[64];    /* uname(2): Linux, FreeBSD, OpenBSD, ... */
    char id[64];         /* os-release ID, normalized */
    char id_like[256];   /* os-release ID_LIKE, normalized */
    char name[128];      /* NAME/PRETTY_NAME, diagnostics only */
    const char *source;  /* "/etc/os-release", "uname", "probe:/etc/arch-release", ... */
    int is_bsd;
};
```

`ID` and `ID_LIKE` follow the freedesktop spec: lowercase `[0-9a-z._-]`, quoting stripped, and `ID_LIKE`
is a space-separated list in order of closeness. `NAME` is never used for matching.

### Probe ladder

All in C. No shelling out, no `popen`.

1. `uname()` for `sysname`.
2. `/etc/os-release`, then `/usr/lib/os-release` (8 KiB cap). `/etc` is the admin override; `/usr/lib` is
   the vendor copy.
3. If no usable `ID`: `/etc/lsb-release` → `DISTRIB_ID`.
4. Release-file probes as a last resort:
   - `/etc/debian_version` → `debian`
   - `/etc/arch-release` → `arch`
   - `/etc/gentoo-release` → `gentoo`
   - `/etc/fedora-release` → `fedora`
   - `/etc/redhat-release` → parse for RHEL/CentOS/Rocky/Alma/Oracle/Scientific
   - `/etc/SuSE-release` or `/etc/SuSE-brand` → `opensuse`
   - `/etc/alpine-release` → `alpine`
   - `/etc/slackware-version` → `slackware`
   - `/etc/nixos/` → `nixos`
5. `sysname` family fallback.

BSD notes:

- FreeBSD 13+ often ships `/etc/os-release`; OpenBSD does not.
- OpenBSD, NetBSD, DragonFly, MidnightBSD: `uname` is authoritative.
- GhostBSD, HardenedBSD, NomadBSD: use `os-release` when present; otherwise `uname=FreeBSD` falls back to
  the FreeBSD mark, which is the correct family answer.
- If `sysname` says BSD but `os-release` claims a Linux-only ID, trust `sysname` for the platform family and
  log the disagreement.

Detection runs once at server creation. No file I/O or `uname` per frame.

## Resolution

The resolver returns a pointer to an icon object; do not use a large enum.

```c
struct aws_system_icon;

struct aws_server {
    ...
    const struct aws_system_icon *system_menu_icon; /* resolved, never NULL */
};

int aws_server_set_system_menu_icon(struct aws_server *server, const char *name);
```

Order:

1. Exact compiled-in `ID` in `distros.tsv`.
2. Alias in `aliases.tsv`.
3. Each `ID_LIKE` token in order: exact, then alias.
4. Family icon from `families.tsv`, using the family recorded for the exact ID or the one carried by
   `ID_LIKE`.
5. Hamburger.

Examples:

| `/etc/os-release` | Result |
|---|---|
| `ID=debian` | Debian swirl |
| `ID=kali`, `ID_LIKE=debian` | Debian swirl |
| `ID=linuxmint`, `ID_LIKE="ubuntu debian"` | Linux Mint |
| `ID=pop`, `ID_LIKE="ubuntu debian"` | Ubuntu |
| `ID=rocky`, `ID_LIKE="rhel centos fedora"` | Red Hat |
| `ID=manjaro`, `ID_LIKE=arch` | Arch Linux |
| `ID=opensuse-tumbleweed`, `ID_LIKE="opensuse suse"` | openSUSE |
| `ID=postmarketos`, `ID_LIKE=alpine` | Alpine |
| `ID=ghostbsd`, `ID_LIKE=freebsd` | FreeBSD |
| `sysname=OpenBSD`, no os-release | OpenBSD |
| `sysname=NetBSD`, no os-release | NetBSD |
| `ID=gentoo` | Hamburger |
| `sysname=Linux`, unknown ID | Hamburger |

## Geometry and rasterizer

- Keep the existing menu bar height and System menu square.
- Glyph box: **16×16 at x = 10, y = 4** (`AWS_SYSTEM_MENU_LEFT_MARGIN`, centered in the 24-pixel bar).
- Ink: `AWS_SYSTEM_MENU_GLYPH_COLOUR` only. Edges are coverage-blended into `AWS_MENU_BAR_COLOUR`.
- The hamburger becomes a path too, so there is one drawing path. Define it in the 16-unit box so rows
  4–19 keep the current look and the existing screenshot probes still pass.
- Path format: subpaths of `MOVE`, `LINE`, `QUAD`, `CUBIC`, `CLOSE`; normalized `[0,1]` coordinates;
  even-odd fill.
- Flatten curves, then rasterize into a 16×16 `uint8_t` coverage grid with 4×4 supersampling. Cache it on
  first use. Rasterizing once per run is enough; the composite only blends 256 cached bytes per frame.
- `output.c` keeps the blending: `aws_blend_pixel(destination, ink, coverage)`.
- No new libraries. `uname(2)`, `stdio`, `ctype`, and `math` are already available.

## Data and build

No runtime packs in this version. Everything is compiled in, but the mapping is data-driven so expansion
is cheap.

```text
window-server/
  assets/system-menu-icons/
    distros.tsv       # id <TAB> family <TAB> icon
    aliases.tsv       # alias <TAB> icon
    families.tsv      # family <TAB> icon
    *.icon            # vector path data
    svg/              # the source SVG each .icon was generated from
    PROVENANCE.md     # source, license, modifications, per icon
  src/
    osdetect.c
    osdetect.h
    systemicon.c
    systemicon.h
    systemicon-data.c   # generated icon paths
    systemicon-data.h   # generated path declarations
    systemicon-table.c  # generated lookup tables
  tools/
    svg2icon.py         # SVG <path> -> .icon
    gen-icon-table.py   # TSV + .icon -> C
  tests/
    system_icon.c
    check_icon.py
    run-system-menu-icons.sh
```

`tools/gen-icon-table.py` emits `systemicon-data.c` and `systemicon-table.c`. The generated files are
checked in, so a normal build needs no Python. A `make icons-check` target regenerates into a temporary
file and diffs, preventing silent drift.

`.icon` format:

```text
# debian.icon
id debian
grid 24
fill nonzero
M 13.88 12.685
C 13.48 12.685 13.96 12.885 14.481 12.965
...
Z
```

The SVG converter supports `M/L/H/V/C/S/Q/T/A/Z` (relative and absolute) and converts arcs to cubics. The
source SVGs use a 24-unit viewBox; the generator normalizes to `[0,1]` for the rasterizer.

### Sources for the marks

The eleven distinct marks are exact monochrome paths from Simple Icons at revision
`c71c69e3a27e4436936b831c72643e6513b9f49b` (CC0 path data; the marks remain trademarks):

- `debian`, `ubuntu`, `linuxmint`, `fedora`, `redhat`, `archlinux`, `opensuse`, `alpinelinux`, `freebsd`,
  `openbsd`, `netbsd`

DragonFly BSD has no Simple Icons entry and uses the `freebsd` mark for now. There is no `dragonflybsd.icon`;
the ID and aliases resolve to `freebsd`. `PROVENANCE.md` records the revision, the source SVGs under `svg/`, the
CC0/trademark split, and the regeneration steps.

The exact paths are used as-is; `svg2icon.py` converts arcs to cubics and keeps the 24-unit grid, and the
rasterizer supersamples at 16 px. No hand simplification was applied.

## CLI and diagnostics

```text
--system-menu-icon auto|NAME     force a mark by id or alias, or auto (default)
--list-system-menu-icons         print available marks and exit
--dump-os-identity               print sysname, os-release source, ID, ID_LIKE,
                                 family, and chosen mark; exit
```

`--dump-os-identity` and `--list-system-menu-icons` work before the Wayland display or backend is created,
so they run on a headless machine and in bug reports.

Log once at startup:

```text
window-server: system menu: debian (source /etc/os-release, ID=debian)
```

If the override is invalid, print the allowed names and exit non-zero.

## Tests

### Unit tests (`tests/system_icon.c`)

- Fixture table of 40+ `os-release` snippets: all twelve marks, their families, and the hamburger cases.
- Precedence: exact beats alias, alias beats `ID_LIKE`, first `ID_LIKE` token wins, family fallback,
  hamburger fallback.
- Parsing: quoted values, missing `ID`, empty `ID_LIKE`, malformed lines, `lsb-release`.
- BSD: `uname` mapping for FreeBSD/OpenBSD/NetBSD/DragonFly/MidnightBSD; `sysname` wins over a conflicting
  Linux `os-release`.
- Every icon: non-empty coverage, ink bounding box inside 16×16, and a deterministic FNV-1a hash plus ink count
  matching the golden table in `tests/system_icon.c`.
- A `--sheet` mode that renders all marks at 8× for visual review.

### Integration tests

- Existing `window-server-headless` passes `--system-menu-icon hamburger`, so it stays host-independent.
- New `window-server-system-menu-icons` runs the headless server once per mark with one real client, then
  checks the glyph box: bar colour, expected ink-pixel count, anti-aliasing, monochrome pixels, and no ink
  elsewhere in the bar.
- Audit `libagora-app/tests/run-*.sh`; pin `--system-menu-icon hamburger` wherever a screenshot could
  sample the System menu.

### Optional CI matrix

Linux containers for Debian, Ubuntu, Fedora, Arch, Alpine; BSD VMs for FreeBSD/OpenBSD if available. Each
runs `--dump-os-identity` and the unit suite. The unit fixtures do the real coverage work; the matrix
proves the probes against real systems.

## Licensing and provenance

- The server code stays MIT. The hamburger path is original and CC0 1.0.
- The OS marks do not become MIT. Keep them in `assets/system-menu-icons/` with a `PROVENANCE.md` and a
  separate notice.
- Simple Icons' path data is CC0 1.0; the brand marks remain trademarks and may have their own usage
  rules. Record source URL, upstream revision, license, and modifications for each icon.
- A downstream that cannot redistribute a mark can replace its `.icon` with the hamburger and regenerate; a
  build-time pack option is future work.
- Do not describe the marks as MIT-licensed in the README.

## Implementation phases

1. **Detection and resolution tables.** *Done.* `osdetect.c`, identity struct, probe ladder, TSV tables, CLI
   override, `--dump-os-identity`, `--list-system-menu-icons`, unit fixtures. Output remains the
   hamburger.
2. **Rasterizer and hamburger.** *Done.* Coverage module, 16×16 box, hamburger path, golden test, existing
   headless probes passing.
3. **Twelve marks.** *Done.* The Simple Icons SVGs are checked in under `svg/`, converted by
   `tools/svg2icon.py` into `.icon` files, and compiled into `systemicon-data.c`; provenance and per-mark
   coverage hashes are checked in. DragonFlyBSD uses the FreeBSD mark for now.
4. **Wire-up and screenshots.** *Done.* The resolved mark is drawn in `output.c`, the per-mark screenshot
   matrix runs in CTest, and `DESIGN.md`/`README.md` are updated.

Each phase builds and passes tests on its own.

## Acceptance checklist

- [x] On Debian, `auto` draws the swirl; on Ubuntu, Fedora, Arch, openSUSE, Alpine, Linux Mint, RHEL,
      FreeBSD, OpenBSD, and NetBSD, it draws that mark. DragonFlyBSD draws the FreeBSD mark for now.
- [x] Debian derivatives without their own mark get the swirl; Ubuntu derivatives get the Ubuntu mark;
      Arch derivatives get Arch; RHEL derivatives get Red Hat.
- [x] Unknown Linux and unknown BSD get the hamburger.
- [x] `--system-menu-icon` forces any of the marks; invalid names fail clearly.
- [x] All marks are monochrome, anti-aliased, and confined to the 16×16 box at x 10, y 4.
- [x] Detection runs once; no per-frame file I/O, `uname`, or rasterization.
- [x] Existing headless test is host-independent and still passes.
- [x] Per-mark screenshot matrix and unit fixtures pass.
- [x] `PROVENANCE.md` covers every mark; the code remains MIT.
