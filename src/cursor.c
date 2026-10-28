/*
 * cursor.c — the pointer the compositor puts under the pointer's position.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **THE PIXELS ARE THE THEME'S, AND THE THEME IS THE DESKTOP'S DESIGN.** This file used to own the arrow: an outline
 * in points, rasterized per scale. That was a compositor inventing its own pointer, and it is not what a cursor
 * theme is for — a desktop is designed with its pointers, and the arrow someone put in the theme is the one they
 * expect to see over their windows. So `aws_cursor_ensure` asks the theme (`cursor-theme.c`) for `left_ptr` at this
 * output's size, copies its pixels and keeps its hotspot, and the drawn arrow below is what happens when there is no
 * theme: a checkout without the vendored artwork still has a visible pointer, and that image is original to Agora.
 *
 * WHERE THE TWO DISPLAY PATHS GET IT. Both ask the same function. A DRM output has no host pointer, so the image is
 * composited above every surface — by `aws_cursor_draw_cpu` here, or by the GL renderer uploading `aws_cursor_image`
 * as a texture. A nested X11 output has a host pointer already, so the image is handed to the HOST through
 * `aws_cursor_theme_pointer`, which is the same variant for the same scale; the host draws it inside this process's
 * window, and no second pointer is composited there.
 *
 * **THE SHAPE THIS FILE DRAWS IS POINTS, NOT PIXELS.** An arrow rasterized on a 1x output and then magnified is a
 * blocky arrow next to device-resolution text, so the fallback outline below is in POINTS and the raster is made for
 * the output's own scale — the same reason the menu bar's mark is rasterized at the size it is drawn. The scale here
 * is FRACTIONAL (the measured 1.332 of a 96-dpi panel), and the device box and the hotspot both ROUND, because a
 * cursor that landed on a half pixel would be a cursor drawn twice.
 *
 * THE TWO-TONE LOOK IS TWO LAYERS, NOT TWO SHAPES. A black arrow alone disappears against a dark window and a white
 * outline alone is washed out on a light one, so the image is WHITE FILLED FIRST and BLACK DRAWN OVER IT at one
 * device pixel of separation: the white fringe is the arrow's own coverage dilated by one pixel, and the inside is
 * the arrow itself. Both layers carry antialiased coverage, so a diagonal edge is a blend and not a staircase.
 */

#include "server.h"
#include "cursor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* THE FALLBACK ARROW, IN POINTS: the tip, down the left edge, the notch, the tail's outer edge, the tail's end, back
 * up to the body, and out to the right shoulder. 11 x 18 points is the size a desktop pointer has been since forever;
 * what matters here is that the tip is INSIDE the first point, so the hotspot has room for its fringe. */
static const double s_arrow[] = {
	0.5, 0.5,
	0.5, 15.5,
	4.0, 12.0,
	6.5, 17.5,
	8.8, 16.4,
	6.2, 11.0,
	10.5, 11.0
};
#define AWS_ARROW_POINTS ((int)(sizeof s_arrow / sizeof s_arrow[0] / 2))
#define AWS_ARROW_WIDTH 11.0
#define AWS_ARROW_HEIGHT 18.0

/* SIXTEEN SAMPLES PER DEVICE PIXEL, which is enough for a smooth diagonal at one device pixel of edge and costs
 * nothing that matters: the raster is made once per scale and never again. */
#define AWS_CURSOR_SAMPLES 4

/* **A DEVICE EDGE ROUNDS, HALF AWAY FROM ZERO — THE SAME RULE THE COMPOSITE USES** (see `cpu_device_edge` in
 * renderer-cpu.c). Two renderers that rounded differently would put the pointer one pixel apart at a fractional
 * scale, which is exactly the kind of disagreement this module exists to prevent. */
static int cursor_device_edge(double value)
{
	return (int)(value >= 0.0 ? value + 0.5 : value - 0.5);
}

/* THE CROSSING TEST, on the arrow's own points. `y` selects the edges that straddle the sample row and the cross
 * product says which side of one the sample is on. */
static int cursor_point_inside(double x, double y)
{
	int inside = 0;
	int i, j;

	for (i = 0, j = AWS_ARROW_POINTS - 1; i < AWS_ARROW_POINTS; j = i++) {
		double xi = s_arrow[i * 2], yi = s_arrow[i * 2 + 1];
		double xj = s_arrow[j * 2], yj = s_arrow[j * 2 + 1];

		if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
			inside = !inside;
		}
	}
	return inside;
}

/* SOURCE OVER DESTINATION, BOTH STRAIGHT (NON-PREMULTIPLIED) ARGB. The two layers below are 8-bit coverage, so the
 * rounding is done once per channel rather than left to drift. */
