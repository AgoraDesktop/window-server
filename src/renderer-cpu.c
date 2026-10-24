/*
 * renderer-cpu.c — the compositor that draws into a frame buffer.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * THIS IS THE RENDERER THE SERVER SHIPPED WITH, MOVED OUT OF `output.c` AND NOT REWRITTEN. It is the one every
 * backend can run — headless has nothing else, and DRM/KMS has no EGL path yet — so it is not a fallback in the
 * sense of being temporary; it is the reference picture the GL renderer is checked against.
 *
 * WHAT IT IS HONEST ABOUT: every pixel costs CPU, so a full-screen composite is milliseconds rather than a
 * triangle. That is why the row loops below are written the way they are — a transparent window skips four pixels
 * at a time, an opaque row is a copy, and a row clear of both corner bands asks about no shape at all. The
 * measurements that produced those loops are in `README.md`, and they are the reason a click no longer waits a
 * tenth of a second for its own shadow.
 *
 * WHAT IT DOES NOT DECIDE: which surfaces are drawn, in what order, or which of them casts a shadow. That is
 * `aws_output_walk` in `output.c`, and this file is called back from it — a second level walk here would be a
 * second answer to "does the menu sit above the window".
 */

#include "server.h"

#include <math.h>	/* sqrt — the one place a distance is needed */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* EVERY PIXEL, DRAWN BY THE CPU. The background is cleared first and the walk then paints the bar and the
 * surfaces over it, in the order the server decided on. */
static void cpu_draw(struct aws_server *server);

/* ---------------------------------------------------------------------------------------------------
 * the System menu's mark
 * --------------------------------------------------------------------------------------------------- */

/* THE BLEND IS PER PIXEL AND NOT A SHAPE TEST. A pixel at coverage 255 is the ink exactly; a pixel at 127 is half
 * ink and half bar. That is what keeps a diagonal or a curve from looking like a staircase at 16 pixels. */
static uint32_t aws_blend_pixel(uint32_t destination, uint32_t source, uint32_t alpha);

/* **THE BAR'S MARK IS DRAWN AT DEVICE RESOLUTION FROM A LOGICAL COVERAGE GRID.** The grid is
 * `AWS_SYSTEM_MENU_GLYPH_WIDTH` square because a mark is designed in logical pixels; at 2x each of its cells covers a
 * 2x2 block of device pixels, so the loop walks DEVICE pixels and asks the grid which cell that pixel belongs to. A
 * mark drawn one grid cell per device pixel would be half size in the corner of its own box, which is exactly the
 * "everything is tiny" this whole change exists to fix. */
static void aws_output_draw_system_menu(struct aws_backend *backend, struct aws_system_icon *icon, int scale)
{
	const uint8_t *coverage;
	int row, column;

	/* **A MARK WITH NO PATHS IS THE HAMBURGER.** Every current mark has paths; this remains the safety net for a
	 * generated table that names a mark without data, so the bar never draws a blank square. */
	if (icon == NULL || icon->paths == NULL || icon->path_count == 0) {
		icon = aws_system_icon_by_id(AWS_SYSTEM_MENU_ICON_FALLBACK);
	}
	coverage = aws_system_icon_coverage(icon);
	if (coverage == NULL) {
		return;
	}
	for (row = 0; row < AWS_SYSTEM_MENU_GLYPH_HEIGHT * scale; row++) {
		const int y = AWS_SYSTEM_MENU_GLYPH_TOP * scale + row;

		if (y < 0 || y >= backend->height) {
			continue;
		}
		for (column = 0; column < AWS_SYSTEM_MENU_GLYPH_WIDTH * scale; column++) {
			const int x = AWS_SYSTEM_MENU_LEFT_MARGIN * scale + column;
			const uint8_t alpha = coverage[(row / scale) * AWS_SYSTEM_ICON_PIXELS + (column / scale)];
			uint32_t *pixel;

			if (alpha == 0 || x < 0 || x >= backend->width) {
				continue;
			}
			pixel = &backend->pixels[(size_t)y * (size_t)backend->width + (size_t)x];
			if (alpha == 255u) {
				*pixel = AWS_SYSTEM_MENU_GLYPH_COLOUR;
			} else {
				*pixel = aws_blend_pixel(*pixel, AWS_SYSTEM_MENU_GLYPH_COLOUR, alpha);
			}
		}
	}
}

