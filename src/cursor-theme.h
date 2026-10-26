/*
 * cursor-theme.h — the pointer's artwork, read from an Xcursor theme.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **A CURSOR THEME IS A DIRECTORY OF FILES, AND THE FILE FORMAT IS XCURSOR.** Every freedesktop cursor
 * theme — Vimix, Adwaita, Breeze — is `<theme>/cursors/<name>` where each `name` is an `Xcur` file: a table
 * of contents and, for each nominal size, one or more 32-bit images with a hotspot and a frame delay. That
 * is the whole format, so this module reads it rather than linking `libXcursor`: the compositor would
 * otherwise take an X11 library into a Wayland server to read a file, and the two display paths (the DRM
 * one that composites the image, and the nested X11 one that hands it to the host) would each read it
 * their own way and could disagree about which variant a pointer got.
 *
 * **WHAT A THEME DECIDES, AND WHAT AGORA DECIDES.** The theme supplies the artwork, its size variants, its
 * hotspots and its frame delays. Agora decides WHICH variant an output gets: the smallest image whose
 * larger side covers the size the output asks for, or the largest when none does — `libXcursor`'s own
 * rule, and the reason a 24-point pointer is the theme's 32×32 arrow on a 96-dpi panel and its 48×48
 * arrow at 2x. Images are never resampled: a themed pointer is the theme's pixels, which is why the
 * variants exist.
 *
 * **THE PIXELS ARE STRAIGHT (NON-PREMULTIPLIED) ARGB** even though the files store them premultiplied.
 * Both Agora renderers blend straight alpha (the CPU one in `cursor.c`, the GL one with
 * `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`), so the division is done once here instead of in
 * every blend — and the one consumer that needs premultiplied pixels again (the X11 host cursor, whose
 * `XRender` picture is ARGB32) multiplies them back where it fills the pixmap.
 */

#ifndef AGORA_WINDOW_SERVER_CURSOR_THEME_H
#define AGORA_WINDOW_SERVER_CURSOR_THEME_H

#include <stdint.h>

/* ONE IMAGE OF ONE CURSOR, as the file holds it. `pixels` is owned by the theme and is valid until the
 * theme is closed; it is 0xAARRGGBB with STRAIGHT alpha, `width * height` entries, row-major. */
struct aws_cursor_image {
	int nominal_size;	/* the file's own "Size" for this image — its design size in points */
	int width;
	int height;
	int hotspot_x;		/* the tip, in this image's own pixels */
	int hotspot_y;
	int delay_ms;		/* how long an animated cursor shows this frame */
	const uint32_t *pixels;
};

/* THE FRAMES OF ONE CURSOR AT ONE SIZE. One entry for a still cursor; `wait` and `progress` have 23. The
 * compositor draws frame zero today — the only shape it shows is the arrow, which does not animate — but
 * the frames are decoded and kept because a cursor-shape door, when it exists, is a selection over this
 * same list. */
struct aws_cursor_frames {
	int count;
	const struct aws_cursor_image *frames;
};

struct aws_cursor_theme;

/* OPEN A THEME DIRECTORY: the one that contains `cursors/` and `index.theme`. NULL when the directory is
 * not there or has no `cursors/` — a missing theme is not an error, because the compositor draws its own
 * arrow (`cursor.c`) and every session without a theme installed still has a pointer. */
struct aws_cursor_theme *aws_cursor_theme_open(const char *directory);
void aws_cursor_theme_close(struct aws_cursor_theme *theme);

/* THE DIRECTORY THIS THEME WAS READ FROM, for a startup line that names what a person is looking at. */
const char *aws_cursor_theme_directory(const struct aws_cursor_theme *theme);

/* **WHERE THE THEME COMES FROM WHEN NOBODY SAID.** `AGORA_CURSOR_THEME` first — a developer running the
 * build trees, or a person trying another theme, has not installed one — then the directory this build
 * installs the vendored Vimix theme into. Never NULL. */
const char *aws_cursor_theme_default_directory(void);

/* THE CURSOR NAMED `name`, IN THE VARIANT FOR `device_size` DEVICE PIXELS. Answers NULL when the theme has
 * no such cursor or the file is unreadable; the result belongs to the theme and is cached, so asking
 * twice for the same name does not read the file twice. `name` is a plain Xcursor name (`left_ptr`) —
 * anything with a `/` or a leading `.` is refused rather than turned into a path. */
const struct aws_cursor_frames *aws_cursor_theme_frames(struct aws_cursor_theme *theme, const char *name,
							double device_size);

#endif /* AGORA_WINDOW_SERVER_CURSOR_THEME_H */