static uint32_t cursor_over(uint32_t destination, uint32_t source, uint32_t alpha)
{
	uint32_t inverse = 255u - alpha;
	uint32_t da = destination >> 24;
	uint32_t out_alpha = alpha + (da * inverse + 127u) / 255u;
	uint32_t r, g, b;

	if (out_alpha == 0) {
		return 0;
	}
	r = (((source >> 16) & 0xFFu) * alpha + ((destination >> 16) & 0xFFu) * da * inverse / 255u) / out_alpha;
	g = (((source >> 8) & 0xFFu) * alpha + ((destination >> 8) & 0xFFu) * da * inverse / 255u) / out_alpha;
	b = ((source & 0xFFu) * alpha + (destination & 0xFFu) * da * inverse / 255u) / out_alpha;
	return (out_alpha << 24) | (r << 16) | (g << 8) | b;
}

/* THE FALLBACK'S RASTER, FOR ONE SCALE. Everything here is device pixels except the polygon, which is looked up in
 * points and divided by the scale at the sample: that is what makes the fringe one DEVICE pixel wide at every
 * scale. */
static void cursor_rasterize_arrow(struct aws_cursor *cursor, double scale)
{
	int width = (int)(AWS_ARROW_WIDTH * scale + 0.5) + 1;
	int height = (int)(AWS_ARROW_HEIGHT * scale + 0.5) + 1;
	unsigned char *fill;
	uint32_t *pixels;
	int row, column, sy, sx;

	if (width < 2 || height < 2) {
		return;
	}
	fill = calloc((size_t)width * (size_t)height, 1);
	pixels = calloc((size_t)width * (size_t)height, sizeof *pixels);
	if (fill == NULL || pixels == NULL) {
		free(fill);
		free(pixels);
		return;
	}
	for (row = 0; row < height; row++) {
		for (column = 0; column < width; column++) {
			unsigned inside = 0;

			for (sy = 0; sy < AWS_CURSOR_SAMPLES; sy++) {
				for (sx = 0; sx < AWS_CURSOR_SAMPLES; sx++) {
					double x = (column + ((double)sx + 0.5) / AWS_CURSOR_SAMPLES) / scale;
					double y = (row + ((double)sy + 0.5) / AWS_CURSOR_SAMPLES) / scale;

					if (cursor_point_inside(x, y)) {
						inside++;
					}
				}
			}
			fill[(size_t)row * (size_t)width + (size_t)column] =
				(unsigned char)(inside * 255u / (AWS_CURSOR_SAMPLES * AWS_CURSOR_SAMPLES));
		}
	}
	/* THE WHITE LAYER IS THE FILL, DILATED: for every pixel, the greatest coverage in its 3x3 neighbourhood. A
	 * dilation rather than a stroked outline because the two layers then share one shape by construction, and the
	 * fringe cannot pinch where the arrow does. */
	for (row = 0; row < height; row++) {
		for (column = 0; column < width; column++) {
			unsigned outline = 0;
			int dy, dx;

			for (dy = -1; dy <= 1; dy++) {
				for (dx = -1; dx <= 1; dx++) {
					int y = row + dy;
					int x = column + dx;
					unsigned value;

					if (y < 0 || y >= height || x < 0 || x >= width) {
						continue;
					}
					value = fill[(size_t)y * (size_t)width + (size_t)x];
					if (value > outline) {
						outline = value;
					}
				}
			}
			if (outline == 0) {
				continue;
			}
			pixels[(size_t)row * (size_t)width + (size_t)column] =
				cursor_over((outline << 24) | 0x00FFFFFFu, 0xFF000000u,
					    fill[(size_t)row * (size_t)width + (size_t)column]);
		}
	}
	free(fill);
	cursor->scale = scale;
	cursor->width = width;
	cursor->height = height;
	/* THE HOTSPOT IS THE TIP, IN DEVICE PIXELS — the same rounding as the box. */
	cursor->hotspot_x = cursor_device_edge(0.5 * scale);
	cursor->hotspot_y = cursor_device_edge(0.5 * scale);
	cursor->pixels = pixels;
}

/* **THE THEME'S IMAGE, COPIED.** A copy rather than a borrowed pointer because the theme's artwork can be closed
 * under a running server (a theme that changed, a session that ended) and the renderer may still be holding the
 * previous frame's image. A cursor is at most 64×64, so the copy costs nothing that matters; the pixels arrive as
 * straight alpha already, which is what every blend here expects. */