/* ============================================================================================ *
 * THE SHADOW, WHICH IS THE SERVER'S SHARE AND NOT THE CLIENT'S.                               *
 *                                                                                                *
 * `agora-shell.xml` says so in its own words — "the server's share of the Mac's model is the    *
 * shadow, the work area, and knowing who is in front" — and it is the right division: a shadow   *
 * falls on WHAT IS BEHIND THE WINDOW, and no client can see that. A client that drew its own     *
 * would be drawing over its neighbours, and one that drew it INSIDE its own frame would be       *
 * shrinking its content to make room for a lie.                                                  *
 *                                                                                                *
 * IT IS SEMITRANSPARENT, AND THAT IS THE WHOLE OF WHAT IT IS: the frame buffer is opaque, so     *
 * every pixel of shadow is a BLEND against whatever the compositor has already put there — the  *
 * same source-over arithmetic `composite_surface` does for a client's own alpha, which is why    *
 * the two share a shape and not a function.                                                      *
 *                                                                                                *
 * `AWS_SHADOW_DEPTH`, `AWS_SHADOW_ALPHA` and `AWS_SHADOW_RADIUS` live in `server.h` now, because  *
 * the GL renderer falls off over the same pixels and two renderers with two radii would be two    *
 * shadows.                                                                                       *
 * ============================================================================================ */

/* **SOURCE OVER, ON THE DESTINATION'S OWN BITS.** The frame buffer is opaque, so there is no alpha to carry and
 * the blend happens here or nowhere. One helper, because the row loop and the corner bands both need it. */
