# Vimix cursor theme provenance

THE THEME IS GPL-3.0 AND IS NOT AGORA'S. Everything under `cursors/` — 111 entries, 64 of them
symlinks — is the Vimix cursor theme by **vinceliuice**, copied into this tree unchanged. Agora added
no cursor artwork and redrew nothing; this directory only adds the theme's metadata (`index.theme`),
its upstream licence text (`COPYING`) and this record.

## Upstream

- Project: [vinceliuice/Vimix-cursors](https://github.com/vinceliuice/Vimix-cursors) — the Vimix cursor
  theme, by [vinceliuice](https://github.com/vinceliuice)
- Revision: **not recorded by the delivery.** The theme arrived in this repository as the directory
  `cursors/` at the workspace root, with no git link and no commit id, and the files carry no Xcursor
  `COMMENT` chunk naming a version. The cursor files' own timestamps are 2020-02-19. Whoever re-vendors
  or updates the theme should record the upstream commit here; until then this provenance is exact
  about *what* is here and honest about *which revision* it is.
- Licence: **GPL-3.0** — see `COPYING`. Upstream states version 3 and does not state "or later", so
  neither does this record.

## Why it lives in `window-server`

The theme is data, not linked code, but its licence still decides where it can live. `window-server`
is **GPL-3.0-or-later** — the only licence in the tree that a GPL-3.0 theme sits in without adding a
term to anything else. The `libagora-*` libraries are LGPL, and the theme is deliberately not placed in
any of them.

The compositor is also the component that owns the pointer's pixels (see `src/cursor.c`), which is the
only place in Agora that can use a cursor theme today: on a DRM output it composites the theme's image,
and in the nested X11 backend it hands the theme's image to the host as the window's cursor.

## What is here

```
Vimix/
├── COPYING            GPL-3.0, as the theme's upstream licence (not Agora's)
├── index.theme        added by Agora: the freedesktop theme name, with no `Inherits`
├── PROVENANCE.md      this record
└── cursors/           the theme itself, unchanged
```

`index.theme` is Agora's only addition to the theme's behaviour. It names the theme and sets
`Inherits=` **empty on purpose**: a cursor theme that inherits would silently pull artwork from
whatever other theme the host happens to have installed, so a session on a machine with no such theme
would look different from one with it. Agora's own fallback is the compositor's drawn arrow
(`src/cursor.c`), which is reproducible everywhere.

## The theme's own design, as Agora reads it

Xcursor files, version 1.0 (`Xcur`), one TOC entry per image, and four nominal sizes per cursor. The
artwork is **4/3 the nominal size**, which is what makes the nominal size a size in *points*: at 96 dpi
a 24-point pointer is 32 device pixels, exactly the theme's 24-nominal image. The hotspots are the
files' own, per variant and per cursor.

| nominal size | image (px) | `left_ptr` hotspot | `text` hotspot | `pointer` hotspot |
|---|---|---|---|---|
| 24 | 32×32 | (4, 4) | (16, 16) | (16, 4) |
| 30 | 40×40 | (5, 5) | (20, 20) | (20, 5) |
| 36 | 48×48 | (6, 6) | (24, 24) | (24, 6) |
| 48 | 64×64 | (8, 8) | (32, 32) | (32, 8) |

`wait` is the one animated cursor: 23 frames per variant, 30 ms each. Agora decodes the frames but
draws the first, because the only shape the compositor shows today is the arrow (see
`src/cursor-theme.h` for where the animation would be driven).

The pixels in the files are **premultiplied** ARGB. The compositor's two renderers both blend straight
alpha, so the decoder divides the premultiplication out on the way in — one conversion point, tested
(`tests/cursor_theme.c`).

## Licence obligations

The theme is distributed here under GPL-3.0, in a GPL-3.0-or-later program, with `COPYING` beside it
and this record naming its source. `THIRD-PARTY.md` at the tree root lists it with the other vendored
art. A downstream that cannot ship GPL-3.0 artwork can delete this directory: the compositor then draws
its own arrow, which is original to Agora and is what every test without `AGORA_CURSOR_THEME` exercises.