static void cursor_copy_image(struct aws_cursor *cursor, const struct aws_cursor_image *image, double scale)
{
	size_t count = (size_t)image->width * (size_t)image->height;
	uint32_t *pixels = malloc(count * sizeof *pixels);

	if (pixels == NULL) {
		return;
	}
	memcpy(pixels, image->pixels, count * sizeof *pixels);
	cursor->scale = scale;
	cursor->width = image->width;
	cursor->height = image->height;
	cursor->hotspot_x = image->hotspot_x;
	cursor->hotspot_y = image->hotspot_y;
	cursor->pixels = pixels;
}

/* **THE SHAPE IN FORCE, WHICH IS `agora_window.set_cursor`'s NAME.** Empty until a window names one, which is why the
 * default is answered here rather than written into the server at startup: a session with no client and a session
 * whose windows never asked draw the same arrow. */
const char *aws_cursor_shape(const struct aws_server *server)
{
	if (server == NULL || server->cursor_name[0] == '\0') {
		return AWS_CURSOR_ARROW_NAME;
	}
	return server->cursor_name;
}

/* THE BLANK CURSOR: `none`, as ONE TRANSPARENT PIXEL. It is an image rather than a flag so that nothing downstream has to
 * know about it — a transparent pixel blends to nothing in the CPU and GL paths, and an X cursor made from it is invisible
 * on the host — and it is one pixel because the pointer's damage box is its image, so hiding the pointer costs a pixel
 * and not a rectangle. */
static const uint32_t s_blank_pixels[1] = { 0u };
static const struct aws_cursor_image s_blank_image = {
	1,			/* nominal size */
	1,			/* width */
	1,			/* height */
	0,			/* hotspot x */
	0,			/* hotspot y */
	0,			/* delay ms */
	s_blank_pixels
};

/* ONE LOOKUP BY NAME, WITH THE TWO FALLBACKS THE PROTOCOL NAMES. `none` is the compositor's own blank pixel and never
 * reaches the theme; a name the theme does not have is the DEFAULT, which is a theme entry and not Agora's drawn arrow —
 * a theme that has `left_ptr` but not `help` can still show its own arrow. NULL is "there is no theme image at all", which
 * is what makes `aws_cursor_ensure` fall back to the arrow this file draws.
 *
 * `known` answers a different question from the image, and the diagnostic needs both: whether the THEME had the name that
 * was asked for. A cursor that resolved through the fallback still has an image, and a client whose name was quietly
 * replaced deserves to be told which of the two happened. NULL when nobody is asking. */
static const struct aws_cursor_image *cursor_lookup(struct aws_server *server, const char *name, double scale,
				    int *known)
{
	const struct aws_cursor_frames *frames;

	if (known != NULL) {
		*known = 0;
	}
	if (strcmp(name, AWS_CURSOR_NONE_NAME) == 0) {
		if (known != NULL) {
			*known = 1;
		}
		return &s_blank_image;
	}
	if (server->cursor_theme == NULL) {
		return NULL;
	}
	/* THE FIRST FRAME: the compositor shows one shape and an arrow does not animate. The rest are decoded and kept for the
	 * animated half of a theme — the X11 host cannot animate an X cursor at all — see cursor-theme.h. */
	frames = aws_cursor_theme_frames(server->cursor_theme, name, server->cursor_size * scale);
	if (frames != NULL && frames->count > 0) {
		if (known != NULL) {
			*known = 1;
		}
		return &frames->frames[0];
	}
	if (strcmp(name, AWS_CURSOR_ARROW_NAME) != 0) {
		frames = aws_cursor_theme_frames(server->cursor_theme, AWS_CURSOR_ARROW_NAME,
						 server->cursor_size * scale);
		if (frames != NULL && frames->count > 0) {
			return &frames->frames[0];
		}
	}
	return NULL;
}

/* **THE THEME'S POINTER FOR THIS OUTPUT, OR NULL WHEN THERE IS NOTHING TO ASK.** One place decides the size — the
 * cursor's points times the output's scale — so the composited pointer and the nested host's pointer are never a variant
 * apart, and one place decides the SHAPE — the name `agora_window.set_cursor` put in force. */
static const struct aws_cursor_image *cursor_shape_image(struct aws_server *server, double scale)
{
	return cursor_lookup(server, aws_cursor_shape(server), scale, NULL);
}

/* **AND THE TWO THINGS A SHAPE CHANGE COSTS.** The cached raster is invalidated by bumping the generation — the same door
 * a theme or a size change uses, so `aws_cursor_ensure` re-makes the image and a renderer's texture is re-uploaded — and
 * the picture is repainted, because on a backend that composites its own pointer the old shape's pixels are still on the
 * glass. The whole output is damaged rather than the pointer's boxes: the box arithmetic is a function of the image that
 * is being replaced, so "where it was" is not a rectangle this file can still compute. A shape change is a handful per
 * second at the very most, and the alternative is a stale arrow. */