static uint32_t aws_blend_pixel(uint32_t destination, uint32_t source, uint32_t alpha)
{
	uint32_t inverse = 255u - alpha;
	uint32_t r = (((source >> 16) & 0xFFu) * alpha + ((destination >> 16) & 0xFFu) * inverse) / 255u;
	uint32_t g = (((source >> 8) & 0xFFu) * alpha + ((destination >> 8) & 0xFFu) * inverse) / 255u;
	uint32_t b = ((source & 0xFFu) * alpha + (destination & 0xFFu) * inverse) / 255u;

	return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* ONE PIXEL OF SHADOW: black, over what is already there, at `alpha` of 255. */
static uint32_t aws_shadow_pixel(uint32_t destination, uint32_t alpha)
{
	uint32_t inverse = 255u - alpha;
	uint32_t r = ((destination >> 16) & 0xFFu) * inverse / 255u;
	uint32_t g = ((destination >> 8) & 0xFFu) * inverse / 255u;
	uint32_t b = (destination & 0xFFu) * inverse / 255u;

	return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* ONE ROW'S SPAN OF SHADOW — see the ring note in `aws_output_draw_shadow`, which is the only caller and the
 * reason the spans exist. */
static void shadow_span(struct aws_backend *output, int row, int from, int to,
			int cx, int cy, int innerW, int innerH, int radius, int depth)
{
	int dy;
	int column;

	/* **THE BAR'S OWN ROWS ARE DEVICE PIXELS, so the boundary is scaled here.** A shadow must never be painted up
	 * across the bar, and the bar is `AWS_MENU_BAR_HEIGHT * scale` device rows tall. */
	if (row < AWS_MENU_BAR_HEIGHT * (output->scale > 0 ? output->scale : 1) || row >= output->height) {
		return;
	}
	if (from < 0) {
		from = 0;
	}
	if (to > output->width) {
		to = output->width;
	}
	dy = (row > cy ? row - cy : cy - row) - innerH;
	if (dy < 0) {
		dy = 0;
	}
	for (column = from; column < to; column++) {
		int dx, distance;
		double t, falloff;
		uint32_t alpha;

		dx = (column > cx ? column - cx : cx - column) - innerW;
		if (dx < 0) {
			dx = 0;
		}
		/* THE INTEGER SQUARE ROOT IS ENOUGH: a distance in pixels is the unit a falloff wants, and
		 * `sqrt` here would be the only double in the compositor's inner loop. */
		distance = (int)sqrt((double)(dx * dx + dy * dy)) - radius;
		if (distance <= 0 || distance > depth) {
			continue;	/* inside the frame, or past the shadow's reach */
		}
		t = 1.0 - (double)distance / (double)depth;
		falloff = t * t;
		alpha = (uint32_t)((double)AWS_SHADOW_ALPHA * falloff);
		if (alpha == 0) {
			continue;
		}
		output->pixels[(size_t)row * (size_t)output->width + (size_t)column] =
			aws_shadow_pixel(output->pixels[(size_t)row * (size_t)output->width + (size_t)column],
					 alpha);
	}
}

/* **AND THE SHADOW IS THE DISTANCE TO A ROUNDED RECTANGLE, WHICH IS WHY IT HAS ROUNDED CORNERS.** A rectangle's
 * shadow under a rounded window shows four square shoulders — the corners are exactly where a shadow is most
 * visible, because that is where it is widest. The signed distance to a rounded rectangle is three lines: clamp the
 * point into the rectangle's INNER box (the rectangle shrunk by the radius on every side), measure from there, and
 * take the radius off — negative inside the shape, positive outside, and the same expression at every corner. The
 * ALPHA then falls off as the square of the distance, which is what makes a hard edge read as a soft one. */
/* **`depth` AND `radius` ARRIVE IN DEVICE PIXELS; the caller scaled them from their logical constants.** The
 * falloff itself is unchanged — what a 2x output owes is a shadow that reaches as far, by eye, as a 1x one does. */
static void aws_output_draw_shadow(struct aws_backend *output, int x, int y, int w, int h, int radius, int depth)
{
	int left = x - depth, top = y - depth;
	int right = x + w + depth, bottom = y + h + depth;
	int halfW = w / 2, halfH = h / 2;
	int cx = x + halfW, cy = y + halfH;
	int innerW = halfW - radius, innerH = halfH - radius;
	int row, column;

	if (w <= 0 || h <= 0) {
		return;
	}
	for (row = top; row < bottom; row++) {
		/* **AND NOTHING BEHIND THE MENU BAR CASTS A SHADOW ONTO IT.** The bar is the server's own furniture and the
		 * topmost thing on the screen; a pop-down menu opens with its top edge AT the bar's bottom, so its shadow
		 * would otherwise paint UPWARDS across the strip — a dark band above every open menu, which reads as a dirty
		 * bar rather than as a shadow under a window. THE BAR'S OWN SHADOW FALLS BELOW THE STRIP and is unaffected by
		 * this line, because its rectangle begins at the strip's own bottom edge. */
		if (row < AWS_MENU_BAR_HEIGHT * (output->scale > 0 ? output->scale : 1) || row >= output->height) {
			continue;
		}
		/* **THE SHADOW IS A RING, AND A ROW THROUGH THE FRAME PAINTS ONLY ITS TWO SIDES.** Every pixel a shadow can
		 * reach is within `AWS_SHADOW_DEPTH` of the frame's edge — the falloff is zero past that — so the columns
		 * between the frame's two corner arcs cannot receive one: their horizontal distance is zero and the vertical
		 * one is at most the radius, which the radius takes off again. Walking them anyway was the first version,
		 * and it asked for a square root per pixel over the frame's WHOLE AREA: MEASURED, 10 ms of a 30 ms composite
		 * for a desktop-sized window, and the reason a click took a fifth of a second to appear. The two spans below
		 * are the whole ring. */
		{
			int hole0 = x + radius;
			int hole1 = x + w - radius;

			if (row >= y && row < y + h && hole1 > hole0) {
				shadow_span(output, row, left, hole0, cx, cy, innerW, innerH, radius, depth);
				shadow_span(output, row, hole1, right, cx, cy, innerW, innerH, radius, depth);
			} else {
				shadow_span(output, row, left, right, cx, cy, innerW, innerH, radius, depth);
			}
		}
	}
}

/* **THE WINDOW'S SHAPE, WHICH IS A ROUNDED RECTANGLE — AND THE COMPOSITOR IS THE ONLY ONE WHO CAN DRAW IT.** A
 * window is opaque on the wire (`XRGB8888`, no alpha, which is what keeps text sub-pixel-smoothed in the client), so
 * a client that rounded its own top corners could only PAINT a corner: the pixels outside the curve are still its
 * own, still opaque, and a square shoulder shows around the arc. MEASURED, that is exactly what a decorated window
 * looked like — "the rounding is sort of overlaid on a rectangle" — and the fix is here rather than in the client.
 *
 * IT IS THE SAME RADIUS THE SHADOW IS DRAWN WITH (`AWS_SHADOW_RADIUS`), which is what makes the two agree: a shadow
 * cut to six points around a window whose corners are six points is one shape; two radii would be two.
 *
 * AND A CORNER IS ROUNDED ONLY WHERE THE WINDOW IS NOT FLUSH WITH THE OUTPUT. A maximised window, and the menu bar
 * itself, span the screen's edges — rounding their corners would cut notches out of the desktop's own edge. Each of
 * the four is decided by the two edges that meet at it, so a window flush on one side keeps its two corners there
 * and its two free ones elsewhere.
 *
 * A SURFACE THAT LIVES IN THE MENU BAR IS NOT A WINDOW'S SHAPE AT ALL (`AWS_MENU_BAR_HEIGHT`, the test the shadow
 * uses): the bar is flush and square, and a status item is a piece of the bar, not a pane on the desktop.
 *
 * THE ANSWER IS A COVERAGE FROM 0 TO 255, not a yes or a no: a curve drawn on whole pixels is a staircase, so the
 * pixel the curve passes through is blended with what is behind it — the same source-over arithmetic a client's own
 * alpha goes through below. */
/* **THE SHAPE IS ASKED IN DEVICE PIXELS, BECAUSE THAT IS WHERE THE INK IS.** `width`, `height`, `column` and `row`
 * arrive multiplied by the output scale, and so do the surface's origin and its bar test; the RADIUS and the bar's
 * height are scaled here, once. A shape computed in logical units and then applied to device pixels would give a
 * corner half as round at 2x — a window that looks scaled up rather than one drawn large. */
static unsigned shape_coverage(const struct aws_backend *output, const struct aws_surface *surface,
			       int width, int height, int column, int row)
{
	int scale = (output->scale > 0) ? output->scale : 1;
	int radius = AWS_SHADOW_RADIUS * scale;
	int bar_height = AWS_MENU_BAR_HEIGHT * scale;
	int sx = surface->x * scale;
	int sy = surface->y * scale;
	int sw = surface->frame_width * scale;
	int sh = surface->frame_height * scale;
	int in_bar;
	int hangs_from_bar;
	double cx, cy;
	double outside;
	unsigned cover;

	if (radius <= 0) {
		return 255;
	}
	/* A surface entirely inside the bar's rows is part of the bar — square, and no shadow either. */
	in_bar = sy >= 0 && sy + sh <= bar_height;
	if (in_bar) {
		return 255;
	}
	/* **AND A MENU HANGS FROM THE BAR, SO ITS TOP CORNERS ARE NOT ITS OWN.** A dropdown's top edge IS the bar's
	 * bottom edge: rounding it would cut two notches out of the seam and, MEASURED, made a menu look pasted under the
	 * bar rather than hanging from it. Its BOTTOM corners are free and stay round, which is the shape a menu has. */
	hangs_from_bar = (surface->y >= 0) && (sy == bar_height);
	if (radius > width / 2) {
		radius = width / 2;
	}
	if (radius > height / 2) {
		radius = height / 2;
	}
	if (radius <= 0) {
		return 255;
	}
	if (column >= radius && column < width - radius) {
		return 255;			/* between the corners, whatever the row */
	}
	if (row >= radius && row < height - radius) {
		return 255;
	}
	/* WHICH CORNER, AND MAY IT BE ROUNDED AT ALL. */
	if (column < radius && row < radius) {
		if (!(sx > 0 && sy > 0) || hangs_from_bar) {
			return 255;
		}
		cx = radius;
		cy = radius;
	} else if (column >= width - radius && row < radius) {
		if (!(sx + width < output->width && sy > 0) || hangs_from_bar) {
			return 255;
		}
		cx = width - radius;
		cy = radius;
	} else if (column < radius && row >= height - radius) {
		if (!(sx > 0 && sy + height < output->height)) {
			return 255;
		}
		cx = radius;
		cy = height - radius;
	} else {
		if (!(sx + width < output->width && sy + height < output->height)) {
			return 255;
		}
		cx = width - radius;
		cy = height - radius;
	}
	/* **FROM THE PIXEL'S CENTRE, BECAUSE THAT IS WHAT A PIXEL IS.** A pixel at column `c` covers the square
	 * `[c, c+1]`, so its centre is `c + 0.5`; measuring from the corner instead biases the whole shape half a point
	 * toward the corner and clips a row that the curve does not reach — MEASURED, a client's frame at its own corner
	 * read three-quarters of its colour where the shape said "inside". */
	outside = (double)(column + 0.5 - cx) * (double)(column + 0.5 - cx)
		+ (double)(row + 0.5 - cy) * (double)(row + 0.5 - cy);
	outside = sqrt(outside) - (double)radius;
	/* FULLY INSIDE at half a point in, fully outside at half a point out, linear between — the whole of the
	 * antialiasing, and it is the same expression at every corner. */
	cover = (unsigned)((0.5 - outside) * 255.0);
	if (outside <= -0.5) {
		return 255;
	}
	if (outside >= 0.5) {
		return 0;
	}
	return cover;
}

/* **ONE LOGICAL PIXEL OUT OF A SCALED BUFFER'S BLOCK OF THEM.** A 2x client's buffer has four device pixels for every
 * logical pixel the compositor places, and the answer that keeps a glyph's stem as dark as it was drawn is the AVERAGE
 * of the block, not the first pixel of it — nearest-neighbour would drop three of every four pixels and turn text into
 * a broken pattern of stems.
 *
 * THE AVERAGING IS SAFE ON PREMULTIPLIED PIXELS, which is what a transparent window's buffer holds: averaging
 * premultiplied channels and the alpha together keeps them consistent, and un-premultiplying first would divide by an
 * alpha that may be zero.
 *
 * `XRGB8888`'s top byte is not alpha, so it is masked out of the sum and forced back on the way out — otherwise a
 * client that leaves junk there would average it into the colour. */
static uint32_t scaled_block_pixel(const uint8_t *base, size_t stride, int buffer_width, int buffer_height,
				   int sx, int sy, int scale, int transparent)
{
	unsigned sum_a = 0, sum_r = 0, sum_g = 0, sum_b = 0;
	unsigned count = 0;
	int dy, dx;

	for (dy = 0; dy < scale; dy++) {
		int y = sy + dy;
		const uint32_t *row;

		if (y >= buffer_height) {
			break;
		}
		row = (const uint32_t *)(const void *)(base + (size_t)y * stride);
		for (dx = 0; dx < scale; dx++) {
			int x = sx + dx;
			uint32_t pixel;

			if (x >= buffer_width) {
				break;
			}
			pixel = row[x];
			if (transparent) {
				sum_a += (pixel >> 24) & 0xFFu;
			}
			sum_r += (pixel >> 16) & 0xFFu;
			sum_g += (pixel >> 8) & 0xFFu;
			sum_b += pixel & 0xFFu;
			count++;
		}
	}
	if (count == 0) {
		return 0;
	}
	if (!transparent) {
		sum_a = 0xFFu * count;	/* an opaque buffer has no alpha to average */
	}
	return (((sum_a + count / 2) / count) << 24) | (((sum_r + count / 2) / count) << 16)
		| (((sum_g + count / 2) / count) << 8) | ((sum_b + count / 2) / count);
}

/* ONE CLIENT BUFFER ONTO THE OUTPUT, CLIPPED TO IT. A window half off the right edge is a normal state, not an
 * error, so the clipping is not an edge case — it is the loop.
 *
 * THE FIELDS COME FROM THE `wl_shm_buffer` EVERY TIME rather than from a copy kept on the surface: the buffer
 * belongs to the client, and a copy would be a second place for its size and format to be true. */
static void composite_surface(struct aws_backend *output, struct aws_surface *surface)
{
	struct wl_shm_buffer *shm = wl_shm_buffer_get(surface->current_buffer);
	const uint8_t *base;
	int buffer_width, buffer_height;
	int width, height, stride;
	int scale;			/* the CLIENT's device pixels per logical pixel */
	int out_scale;			/* the OUTPUT's — the compositor draws this many per logical pixel */
	int transparent;
	int corner;
	int row, column;

	if (shm == NULL) {
		return;
	}
	buffer_width = wl_shm_buffer_get_width(shm);
	buffer_height = wl_shm_buffer_get_height(shm);
	stride = wl_shm_buffer_get_stride(shm);
	/* **THE SURFACE IS THE BUFFER DIVIDED BY THE CLIENT'S SCALE.** Everything below — the frame clip, the corner
	 * radius, the destination row — is in LOGICAL pixels, because that is the space the compositor places windows
	 * in. With the scale at 1 (every client that says nothing) this is the same arithmetic it always was. */
	scale = surface->buffer_scale > 0 ? surface->buffer_scale : 1;
	out_scale = (output->scale > 0) ? output->scale : 1;
	width = buffer_width / scale;
	height = buffer_height / scale;
	/* **AND THE FRAME IS THE WINDOW'S BOUNDS, SO A BIGGER BUFFER IS CLIPPED TO IT.** MEASURED, not assumed: a
	 * status item's buffer is 320x200 while the server placed it in a 64x24 box, and without this the item ran
	 * 200 rows down the screen — a status item taller than the menu bar it is drawn in. The frame is what the
	 * server was told the window IS, so anything outside it is not part of the window. */
	if (surface->frame_width > 0 && width > surface->frame_width) {
		width = surface->frame_width;
	}
	if (surface->frame_height > 0 && height > surface->frame_height) {
		height = surface->frame_height;
	}
	transparent = wl_shm_buffer_get_format(shm) == WL_SHM_FORMAT_ARGB8888;
	/* **THE SHAPE IS ASKED FOR ONLY WHERE A SHAPE CAN DIFFER FROM A RECTANGLE.** `shape_coverage` answers 255 for
	 * every pixel that is not within the corner radius of a corner, and it says so itself — so asking it once per
	 * pixel bought a call and a distance for a number this loop already knows. The radius is clamped exactly as that
	 * function clamps it, so a surface too small to have a middle is asked about every pixel, as before. */
	corner = AWS_SHADOW_RADIUS;
	if (corner > width / 2) {
		corner = width / 2;
	}
	if (corner > height / 2) {
		corner = height / 2;
	}

	wl_shm_buffer_begin_access(shm);
	base = wl_shm_buffer_get_data(shm);
	if (base == NULL) {
		wl_shm_buffer_end_access(shm);
		return;
	}
	for (row = 0; row < height; row++) {
		int oy = surface->y + row;		/* 1:1 means device row == logical row */
		int first = 0;
		int last = width;
		const uint32_t *source;
		uint32_t *destination;

		if (oy < 0 || oy >= output->height) {
			continue;
		}
		/* THE ROW'S SPAN, CLIPPED TO THE OUTPUT ONCE rather than tested per pixel. */
		if (surface->x < 0) {
			first = -surface->x;
		}
		if (surface->x + last > output->width) {
			last = output->width - surface->x;
		}
		if (first >= last) {
			continue;
		}
		source = (const uint32_t *)(const void *)(base + (size_t)row * (size_t)stride);
		destination = output->pixels + (size_t)oy * (size_t)output->width + (size_t)surface->x;
		/* **A ROW BETWEEN THE CORNERS HAS ONE SHAPE FOR ALL OF ITS PIXELS, AND IT IS A RECTANGLE.** `shape_coverage`
		 * says so itself: a pixel is rounded only when it is within the radius of a VERTICAL edge AND of a HORIZONTAL
		 * one, so a row that is clear of both horizontal corner bands has no shape to ask about at all. Asking per
		 * pixel, and blending per pixel, is what made a full-screen composite cost 20 ms — MEASURED, and the reason a
		 * click with a wallpaper behind it waited a tenth of a second. THE THREE CASES BELOW ARE THE WHOLE ROW:
		 * an opaque one is a copy, a transparent one is mostly nothing, and only the corner bands need the shape. */
		if (out_scale == 1 && corner <= 0 || (out_scale == 1 && row >= corner && row < height - corner)) {
			if (!transparent) {
				/* AN OPAQUE ROW IS A COPY. The `X` in `XRGB8888` is not alpha and the output's is opaque. */
				for (column = first; column < last; column++) {
					destination[column] = source[column] | 0xFF000000u;
				}
				continue;
			}
			/* **AND A TRANSPARENT WINDOW IS MOSTLY NOTHING: four pixels are asked at once.** A desktop icon
			 * window is a handful of glyphs on an empty buffer, so the zero runs are most of every row and the cost
			 * of this surface should be the handful. */
			column = first;
			while (column < last) {
				if (column + 4 <= last) {
					uint64_t pair0, pair1;

					memcpy(&pair0, source + column, sizeof pair0);
					memcpy(&pair1, source + column + 2, sizeof pair1);
					if ((pair0 | pair1) == 0) {
						column += 4;
						continue;
					}
				}
				{
					uint32_t pixel = source[column];
					uint32_t alpha = pixel >> 24;

					if (alpha != 0) {
						uint32_t *out = destination + column;

						if (alpha == 0xFFu) {
							*out = pixel | 0xFF000000u;
						} else {
							*out = aws_blend_pixel(*out, pixel, alpha);
						}
					}
				}
				column++;
			}
			continue;
		}
		for (column = first; column < last; column++) {
			uint32_t pixel = source[column];
			unsigned shape;
			uint32_t alpha;

			/* **THE WINDOW'S OWN SHAPE FIRST, WHICH IS A ROUNDED CORNER OR NOTHING AT ALL.** `shape` is the
			 * corner's coverage: 255 everywhere but the four arcs, where it is the pixel the curve passes
			 * through, blended below like any other partial alpha. Only the corner bands get here. */
			shape = shape_coverage(output, surface, width, height, column, row);
			if (shape == 0) {
				continue;
			}
			alpha = transparent ? (pixel >> 24) : 0xFFu;
			if (shape != 255) {
				alpha = (alpha * shape + 127u) / 255u;
			}
			if (alpha == 0) {
				continue;
			}
			if (alpha != 0xFFu) {
				destination[column] = aws_blend_pixel(destination[column], pixel, alpha);
			} else {
				destination[column] = pixel | 0xFF000000u;
			}
		}
	}

	/* **AND ANYTHING THAT IS NOT 1:1 TAKES ONE GENERAL PATH.** The loops above are written in buffer pixels because
	 * almost every client is 1:1 with a 1x output, and the fast paths there — an opaque row is a copy, four
	 * transparent pixels are asked at once — are most of what makes a composite cheap. Every one of them assumes
	 * source and destination advance together, which stops being true the moment either factor is not 1:
	 *
	 *   the client says 2  the buffer has 2x2 device pixels per logical pixel, and the answer is their AVERAGE —
	 *                      nearest-neighbour would drop three of every four and turn a stem into a broken pattern.
	 *   the output is 2    every logical pixel becomes a 2x2 block of device pixels, filled with that same colour.
	 *
	 * THE TWO COMBINE, and that is the whole table: 2/2 is a 1:1 copy (the crisp HiDPI case), 2/1 is the downsample
	 * that makes a scaled client fit an unscaled output, 1/2 is the upscale that makes an ordinary client the right
	 * SIZE on a HiDPI screen. Only the middle of that is a copy, so the loop is written as "one logical pixel, its
	 * colour, then however many device pixels it covers". */
	if (out_scale != 1 || scale != 1) {
		for (row = 0; row < height; row++) {			/* logical */
			int oy0 = (surface->y + row) * out_scale;
			int first = 0;
			int last = width;
			int dy;

			if (oy0 + out_scale <= 0 || oy0 >= output->height) {
				continue;
			}
			if (surface->x < 0) {
				first = -surface->x;
			}
			/* THE OUTPUT'S LOGICAL WIDTH, which is its device width divided by the same factor: a window clipped
			 * at the right edge is clipped at the LOGICAL edge, and the conversion to device columns happens per
			 * pixel below. */
			if (surface->x + last > output->width / out_scale) {
				last = output->width / out_scale - surface->x;
			}
			for (column = first; column < last; column++) {
				uint32_t pixel = (scale > 1)
					? scaled_block_pixel(base, (size_t)stride, buffer_width, buffer_height,
							     column * scale, row * scale, scale, transparent)
					: ((const uint32_t *)(const void *)(base + (size_t)(row * scale) * (size_t)stride))[column * scale];
				int dx0 = (surface->x + column) * out_scale;
				int dx;

				for (dy = 0; dy < out_scale; dy++) {
					int py = oy0 + dy;

					if (py < 0 || py >= output->height) {
						continue;
					}
					for (dx = 0; dx < out_scale; dx++) {
						int px = dx0 + dx;
						unsigned shape;
						uint32_t alpha;
						uint32_t *destination;

						if (px < 0 || px >= output->width) {
							continue;
						}
						/* THE SHAPE IS ASKED PER DEVICE PIXEL, in device space — a rounded corner is a curve, and
						 * sampling it once per logical pixel would give the block a stair-step two physical pixels
						 * wide. */
						shape = shape_coverage(output, surface, width * out_scale, height * out_scale,
								       column * out_scale + dx, row * out_scale + dy);
						if (shape == 0) {
							continue;
						}
						alpha = transparent ? (pixel >> 24) : 0xFFu;
						if (shape != 255) {
							alpha = (alpha * shape + 127u) / 255u;
						}
						if (alpha == 0) {
							continue;
						}
						destination = &output->pixels[(size_t)py * (size_t)output->width + (size_t)px];
						if (alpha != 0xFFu) {
							*destination = aws_blend_pixel(*destination, pixel, alpha);
						} else {
							*destination = pixel | 0xFF000000u;
						}
					}
				}
			}
		}
	}
	wl_shm_buffer_end_access(shm);
}

/* **THE BAR, DRAWN AT ITS OWN LEVEL** — the strip's fill, the SYSTEM menu at its left, and its shadow below.
 * Called from the level walk at `AWS_MENU_BAR_LEVEL`, so the desktop and ordinary windows are painted first and the
 * application's own menu window (at the same level, later in the list) is over it. */
static void aws_output_draw_menu_bar(struct aws_server *server)
{
	struct aws_backend *backend = &server->backend;
	int row, column;

	int scale = aws_output_scale(server);
	int bar_height = AWS_MENU_BAR_HEIGHT * scale;

	for (row = 0; row < bar_height && row < backend->height; row++) {
		for (column = 0; column < backend->width; column++) {
			backend->pixels[row * backend->width + column] = AWS_MENU_BAR_COLOUR;
		}
	}
	aws_output_draw_system_menu(backend, server->system_menu_icon, scale);
	/* **AND THE BAR'S OWN SHADOW, WHICH FALLS BELOW IT ON WHATEVER IS UNDER IT.** The strip's rectangle is expanded
	 * and the falloff computed the same way, and the sides that run off the top, the left and the right are dropped
	 * by the bounds check — and `aws_output_draw_shadow` stops at the bar's bottom edge, so a pop-down menu's shadow
	 * is never painted up across the bar. */
	aws_output_draw_shadow(backend, 0, 0, backend->width, bar_height, 0, AWS_SHADOW_DEPTH * scale);
}

/* ---------------------------------------------------------------------------------------------------
 * the renderer's two entries: one frame, and the callback the walk calls
 * --------------------------------------------------------------------------------------------------- */

/* **THE SHADOW GOES DOWN BEFORE THE WINDOW DOES**, which is the only order that means anything: it is a shadow ON
 * THE THINGS BEHIND, so anything painted after it would be painted over it.
 *
 * AND NOTHING THAT LIVES IN THE MENU BAR CASTS ONE — ONE RULE, TWO CASES. A STATUS ITEM (`NSStatusItem`) and THE
 * APPLICATION'S OWN MENU TITLES are different in every other way and identical here: both lie entirely inside the
 * bar's rows, and a shadow around either is a dark smudge INSIDE the bar. So the test is the GEOMETRY and not the
 * kind — the first version asked `status_length`, which caught the item and let the menu titles through. */
static void cpu_emit(struct aws_server *server, struct aws_surface *surface, uint32_t time)
{
	struct aws_backend *backend = &server->backend;

	if (surface == NULL) {
		aws_output_draw_menu_bar(server);
		return;
	}
	{
		/* **THE SHADOW IS PLACED IN DEVICE PIXELS AND ITS BUDGETS ARE LOGICAL.** A window's frame is logical, so the
		 * rectangle and the reach are multiplied here — once, at the boundary — and everything inside the shadow's
		 * own loop is device pixels. The bar test is logical too, because a window that lives in the bar is a fact
		 * about where the client put it, not about how many pixels the screen has. */
		int scale = aws_output_scale(server);

		if (surface->y < 0 || surface->y + surface->frame_height > AWS_MENU_BAR_HEIGHT) {
			aws_output_draw_shadow(backend, surface->x * scale, surface->y * scale,
					       surface->frame_width * scale, surface->frame_height * scale,
					       AWS_SHADOW_RADIUS * scale, AWS_SHADOW_DEPTH * scale);
		}
	}
	composite_surface(backend, surface);
	/* AND THE FRAME CALLBACK GOES WITH THE COMPOSITE, NOT WITH THE COMMIT: the client may draw again only once its
	 * picture is on screen. */
	aws_surface_frame_done(surface, time);
}

static void cpu_draw(struct aws_server *server)
{
	struct aws_backend *backend = &server->backend;
	size_t count = (size_t)backend->width * (size_t)backend->height;
	size_t i;

	for (i = 0; i < count; i++) {
		backend->pixels[i] = AWS_BACKGROUND;
	}
	aws_output_walk(server, cpu_emit);
}

int aws_renderer_cpu_create(struct aws_server *server)
{
	struct aws_renderer *renderer = calloc(1, sizeof *renderer);

	if (renderer == NULL) {
		return -1;
	}
	renderer->name = "cpu";
	renderer->draw = cpu_draw;
	renderer->stop = NULL;
	renderer->data = NULL;
	server->renderer = renderer;
	return 0;
}
