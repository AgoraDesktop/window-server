/*
 * cursor_theme — reading the Xcursor theme the pointer's pixels come from.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **THE THEME IS SOMEBODY ELSE'S DATA, SO READING IT HAS TO BE CHECKED AGAINST THE REAL FILES.** The claims worth
 * testing are not "the code compiled": they are which variant an output's scale gets, that the hotspot is the file's
 * own point and not a guess, that a premultiplied file comes out straight so both renderers blend it correctly, that an
 * animated cursor's frames all arrive, and that a directory which is not a theme — or a name which is a path — answers
 * nothing instead of reading something else. The fixture is therefore the vendored Vimix theme, passed in by CMake;
 * a fixture written by this test would agree with the reader by construction and prove neither.
 *
 * `../src/cursor-theme.c` is compiled in as it ships.
 */
#include "cursor-theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int s_failed;

static void check(int ok, const char *what)
{
	printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		s_failed++;
	}
}

/* ONE EXPECTED VARIANT: the device size asked for, the artwork it must select, and the hotspot the file carries. */
struct variant {
	double device_size;
	int extent;
	int nominal;
	int hotspot_x;
	int hotspot_y;
};

static void check_variants(struct aws_cursor_theme *theme)
{
	static const struct variant s_left_ptr[] = {
		{ 1.0, 32, 24, 4, 4 },
		{ 24.0, 32, 24, 4, 4 },		/* the 96-dpi design size: exactly this image */
		{ 32.0, 32, 24, 4, 4 },
		{ 32.5, 40, 30, 5, 5 },
		{ 40.0, 40, 30, 5, 5 },
		{ 41.0, 48, 36, 6, 6 },
		{ 48.0, 48, 36, 6, 6 },
		{ 49.0, 64, 48, 8, 8 },
		{ 64.0, 64, 48, 8, 8 },
		{ 4000.0, 64, 48, 8, 8 },	/* past the largest: the largest, never a stretched one */
	};
	size_t i;

	for (i = 0; i < sizeof s_left_ptr / sizeof s_left_ptr[0]; i++) {
		const struct variant *expected = &s_left_ptr[i];
		const struct aws_cursor_frames *frames =
			aws_cursor_theme_frames(theme, "left_ptr", expected->device_size);
		char what[160];

		snprintf(what, sizeof what, "left_ptr at %g device pixels is the %dx%d image (nominal %d)",
			 expected->device_size, expected->extent, expected->extent, expected->nominal);
		if (frames == NULL || frames->count < 1 || frames->frames[0].width != expected->extent ||
		    frames->frames[0].height != expected->extent ||
		    frames->frames[0].nominal_size != expected->nominal) {
			check(0, what);
			continue;
		}
		check(1, what);
		snprintf(what, sizeof what, "  and its hotspot is the file's own (%d,%d)",
			 expected->hotspot_x, expected->hotspot_y);
		check(frames->frames[0].hotspot_x == expected->hotspot_x &&
		      frames->frames[0].hotspot_y == expected->hotspot_y, what);
	}

	/* **A HOTSPOT IS PER CURSOR, NOT A CENTRE.** The hand's tip is at the top-left of its palm where the arrow's is
	 * inset; a reader that assumed anything symmetric would pass an arrow-only test and put a link's click under the
	 * wrong pixel. */
	{
		const struct aws_cursor_frames *frames = aws_cursor_theme_frames(theme, "pointer", 24.0);

		check(frames != NULL && frames->count == 1, "the pointing hand is one still image");
		check(frames != NULL && frames->frames[0].hotspot_x == 16 && frames->frames[0].hotspot_y == 4,
		      "and its hotspot is (16,4), which is not a centre and not the arrow's");
	}

	/* **THE I-BEAM IS TALL AND NARROW AND STILL SELECTED BY ITS BOX**, which is what makes a theme's variants a size
	 * vocabulary rather than an aspect-ratio guess. */
	{
		const struct aws_cursor_frames *small = aws_cursor_theme_frames(theme, "text", 24.0);
		const struct aws_cursor_frames *large = aws_cursor_theme_frames(theme, "text", 50.0);

		check(small != NULL && small->frames[0].width == 32 && small->frames[0].hotspot_x == 16,
		      "the text cursor's 24-point variant is its 32-pixel one");
		check(large != NULL && large->frames[0].width == 64,
		      "and 50 device pixels takes the 64-pixel one");
	}
}

/* THE PIXELS, ASKED OF THE IMAGE THE POINTER WOULD ACTUALLY BE DRAWN WITH. */
static void check_pixels(struct aws_cursor_theme *theme)
{
	const struct aws_cursor_frames *frames = aws_cursor_theme_frames(theme, "left_ptr", 24.0);
	const struct aws_cursor_image *image;
	int transparent = 0;
	int opaque = 0;
	int straight = 0;
	int coloured = 0;
	size_t count, i;

	if (frames == NULL || frames->count < 1) {
		check(0, "left_ptr decodes to an image");
		return;
	}
	image = &frames->frames[0];
	count = (size_t)image->width * (size_t)image->height;
	for (i = 0; i < count; i++) {
		uint32_t pixel = image->pixels[i];
		uint32_t alpha = pixel >> 24;
		uint32_t r = (pixel >> 16) & 0xFFu;
		uint32_t g = (pixel >> 8) & 0xFFu;
		uint32_t b = pixel & 0xFFu;

		if (alpha == 0) {
			transparent++;
			continue;
		}
		if (alpha == 255u) {
			opaque++;
			continue;
		}
		/* **A PREMULTIPLIED PIXEL CAN NEVER HAVE A CHANNEL LARGER THAN ITS ALPHA.** Finding one is proof that the
		 * decoder divided it out, which is the whole reason the files are not passed through untouched: both
		 * renderers blend straight alpha, and a premultiplied image blended that way draws a dark halo. */
		if (r > alpha || g > alpha || b > alpha) {
			straight++;
		}
		if (r != 0 || g != 0 || b != 0) {
			coloured++;
		}
	}
	check(transparent > 0, "the arrow's box has transparent pixels around it");
	check(opaque > 0, "and an opaque body");
	check(straight > 0, "and antialiased pixels whose channels exceed their alpha: premultiplication is undone");
	check(coloured > 0, "and the body is not a transparent placeholder");
}