void aws_cursor_set_shape(struct aws_server *server, const char *name)
{
	const char *wanted = (name != NULL && name[0] != '\0') ? name : AWS_CURSOR_ARROW_NAME;

	if (server == NULL || strcmp(server->cursor_name, wanted) == 0) {
		return;
	}
	if (strlen(wanted) >= sizeof server->cursor_name) {
		/* A NAME NOTHING COULD HAVE INSTALLED is treated as the default rather than truncated into a name that might
		 * exist; `aws_cursor_report_shape` is where that is said out loud, at the request that carried it. */
		return;
	}
	snprintf(server->cursor_name, sizeof server->cursor_name, "%s", wanted);
	server->cursor_generation++;
	if (server->backend.apply_cursor != NULL) {
		server->backend.apply_cursor(server);
	}
	/* ONLY A BACKEND THAT COMPOSITES THE POINTER NEEDS A FRAME: where a host draws it, `apply_cursor` above has already
	 * handed the host the new shape and a redraw would paint pixels nobody is waiting for. */
	if (server->backend.owns_cursor) {
		aws_server_schedule_render(server);
	}
}

void aws_cursor_report_shape(struct aws_server *server, int window_id, const char *name)
{
	const char *wanted = (name != NULL && name[0] != '\0') ? name : AWS_CURSOR_ARROW_NAME;
	const struct aws_cursor_image *image;
	double scale;
	int known = 0;

	if (server == NULL || getenv("AWS_CURSOR_DIAG") == NULL) {
		return;
	}
	if (strlen(wanted) >= sizeof server->cursor_name) {
		fprintf(stderr, "  DIAG cursor: window %d asked for a shape name longer than %d bytes; refused\n",
			window_id, (int)sizeof server->cursor_name - 1);
		return;
	}
	scale = server->backend.scale > 0.0 ? server->backend.scale : 1.0;
	image = cursor_lookup(server, wanted, scale, &known);
	if (strcmp(wanted, AWS_CURSOR_NONE_NAME) == 0) {
		fprintf(stderr, "  DIAG cursor: window %d asks for no pointer at all (`none`)\n", window_id);
	} else if (known && image != NULL) {
		fprintf(stderr, "  DIAG cursor: window %d asks for %s, which the theme has at %dx%d, hotspot (%d,%d)\n",
			window_id, wanted, image->width, image->height, image->hotspot_x, image->hotspot_y);
	} else if (image != NULL) {
		fprintf(stderr, "  DIAG cursor: window %d asks for %s, which the theme has not got — the default is shown\n",
			window_id, wanted);
	} else {
		fprintf(stderr, "  DIAG cursor: window %d asks for %s, and there is no theme to resolve it against\n",
			window_id, wanted);
	}
}

void aws_cursor_follow_focus(struct aws_server *server)
{
	struct aws_surface *focus;

	if (server == NULL) {
		return;
	}
	focus = server->pointer_focus;
	/* THE WINDOW'S OWN NAME, and the default when the pointer is over no window at all — a desk with no window under the
	 * hand has no shape to ask for, and the theme's arrow is what the compositor shows there. */
	aws_cursor_set_shape(server, focus != NULL ? focus->cursor_name : NULL);
}

int aws_cursor_visible(struct aws_server *server)
{
	return server->backend.owns_cursor && server->pointer_inside;
}

void aws_cursor_ensure(struct aws_server *server)
{
	double scale = server->backend.scale;
	const struct aws_cursor_image *image;

	if (scale <= 0.0) {
		scale = 1.0;
	}
	/* CACHED FOR THE SCALE AND FOR THE THEME SETTINGS, not for the scale alone: a pointer asked to be larger, or a
	 * theme changed under a running server, has to re-make the image even though the output did not move. */
	if (server->cursor.pixels != NULL && server->cursor.scale == scale &&
	    server->cursor.generation == server->cursor_generation) {
		return;
	}
	free(server->cursor.pixels);
	memset(&server->cursor, 0, sizeof server->cursor);
	image = cursor_shape_image(server, scale);
	if (image != NULL) {
		cursor_copy_image(&server->cursor, image, scale);
	}
	if (server->cursor.pixels == NULL) {
		cursor_rasterize_arrow(&server->cursor, scale);
	}
	/* **EVERY IMAGE MADE IS A NEW NUMBER, NOT THE SETTINGS' NUMBER.** A renderer caches the image as a texture and a
	 * change of SCALE re-makes it without any setting having changed — so the generation has to count images, not
	 * settings, or a GL cursor would keep the texture it uploaded at the old scale. Bumping the server's counter here
	 * is also what makes the cache test above hold afterwards (the two numbers are equal until something changes). */
	server->cursor.generation = ++server->cursor_generation;
}

