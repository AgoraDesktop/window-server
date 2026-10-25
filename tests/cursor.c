/*
 * cursor — the pointer the compositor draws for an output that has no host cursor.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **THE REPORT THIS FILE EXISTS FOR WAS "the desktop renders, but no mouse cursor is visible", on a DRM output with
 * both renderers.** Pointer motion and buttons worked: the compositor received input and had nothing to draw with it,
 * because `wl_pointer.set_cursor` was an empty function and neither renderer painted a pointer of its own. The fix is
 * an arrow the compositor owns (src/cursor.c), and this is the part of it that needs no scanner: the RASTER, the
 * SCALE IT IS MADE FOR, and WHERE IT LANDS.
 *
 * IT IS A UNIT TEST AND NOT A SCREENSHOT, because the claim is arithmetic about device pixels — a box, a hotspot and
 * a clipped blend — and the headless backend deliberately draws no cursor at all (its screenshots are compared
 * against the scene, not against a pointer). `../src/cursor.c` is compiled in as it ships; the server is a plain
 * struct this file fills in, which is all the cursor code ever reads of it.
 */
#include "server.h"
#include "cursor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failed;

static void check(int ok, const char *what)
{
	printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		s_failed++;
	}
}

#define BACKGROUND 0xFF204060u

/* A SERVER WITH A FRAMEBUFFER AND NOTHING ELSE, which is the whole of what the cursor asks of it. */
static void reset(struct aws_server *server, double scale, int width, int height)
{
	size_t count = (size_t)width * (size_t)height;
	size_t i;

	memset(server, 0, sizeof *server);
	aws_cursor_finish(server);	/* the previous case's raster, if the sizes changed under it */
	server->backend.width = width;
	server->backend.height = height;
	server->backend.scale = scale;
	server->backend.owns_cursor = 1;
	server->backend.pixels = calloc(count, sizeof *server->backend.pixels);
	server->pointer_inside = 1;
	server->pointer_x = 10;
	server->pointer_y = 10;
	for (i = 0; i < count; i++) {
		server->backend.pixels[i] = BACKGROUND;
	}
}

static void finish(struct aws_server *server)
{
	aws_cursor_finish(server);
	free(server->backend.pixels);
	server->backend.pixels = NULL;
}

static uint32_t at(struct aws_server *server, int x, int y)
{
	return server->backend.pixels[(size_t)y * (size_t)server->backend.width + (size_t)x];
}

static unsigned luminance(uint32_t pixel)
{
	unsigned r = (pixel >> 16) & 0xFFu;
	unsigned g = (pixel >> 8) & 0xFFu;
	unsigned b = pixel & 0xFFu;

	return (r * 30u + g * 59u + b * 11u) / 100u;
}

/* THE TWO TONES THE ARROW IS MADE OF, ASKED OF A BOX OF THE FRAME: a black body and a white fringe, both present.
 * A shape drawn in one colour would be an arrow that vanishes on half the desktops a person might put behind it. */
static int box_has_two_tones(struct aws_server *server, int left, int top, int width, int height)
{
	int dark = 0, light = 0;
	int row, column;

	for (row = 0; row < height; row++) {
		for (column = 0; column < width; column++) {
			unsigned value;

			if (left + column < 0 || left + column >= server->backend.width ||
			    top + row < 0 || top + row >= server->backend.height) {
				continue;
			}
			value = luminance(at(server, left + column, top + row));
			if (value < 40u) {
				dark = 1;
			}
			if (value > 200u) {
				light = 1;
			}
		}
	}
	return dark && light;
}