static void check_animation(struct aws_cursor_theme *theme)
{
	const struct aws_cursor_frames *frames = aws_cursor_theme_frames(theme, "wait", 24.0);
	int i;
	int same = 1;

	check(frames != NULL && frames->count > 1, "the wait cursor decodes to more than one frame");
	if (frames == NULL || frames->count < 2) {
		return;
	}
	for (i = 0; i < frames->count; i++) {
		if (frames->frames[i].width != frames->frames[0].width ||
		    frames->frames[i].height != frames->frames[0].height ||
		    frames->frames[i].nominal_size != frames->frames[0].nominal_size) {
			same = 0;
		}
		if (frames->frames[i].delay_ms <= 0) {
			same = 0;
		}
	}
	check(same, "and every frame is the same size with a positive delay");
	/* THE FRAMES ARE THE THEME'S, NOT COPIES OF ONE: a decoder that kept only the first image and repeated it would
	 * make the check above pass and still never animate. */
	check(memcmp(frames->frames[0].pixels, frames->frames[frames->count - 1].pixels,
		     (size_t)frames->frames[0].width * (size_t)frames->frames[0].height * sizeof(uint32_t)) != 0,
	      "and the last frame is different artwork from the first");
}

int main(int argc, char **argv)
{
	struct aws_cursor_theme *theme;
	const char *directory;
	char empty[] = "/tmp/agora-cursor-theme-XXXXXX";

	printf("cursor-theme: the pointer's artwork, read from an Xcursor theme\n");

	if (argc < 2) {
		fprintf(stderr, "usage: %s THEME-DIRECTORY\n", argv[0]);
		return 2;
	}
	directory = argv[1];
	theme = aws_cursor_theme_open(directory);
	check(theme != NULL, "the vendored Vimix theme opens");
	if (theme == NULL) {
		printf("\nFAILURES\n");
		return 1;
	}
	check(aws_cursor_theme_directory(theme) != NULL &&
	      strcmp(aws_cursor_theme_directory(theme), directory) == 0,
	      "and names the directory it was read from");

	/* 1. **THE VARIANT IS CHOSEN FOR THE SIZE THE OUTPUT ASKED FOR**, which is the whole of what makes a themed
	 * pointer the right size on a 1x panel, a 96-dpi panel and a 2x one. */
	check_variants(theme);

	/* 2. **AND THE PIXELS ARRIVE THE WAY EVERY BLEND HERE EXPECTS THEM.** */
	check_pixels(theme);

	/* 3. **AN ANIMATED CURSOR'S FRAMES ARE ALL THERE**, even though the compositor draws the first today: the decoder
	 * is what a cursor-shape door would stand on. */
	check_animation(theme);

	/* 4. **A NAME IS A NAME AND NEVER A PATH.** `../Vimix` would be a theme file outside `cursors/`, and a reader that
	 * joined it would be reading whatever the caller named. */
	check(aws_cursor_theme_frames(theme, "../left_ptr", 24.0) == NULL, "a name containing a path is refused");
	check(aws_cursor_theme_frames(theme, ".", 24.0) == NULL, "and a name that is a directory claim is refused");
	check(aws_cursor_theme_frames(theme, "", 24.0) == NULL, "and an empty name is refused");

	/* 5. **A CURSOR THE THEME DOES NOT HAVE IS NOT AN ERROR**, it is the fallback: the compositor then draws its own
	 * arrow rather than refusing to have a pointer. */
	check(aws_cursor_theme_frames(theme, "no-such-cursor", 24.0) == NULL, "an unknown cursor answers nothing");
	check(aws_cursor_theme_frames(theme, "index.theme", 24.0) == NULL, "and so does a name that is not in cursors/");

	aws_cursor_theme_close(theme);

	/* 6. **A DIRECTORY THAT IS NOT A THEME IS NOT A THEME.** `<dir>/cursors` is the whole test, and it is what makes
	 * `AGORA_CURSOR_THEME` pointing at the wrong place fall back instead of reading a directory of nothing. */
	check(aws_cursor_theme_open("/nonexistent/agora/theme") == NULL, "a directory that is not there is not a theme");
	if (mkdtemp(empty) != NULL) {
		check(aws_cursor_theme_open(empty) == NULL, "an empty directory is not a theme");
		rmdir(empty);
	} else {
		check(0, "could make a temporary empty directory to test");
	}

	/* 7. **AND A THEME CAN BE RE-OPENED**, which is what a theme change under a running server does. */
	theme = aws_cursor_theme_open(directory);
	check(theme != NULL && aws_cursor_theme_frames(theme, "left_ptr", 24.0) != NULL,
	      "a second open of the same theme reads it again");
	aws_cursor_theme_close(theme);
	check(aws_cursor_theme_open(NULL) == NULL, "and no directory at all is not a theme");

	printf("\n%s\n", s_failed == 0 ? "all checks passed" : "FAILURES");
	return (s_failed == 0) ? 0 : 1;
}