const struct aws_cursor_image *aws_cursor_theme_pointer(struct aws_server *server)
{
	double scale = server->backend.scale;

	if (scale <= 0.0) {
		scale = 1.0;
	}
	return cursor_shape_image(server, scale);
}

const struct aws_cursor *aws_cursor_image(struct aws_server *server)
{
	if (!aws_cursor_visible(server)) {
		return NULL;
	}
	aws_cursor_ensure(server);
	return (server->cursor.pixels != NULL) ? &server->cursor : NULL;
}

void aws_cursor_draw_cpu(struct aws_server *server)
{
	aws_cursor_draw_cpu_box(server, NULL);
}

int aws_cursor_device_box(struct aws_server *server, int x, int y, struct aws_box *box)
{
	struct aws_backend *backend;
	int left, top;
	double scale;

	if (server == NULL || box == NULL || !server->backend.owns_cursor) {
		return 0;
	}
	/* **THE IMAGE, NOT "IS IT VISIBLE"** — the seat asks this for the position the pointer is LEAVING, where
	 * `pointer_inside` is already false and the image is still exactly what has to be painted over. */
	aws_cursor_ensure(server);
	if (server->cursor.pixels == NULL) {
		return 0;
	}
	backend = &server->backend;
	scale = backend->scale > 0.0 ? backend->scale : 1.0;
	left = cursor_device_edge((double)x * scale) - server->cursor.hotspot_x;
	top = cursor_device_edge((double)y * scale) - server->cursor.hotspot_y;
	box->x = left;
	box->y = top;
	box->width = server->cursor.width;
	box->height = server->cursor.height;
	return 1;
}

void aws_cursor_draw_cpu_box(struct aws_server *server, const struct aws_box *box)
{
	struct aws_backend *backend = &server->backend;
	const struct aws_cursor *cursor;
	struct aws_box placed;
	int left, top, row, column;

	if (!aws_cursor_visible(server)) {
		return;
	}
	/* **THE PLACEMENT IS `aws_cursor_device_box`'s, SO THE SEAT AND THE DRAW CANNOT DISAGREE.** The seat asks the same
	 * question about the position the pointer is leaving, and a second copy of this arithmetic is how "the frame repaints
	 * where the pointer was" would end up repainting a rectangle a hotspot away from it. */
	if (!aws_cursor_device_box(server, server->pointer_x, server->pointer_y, &placed)) {
		return;
	}
	cursor = &server->cursor;
	left = placed.x;
	top = placed.y;
	/* CLIPPED TO THE OUTPUT PER PIXEL rather than by shrinking the loop: the pointer may be at the very edge, and a
	 * cursor that vanished there would be a pointer that left before it did. */
	for (row = 0; row < cursor->height; row++) {
		int y = top + row;

		if (y < 0 || y >= backend->height) {
			continue;
		}
		/* **THE POINTER IS NOT EXEMPT FROM A PARTIAL FRAME.** Its pixels are blended, so painting them twice darkens
		 * the antialiased edge; a frame whose box misses the pointer entirely must paint none of it, and one that
		 * covers half of it paints that half. The rows and columns outside are already correct on the glass. */
		if (box != NULL && (y < box->y || y >= box->y + box->height)) {
			continue;
		}
		for (column = 0; column < cursor->width; column++) {
			uint32_t pixel;
			uint32_t alpha;
			int x = left + column;
			uint32_t *destination;

			if (x < 0 || x >= backend->width) {
				continue;
			}
			if (box != NULL && (x < box->x || x >= box->x + box->width)) {
				continue;
			}
			pixel = cursor->pixels[(size_t)row * (size_t)cursor->width + (size_t)column];
			alpha = pixel >> 24;
			if (alpha == 0) {
				continue;
			}
			destination = &backend->pixels[(size_t)y * (size_t)backend->width + (size_t)x];
			if (alpha == 255u) {
				*destination = pixel | 0xFF000000u;
			} else {
				*destination = cursor_over(*destination | 0xFF000000u, pixel, alpha);
			}
		}
	}
}

void aws_cursor_finish(struct aws_server *server)
{
	free(server->cursor.pixels);
	memset(&server->cursor, 0, sizeof server->cursor);
}