int main(void)
{
	struct aws_server server;

	printf("cursor: the compositor-owned fallback pointer\n");

	/* 1. **A BACKEND WITH A HOST POINTER DRAWS NOTHING.** The nested X11 backend is inside a window whose pointer
	 * the host already shows, and a screenshot is of the scene: a second arrow in either would be a pointer nobody
	 * asked for. `owns_cursor` is the one fact that decides it, and it is zero for x11 and headless. */
	reset(&server, 1.0, 64, 64);
	server.backend.owns_cursor = 0;
	check(!aws_cursor_visible(&server), "a backend with a host cursor shows no compositor cursor");
	check(aws_cursor_image(&server) == NULL, "and answers no image to upload");
	aws_cursor_draw_cpu(&server);
	check(at(&server, 10, 10) == BACKGROUND, "and the frame is untouched where the pointer is");

	/* 2. **AND A POINTER THAT IS NOT ON THE OUTPUT ALSO DRAWS NOTHING**, which is what a leave is. */
	server.backend.owns_cursor = 1;
	server.pointer_inside = 0;
	check(!aws_cursor_visible(&server), "a pointer outside the output draws nothing");
	aws_cursor_draw_cpu(&server);
	check(at(&server, 10, 10) == BACKGROUND, "and the frame is untouched");

	/* 3. **THE ARROW AT 1x: TWELVE BY NINETEEN DEVICE PIXELS, TIP AT (1,1).** The box is the point span rounded UP
	 * plus one pixel of room for the fringe, and the hotspot is the tip — the pixel the pointer's own coordinate
	 * sits on. */
	server.pointer_inside = 1;
	aws_cursor_ensure(&server);
	check(server.cursor.pixels != NULL, "the raster is made for the output's scale");
	check(server.cursor.width == 12 && server.cursor.height == 19,
	      "an 11x18 point arrow is a 12x19 device image at 1x, with a pixel of fringe");
	check(server.cursor.hotspot_x == 1 && server.cursor.hotspot_y == 1, "and its hotspot is the tip");

	/* 4. **THE ARROW IS BLACK AND ITS FRINGE IS WHITE.** Asked of the drawn box rather than of the raster, so this
	 * is the composited result and not the shape's own claim. */
	aws_cursor_draw_cpu(&server);
	check(box_has_two_tones(&server, 9, 9, 12, 19),
	      "the drawn pointer has a black body and a white outline");
	check(at(&server, 0, 0) == BACKGROUND, "nothing outside its box is touched");

	/* 5. **THE TIP LANDS ON THE POINTER.** At scale 1 the tip is image pixel (1,1) and the box starts at
	 * (10 - 1, 10 - 1), so the pointer's own pixel is where the tip is — the one coincidence that makes "the arrow
	 * points at what you are clicking" true. */
	check(at(&server, 10, 10) != BACKGROUND, "the point the pointer names has ink on it");

	/* 6. **A FRACTIONAL SCALE CHANGES THE BOX AND NOT THE SHAPE'S PROPORTIONS.** 1.33203 is the measured scale of a
	 * 96-dpi 1080p panel, and the case the raster exists for: a magnified 1x arrow next to device-resolution text is
	 * visibly blocky. The box ROUNDS and the hotspot with it, the same rule the composite uses. */
	finish(&server);
	reset(&server, 1.33203125, 96, 96);
	aws_cursor_ensure(&server);
	check(server.cursor.width == 16 && server.cursor.height == 25,
	      "the fractional scale rasterizes to 16x25 device pixels");
	check(server.cursor.hotspot_x == 1 && server.cursor.hotspot_y == 1,
	      "and the tip still rounds to one device pixel in");
	check(server.cursor.scale == 1.33203125, "the raster records the scale it was made for");
	aws_cursor_draw_cpu(&server);
	/* left = round(10 * 1.33203) - 1 = 12, and the tip is the top-left of the box. */
	check(at(&server, 0, 0) == BACKGROUND, "and it is placed on the pointer, not at the origin");
	check(box_has_two_tones(&server, 12, 12, 16, 25), "the fractional-scale pointer is drawn with both tones");

	/* 7. **A NEW SCALE RE-MAKES THE RASTER.** The image is cached per scale, so the cache being stale is the failure
	 * mode this rules out: a session that moved from 1.33 to 2 would otherwise draw a magnified 16-pixel arrow. */
	finish(&server);
	reset(&server, 2.0, 96, 96);
	aws_cursor_ensure(&server);
	check(server.cursor.width == 23 && server.cursor.height == 37,
	      "a 2x output makes the arrow 23x37 device pixels");

	/* 8. **THE BOX IS CLIPPED, NOT MOVED.** A pointer at the top-left corner has its hotspot off the output — the box
	 * starts at -1 — and the pixel that survives is the tip's, not the box's. And a pointer at the far corner draws
	 * only the part that is on the output, without writing past the framebuffer. */
	finish(&server);
	reset(&server, 1.0, 64, 64);
	server.pointer_x = 0;
	server.pointer_y = 0;
	aws_cursor_draw_cpu(&server);
	check(at(&server, 0, 0) != BACKGROUND, "a pointer in the corner still has its tip on the output");
	check(at(&server, 8, 8) != BACKGROUND, "and the body it can show is drawn");

	finish(&server);
	reset(&server, 1.0, 64, 64);
	server.pointer_x = 63;
	server.pointer_y = 63;
	aws_cursor_draw_cpu(&server);
	check(at(&server, 63, 63) != BACKGROUND, "a pointer at the far corner draws the part that fits");
	check(at(&server, 50, 50) == BACKGROUND, "and nothing in the frame it does not cover");

	/* 9. **AND ONE FRAME IS NOT THE NEXT ONE'S MEMORY.** The blend is source-over into the framebuffer, so drawing
	 * twice must not leave a darker ghost than drawing once: the same pixel answers the same colour. */
	finish(&server);
	reset(&server, 1.0, 64, 64);
	aws_cursor_draw_cpu(&server);
	{
		uint32_t once = at(&server, 11, 13);

		aws_cursor_draw_cpu(&server);
		check(at(&server, 11, 13) == once, "drawing the same pointer twice leaves the same pixel");
	}

	finish(&server);
	printf("\n%s\n", s_failed == 0 ? "all checks passed" : "FAILURES");
	return (s_failed == 0) ? 0 : 1;
}
