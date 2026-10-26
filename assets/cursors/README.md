# Cursor theme data

This directory holds the pointer's artwork: the **Vimix** Xcursor theme, vendored under its own
licence (GPL-3.0) with a provenance record beside it. `window-server` reads it, installs it as an
ordinary freedesktop cursor theme, and uses it on both display paths.

## Files

- `Vimix/` — the theme, as an Xcursor theme directory: `cursors/` holds the `Xcur` files, and
  `index.theme`, `COPYING` and `PROVENANCE.md` sit beside them. `cmake --install` copies it to
  `<prefix>/share/icons/Vimix`.
- `Vimix/PROVENANCE.md` — upstream project, licence, what Agora added, the theme's own size and
  hotspot table, and the pixel format. Read it before changing anything here.

## Rules

- **The `cursors/` directory is upstream's and is not edited.** Names are Xcursor cursor names
  (`left_ptr`, `text`, `pointer`, `col-resize`, …) and 64 of the 111 entries are symlinks to canonical
  files — `left_ptr` is one, and it is the name the compositor asks for.
- **Selection is by device pixels, not by nominal size.** `src/cursor-theme.c` reads every image a file
  contains and picks the smallest whose larger side is at least the size the output asks for, falling
  back to the largest. No image is resampled, so a pointer is always the theme's own pixels.
- **`index.theme` inherits nothing.** See `Vimix/PROVENANCE.md`: an inherited theme would make a
  session depend on what else the host has installed, and the compositor's own arrow is the fallback
  instead.
- **`AGORA_CURSOR_THEME` points the compositor at another theme directory**, so a person can use a
  different theme without rebuilding, and the demo scripts use it to reach this checkout. The
  compositor's startup line says which directory it actually read.
