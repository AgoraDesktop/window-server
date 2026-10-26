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

#include <pixman.h>

#include <math.h>	/* sqrt — the one place a distance is needed */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* EVERY PIXEL, DRAWN BY THE CPU. The background is cleared first and the walk then paints the bar and the
 * surfaces over it, in the order the server decided on. */
static void cpu_draw(struct aws_server *server, const pixman_region32_t *damage);

/* ---------------------------------------------------------------------------------------------------
 * the System menu's mark
 * --------------------------------------------------------------------------------------------------- */

/* THE BLEND IS PER PIXEL AND NOT A SHAPE TEST. A pixel at coverage 255 is the ink exactly; a pixel at 127 is half
 * ink and half bar. That is what keeps a diagonal or a curve from looking like a staircase at 16 pixels. */
static uint32_t aws_blend_pixel(uint32_t destination, uint32_t source, uint32_t alpha);

/* **A LOGICAL EDGE TO A DEVICE EDGE, AND IT MUST BE THE EDGE THAT ROUNDS.** At a fractional scale there is no whole
 * number of device pixels in a logical one, so the only stable rule is to round the ABSOLUTE coordinate: two windows
 * that share a logical edge then share a device edge too, and no seam opens between them. Rounding a WIDTH instead
 * would put the seam wherever the two windows'"'"' own widths happened to fall. */
static int cpu_device_edge(int logical, double scale)
{
	return (int)((double)logical * scale + 0.5);
}

/* **THE BAR'S MARK, RASTERIZED AT THE SCALE IT IS DRAWN AT.** The box is a 16-point square of the bar, so a scaled
 * output asks for `round(16 * scale)` device pixels of mark — 16 at 1x, 21 at the 96 dpi of a 1080p panel, 32 at 2x
 * — and the grid answered is that big, because a mark that magnified a 16×16 image next to device-resolution text is
 * visibly blocky: its strokes come out one or two pixels wide with no pattern. The mark gains detail at a higher
 * scale for the same reason a text run does, and the loop below is a straight 1:1 copy of device pixels.
 *
 * **THE MARK IS SQUARE, SO ONE SIDE IS THE BOX.** `AWS_SYSTEM_MENU_GLYPH_WIDTH` and `_HEIGHT` are the same number and
 * the raster is square; asking for two sides that could differ would be a grid that cannot exist. The assert keeps a
 * future edit from making it a lie. */
_Static_assert(AWS_SYSTEM_MENU_GLYPH_WIDTH == AWS_SYSTEM_MENU_GLYPH_HEIGHT,
	       "the System menu's mark is a square box, and its coverage grid is square too");

static void aws_output_draw_system_menu(struct aws_backend *backend, struct aws_system_icon *icon, double scale,
					const struct aws_box *clip)
{
	const uint8_t *coverage;
	int row, column;

	/* **A MARK WITH NO PATHS IS THE HAMBURGER.** Every current mark has paths; this remains the safety net for a
	 * generated table that names a mark without data, so the bar never draws a blank square. */
	if (icon == NULL || icon->paths == NULL || icon->path_count == 0) {
		icon = aws_system_icon_by_id(AWS_SYSTEM_MENU_ICON_FALLBACK);
	}
	/* THE MARK'S BOX IS THE LOGICAL BOX SCALED AND ROUNDED — the rule every other device edge here uses, so the mark
	 * and the bar's own rows round together at a fractional scale. */
	int glyph = cpu_device_edge(AWS_SYSTEM_MENU_GLYPH_WIDTH, scale);
	int glyph_top = cpu_device_edge(AWS_SYSTEM_MENU_GLYPH_TOP, scale);
	int glyph_left = cpu_device_edge(AWS_SYSTEM_MENU_LEFT_MARGIN, scale);
	int grid;

	if (glyph < 1) {
		return;
	}
	/* AND THE GRID IS THE BOX, UP TO THE CEILING A SCALE IS ALLOWED TO ASK FOR. Past it the mark is asked for at the
	 * ceiling and mapped onto the box — a soft mark at a scale no panel has, rather than a quarter megabyte per icon
	 * per nonsense. */
	grid = glyph < AWS_SYSTEM_ICON_MAX_PIXELS ? glyph : AWS_SYSTEM_ICON_MAX_PIXELS;
	coverage = aws_system_icon_coverage_at(icon, grid);
	if (coverage == NULL) {
		return;
	}
	for (row = 0; row < glyph; row++) {
		const int y = glyph_top + row;
		const int gy = row * grid / glyph;	/* identity whenever the grid is the box, which is every real scale */

		if (y < 0 || y >= backend->height) {
			continue;
		}
		if (clip != NULL && (y < clip->y || y >= clip->y + clip->height)) {
			continue;
		}
		for (column = 0; column < glyph; column++) {
			const int x = glyph_left + column;
			const int gx = column * grid / glyph;
			const uint8_t alpha = coverage[gy * grid + gx];
			uint32_t *pixel;

			if (alpha == 0 || x < 0 || x >= backend->width) {
				continue;
			}
			/* **A MARK IS BLENDED, SO IT IS NOT IDEMPOTENT AND THE BOX HAS TO CLIP IT.** Painting the same antialiased
			 * edge twice darkens it; a partial frame that touches the bar's rows anywhere must paint the mark only where the
			 * box says. */
			if (clip != NULL && (x < clip->x || x >= clip->x + clip->width)) {
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
 * reason the spans exist. **`clip_top` IS THE FIRST ROW A SHADOW MAY PAINT IN, in DEVICE pixels**, and the caller is
 * the one that knows which rules apply: the menu bar's rows for every window, and a sheet's own top edge as well. */
static void shadow_span(struct aws_backend *output, int row, int from, int to,
			int cx, int cy, int innerW, int innerH, int radius, int depth, int clip_top,
			const struct aws_box *clip)
{
	int dy;
	int column;

	if (row < clip_top || row >= output->height) {
		return;
	}
	/* **AND THE FRAME'S OWN BOX, WHICH IS A PARTIAL FRAME'S ONLY OTHER RULE.** A shadow is painted on what is behind the
	 * window, and on a frame that repaints one strip the shadow's rows outside that strip are not this frame's to
	 * touch — they already hold the shadow, drawn by the frame that put the window there. */
	if (clip != NULL && (row < clip->y || row >= clip->y + clip->height)) {
		return;
	}
	if (from < 0) {
		from = 0;
	}
	if (to > output->width) {
		to = output->width;
	}
	if (clip != NULL) {
		if (from < clip->x) {
			from = clip->x;
		}
		if (to > clip->x + clip->width) {
			to = clip->x + clip->width;
		}
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
 * falloff itself is unchanged — what a 2x output owes is a shadow that reaches as far, by eye, as a 1x one does.
 *
 * **`clip_top` IS "NOTHING MAY BE PAINTED ABOVE THIS ROW", ALSO IN DEVICE PIXELS.** Two rules use it and they are the
 * same sentence about different seams: nothing casts a shadow up across the MENU BAR (the bar is the server's own
 * furniture and the topmost thing on the screen), and a SHEET casts nothing up across its parent's title bar — a
 * sheet's top edge is that bar's bottom edge, so a shadow above it is a dark band under a title bar that is supposed
 * to be a seam rather than a ledge. */
static void aws_output_draw_shadow(struct aws_backend *output, int x, int y, int w, int h, int radius, int depth,
				   int clip_top, const struct aws_box *clip)
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
		/* **AND NOTHING BEHIND THE MENU BAR — OR BEHIND A SHEET'S PARENT TITLE BAR — CASTS A SHADOW ONTO IT.** The bar
		 * is the server's own furniture and the topmost thing on the screen; a pop-down menu opens with its top edge AT
		 * the bar's bottom, so its shadow would otherwise paint UPWARDS across the strip — a dark band above every open
		 * menu, which reads as a dirty bar rather than as a shadow under a window. THE BAR'S OWN SHADOW FALLS BELOW THE
		 * STRIP and is unaffected by this line, because its rectangle begins at the strip's own bottom edge. The sheet's
		 * half of the rule is `clip_top`'s, computed by the caller from the surface. */
		if (row < clip_top || row >= output->height) {
			continue;
		}
		if (clip != NULL && (row < clip->y || row >= clip->y + clip->height)) {
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
				shadow_span(output, row, left, hole0, cx, cy, innerW, innerH, radius, depth, clip_top,
					    clip);
				shadow_span(output, row, hole1, right, cx, cy, innerW, innerH, radius, depth, clip_top,
					    clip);
			} else {
				shadow_span(output, row, left, right, cx, cy, innerW, innerH, radius, depth, clip_top,
					    clip);
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
	double scale = (output->scale > 0) ? output->scale : 1.0;
	int radius = cpu_device_edge(AWS_SHADOW_RADIUS, scale);
	int bar_height = cpu_device_edge(AWS_MENU_BAR_HEIGHT, scale);
	int sx = cpu_device_edge(surface->x, scale);
	int sy = cpu_device_edge(surface->y, scale);
	int sw = cpu_device_edge(surface->frame_width, scale);
	int sh = cpu_device_edge(surface->frame_height, scale);
	int in_bar;
	int hangs_from_bar;
	/* **AND A SHEET'S TOP EDGE IS ITS PARENT'S TITLE BAR, WHICH IS THE SAME KIND OF SEAM.** The bar rule above is
	 * geometry, because a menu's top edge and the menu bar have one number between them. A sheet's does not: the
	 * compositor draws no title bar (the AppKit does), and two rounded rectangles whose edges meet look exactly like
	 * two that belong to each other — so the application SAYS so (`set_sheet_parent`) and this is the one bit of it the
	 * shape needs. Its top corners are square for the reason a menu's are: rounding them cuts notches out of the seam
	 * it is hanging from, and its bottom corners stay round, which is the shape a sheet has. */
	int hangs_from_titlebar;
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
	/* **EXACT, IN DEVICE TERMS: the surface's top edge IS the bar's bottom edge.** Written as a rounded comparison
	 * rather than a fuzzy one because a fuzz of half a device pixel already catches a surface that is INSIDE the bar
	 * — and one row inside the bar is a status item, whose corners are the bar's business and must stay square. */
	hangs_from_bar = (surface->y >= 0) && (cpu_device_edge(surface->y, scale) == bar_height);
	hangs_from_titlebar = hangs_from_bar || (surface->sheet_parent != NULL);
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
		if (!(sx > 0 && sy > 0) || hangs_from_titlebar) {
			return 255;
		}
		cx = radius;
		cy = radius;
	} else if (column >= width - radius && row < radius) {
		if (!(sx + width < output->width && sy > 0) || hangs_from_titlebar) {
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

/* **A DEVICE PIXEL OUT OF HOWEVER MUCH BUFFER IT COVERS.** The factor between the two spaces is
 * `buffer_scale / output_scale`, and it is not necessarily an integer: a 2x client on a 1.5x output covers its device
 * pixels with three quarters of a buffer pixel each, and a 1x client on the same output is magnified by 1.5. So the
 * question is asked the only way that works for every ratio — "which buffer pixels does this device pixel touch" — and
 * the answer is their AVERAGE, weighted equally.
 *
 * THE AVERAGE, NOT THE NEAREST: at any ratio above 1 a nearest-neighbour sample drops buffer pixels, and a glyph stem
 * that falls between two devices becomes a broken pattern rather than a thin line. The cost is a few reads per device
 * pixel, which is why the 1:1 case keeps its own loop above.
 *
 * THE AVERAGING IS SAFE ON PREMULTIPLIED PIXELS, which is what a transparent window's buffer holds: averaging
 * premultiplied channels and the alpha together keeps them consistent. `XRGB8888`'s top byte is not alpha, so it is
 * masked out of the sum and forced back on the way out.
 *
 * A BOX THAT FALLS PARTLY OUTSIDE THE BUFFER IS CLIPPED, not padded: a surface at the edge of the screen has no pixels
 * beyond its own edge, and inventing black ones would darken every window that touches the border. */
static uint32_t scaled_block_pixel(const uint8_t *base, size_t stride, int buffer_width, int buffer_height,
				   double bx0, double bx1, double by0, double by1, int transparent)
{
	int x0 = (int)bx0, x1 = (int)bx1, y0 = (int)by0, y1 = (int)by1;
	unsigned sum_a = 0, sum_r = 0, sum_g = 0, sum_b = 0;
	unsigned count = 0;
	int y, x;

	/* A BOX NARROWER THAN A PIXEL STILL COVERS ONE (an upscaled output), and one wider covers all of them. */
	if (x1 <= x0) {
		x1 = x0 + 1;
	}
	if (y1 <= y0) {
		y1 = y0 + 1;
	}
	if (x0 < 0) {
		x0 = 0;
	}
	if (y0 < 0) {
		y0 = 0;
	}
	if (x1 > buffer_width) {
		x1 = buffer_width;
	}
	if (y1 > buffer_height) {
		y1 = buffer_height;
	}

	for (y = y0; y < y1; y++) {
		const uint32_t *row = (const uint32_t *)(const void *)(base + (size_t)y * stride);

		for (x = x0; x < x1; x++) {
			uint32_t pixel = row[x];

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
static void composite_surface(struct aws_backend *output, struct aws_surface *surface, const struct aws_box *clip)
{
	struct wl_shm_buffer *shm = wl_shm_buffer_get(surface->current_buffer);
	const uint8_t *base;
	int buffer_width, buffer_height;
	int width, height, stride;
	double client_scale;		/* the same number, and it does not have to be whole: 1.333, 0.805 */
	double out_scale;		/* the OUTPUT's — the compositor draws this many device pixels per logical one, and
					 * it is not necessarily whole: 1.5 is a real configuration */
	int transparent;
	int corner;
	int row, column;
	int ratio_one;		/* the buffer is on the device grid: a 1:1 copy */
	int dx0 = 0, dy0 = 0;	/* the surface's DEVICE origin */
	int copy_w = 0, copy_h = 0;	/* the DEVICE extent to copy, after the frame clip */
	int copy_corner = 0;		/* the corner radius in DEVICE pixels */

	if (shm == NULL) {
		return;
	}
	buffer_width = wl_shm_buffer_get_width(shm);
	buffer_height = wl_shm_buffer_get_height(shm);
	stride = wl_shm_buffer_get_stride(shm);
	/* **THE SURFACE IS THE BUFFER DIVIDED BY THE CLIENT'S SCALE.** Everything below — the frame clip, the corner
	 * radius, the destination row — is in LOGICAL pixels, because that is the space the compositor places windows
	 * in. With the scale at 1 (every client that says nothing) this is the same arithmetic it always was. */
	client_scale = surface->buffer_scale > 0.0 ? surface->buffer_scale : 1.0;
	out_scale = (output->scale > 0.0) ? output->scale : 1.0;
	width = (int)((double)buffer_width / client_scale + 0.5);
	height = (int)((double)buffer_height / client_scale + 0.5);
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
	/* **WHETHER THE DRAW IS 1:1, AND WHERE IT LANDS IN DEVICE PIXELS.** A device-grid client's `buffer_scale` equals
	 * the output's, which makes the ratio exactly one; the copy extent is then the buffer's (clipped to the frame's
	 * device extent), and the origin is the rounded placement. At 1x all three collapse to the logical numbers. */
	ratio_one = (client_scale == out_scale);
	{
		int span_w = cpu_device_edge(width, out_scale);
		int span_h = cpu_device_edge(height, out_scale);

		dx0 = cpu_device_edge(surface->x, out_scale);
		dy0 = cpu_device_edge(surface->y, out_scale);
		copy_w = width;
		copy_h = height;
		copy_corner = corner;
		if (ratio_one && client_scale != 1.0) {
			if (span_w > buffer_width) {
				span_w = buffer_width;
			}
			if (span_h > buffer_height) {
				span_h = buffer_height;
			}
			copy_w = span_w;
			copy_h = span_h;
			copy_corner = cpu_device_edge(AWS_SHADOW_RADIUS, out_scale);
			if (copy_corner > copy_w / 2) {
				copy_corner = copy_w / 2;
			}
			if (copy_corner > copy_h / 2) {
				copy_corner = copy_h / 2;
			}
		}
	}

	wl_shm_buffer_begin_access(shm);
	base = wl_shm_buffer_get_data(shm);
	if (base == NULL) {
		wl_shm_buffer_end_access(shm);
		return;
	}
	/* **A 1:1 DRAW IS A COPY, AND THAT IS TRUE AT EVERY SCALE, NOT ONLY AT 1x.** The client that declared the
	 * output's own scale (`agora_window.set_buffer_scale_fixed`) rasterised on the device grid, so its buffer pixel IS
	 * the device pixel it belongs at: the right work is a row copy at the surface's DEVICE origin, with the same shape
	 * blending at the corners. Before this, only the literal 1x/1x pair took the fast loops and every 2x or 1.332x
	 * device-grid surface fell into the general per-pixel box — a box of exactly one pixel, computed with two
	 * divisions and a coverage test for every pixel of the window. MEASURED: that is the difference between a 4K
	 * desktop being composited and being a slideshow.
	 *
	 * `copy_w`/`copy_h` are DEVICE extents here, and `dx0`/`dy0` the device origin. For the 1x case they are the
	 * logical numbers unchanged, which is why this is the same loop it always was when the scale is 1. */
	for (row = 0; ratio_one && row < copy_h; row++) {
		int oy = dy0 + row;
		int first = 0;
		int last = copy_w;
		const uint32_t *source;
		uint32_t *destination;

		if (oy < 0 || oy >= output->height) {
			continue;
		}
		/* **AND THE FRAME'S BOX, WHICH IS THE WHOLE OF WHAT A PARTIAL FRAME IS.** A row outside it is not this frame's to
		 * touch — it holds the window's pixels from the frame that put them there, and repainting them here would be the
		 * whole-window cost this work exists to avoid. */
		if (clip != NULL && (oy < clip->y || oy >= clip->y + clip->height)) {
			continue;
		}
		/* THE ROW'S SPAN, CLIPPED TO THE OUTPUT ONCE rather than tested per pixel. */
		if (dx0 < 0) {
			first = -dx0;
		}
		if (dx0 + last > output->width) {
			last = output->width - dx0;
		}
		if (clip != NULL) {
			if (first < clip->x - dx0) {
				first = clip->x - dx0;
			}
			if (last > clip->x + clip->width - dx0) {
				last = clip->x + clip->width - dx0;
			}
		}
		if (first >= last) {
			continue;
		}
		source = (const uint32_t *)(const void *)(base + (size_t)row * (size_t)stride);
		destination = output->pixels + (size_t)oy * (size_t)output->width + (size_t)dx0;
		/* **A ROW BETWEEN THE CORNERS HAS ONE SHAPE FOR ALL OF ITS PIXELS, AND IT IS A RECTANGLE.** `shape_coverage`
		 * says so itself: a pixel is rounded only when it is within the radius of a VERTICAL edge AND of a HORIZONTAL
		 * one, so a row that is clear of both horizontal corner bands has no shape to ask about at all. Asking per
		 * pixel, and blending per pixel, is what made a full-screen composite cost 20 ms — MEASURED, and the reason a
		 * click with a wallpaper behind it waited a tenth of a second. THE THREE CASES BELOW ARE THE WHOLE ROW:
		 * an opaque one is a copy, a transparent one is mostly nothing, and only the corner bands need the shape. */
		if (copy_corner <= 0 || (row >= copy_corner && row < copy_h - copy_corner)) {
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
			shape = shape_coverage(output, surface, copy_w, copy_h, column, row);
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

	/* **AND ANYTHING THAT IS NOT 1:1 TAKES ONE GENERAL PATH, IN DEVICE PIXELS.** The loops above are written in buffer
	 * pixels because almost every client is 1:1 with a 1x output, and the fast paths there — an opaque row is a copy,
	 * four transparent pixels are asked at once — are most of what makes a composite cheap. Every one of them assumes
	 * source and destination advance together, which stops being true the moment either factor is not 1:
	 *
	 *   the client says 2   the buffer has 2x2 device pixels per logical pixel
	 *   the output is 2     every logical pixel becomes a 2x2 block of device pixels
	 *   the output is 1.5   a logical pixel is one and a half device pixels, and neither space divides the other
	 *
	 * THE RATIO IS THE WHOLE OF IT: `buffer_scale / output_scale` buffer pixels per device pixel, and the answer for
	 * each device pixel is the average of what it covers. At 2/2 that average is one pixel (the crisp HiDPI copy), at
	 * 2/1 it is a 2x2 block (a scaled client on an ordinary screen), at 1/2 the box is smaller than a pixel and the
	 * patch is magnified, and at 2/1.5 it is three quarters of a buffer pixel. One expression covers all four, which
	 * is why there is no second special case for fractions. */
	if (!ratio_one) {
		int dx1 = cpu_device_edge(surface->x + width, out_scale);
		int dy1 = cpu_device_edge(surface->y + height, out_scale);
		int px, py;

		for (py = dy0; py < dy1; py++) {
			double by0, by1;

			if (py < 0 || py >= output->height) {
				continue;
			}
			if (clip != NULL && (py < clip->y || py >= clip->y + clip->height)) {
				continue;
			}
			/* THE BOX IN THE BUFFER: the device row's own span, divided by the ratio. Computed from the DEVICE
			 * coordinate rather than accumulated, so a fractional ratio cannot drift across a window. */
			by0 = (double)(py - dy0) * client_scale / out_scale;
			by1 = (double)(py - dy0 + 1) * client_scale / out_scale;
			for (px = dx0; px < dx1; px++) {
				double bx0, bx1;
				uint32_t pixel;
				unsigned shape;
				uint32_t alpha;
				uint32_t *destination;

				if (px < 0 || px >= output->width) {
					continue;
				}
				if (clip != NULL && (px < clip->x || px >= clip->x + clip->width)) {
					continue;
				}
				bx0 = (double)(px - dx0) * client_scale / out_scale;
				bx1 = (double)(px - dx0 + 1) * client_scale / out_scale;
				pixel = scaled_block_pixel(base, (size_t)stride, buffer_width, buffer_height, bx0, bx1,
							   by0, by1, transparent);
				/* THE SHAPE IS ASKED PER DEVICE PIXEL, AND IN DEVICE UNITS — the box it is asked about is the
				 * window's DEVICE extent, not its logical one. Mixing them is a real trap: at 2x a logical width of
				 * 80 with a device column of 160 puts every pixel past `width - radius` and into the corner case, so
				 * the shape answers "outside" for most of the window and nothing is drawn at all. */
				shape = shape_coverage(output, surface, dx1 - dx0, dy1 - dy0, px - dx0, py - dy0);
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
	wl_shm_buffer_end_access(shm);
}

/* **THE BAR, DRAWN AT ITS OWN LEVEL** — the strip's fill, the SYSTEM menu at its left, and its shadow below.
 * Called from the level walk at `AWS_MENU_BAR_LEVEL`, so the desktop and ordinary windows are painted first and the
 * application's own menu window (at the same level, later in the list) is over it. */
static void aws_output_draw_menu_bar(struct aws_server *server, const struct aws_box *clip)
{
	struct aws_backend *backend = &server->backend;
	int row, column;

	double scale = aws_output_scale(server);
	int bar_height = cpu_device_edge(AWS_MENU_BAR_HEIGHT, scale);	/* 24 logical rows are 36 device rows at 1.5 */
	int first_column = 0, last_column = backend->width;

	/* **THE BAR IS A WRITER LIKE ANY OTHER, SO THE FRAME'S BOX CLIPS IT TOO.** A partial frame that does not touch the
	 * bar's rows paints none of them — which is what makes a strip in a window cost nothing in the bar. */
	if (clip != NULL) {
		if (clip->x > first_column) {
			first_column = clip->x;
		}
		if (clip->x + clip->width < last_column) {
			last_column = clip->x + clip->width;
		}
	}
	for (row = 0; row < bar_height && row < backend->height; row++) {
		if (clip != NULL && (row < clip->y || row >= clip->y + clip->height)) {
			continue;
		}
		for (column = first_column; column < last_column; column++) {
			backend->pixels[row * backend->width + column] = AWS_MENU_BAR_COLOUR;
		}
	}
	/* THE MARK IS INSIDE THE BAR'S ROWS, and the bar's rows are only painted when the box reaches them, so a frame that
	 * misses the strip draws nothing here — the guard is the same one, asked once. */
	if (clip == NULL || (clip->y < bar_height && clip->y + clip->height > 0)) {
		aws_output_draw_system_menu(backend, server->system_menu_icon, scale, clip);
	}
	/* **AND THE BAR'S OWN SHADOW, WHICH FALLS BELOW IT ON WHATEVER IS UNDER IT.** The strip's rectangle is expanded
	 * and the falloff computed the same way, and the sides that run off the top, the left and the right are dropped
	 * by the bounds check — and `aws_output_draw_shadow` stops at the bar's bottom edge, so a pop-down menu's shadow
	 * is never painted up across the bar. */
	aws_output_draw_shadow(backend, 0, 0, backend->width, bar_height, 0,
			       cpu_device_edge(AWS_SHADOW_DEPTH, scale), bar_height, clip);
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
static void cpu_emit(struct aws_server *server, struct aws_surface *surface, uint32_t time,
		     const struct aws_box *box)
{
	struct aws_backend *backend = &server->backend;

	if (surface == NULL) {
		aws_output_draw_menu_bar(server, box);
		return;
	}
	{
		/* **A SURFACE OUTSIDE THE FRAME STILL OWES ITS CALLBACK AND NOTHING ELSE.** Trap 1 of DAMAGE-PLAN.md §6: a client
		 * that commits and waits for `frame` never draws again if the callback is skipped along with the pixels, so this
		 * is a return AFTER the callback — never before it. */
		if (box != NULL) {
			struct aws_box footprint;
			double scale = aws_output_scale(server);
			int device_width = cpu_device_edge(surface->frame_width, scale);
			int device_height = cpu_device_edge(surface->frame_height, scale);

			footprint.x = cpu_device_edge(surface->x, scale);
			footprint.y = cpu_device_edge(surface->y, scale);
			if (device_width <= 0 || device_height <= 0) {
				struct wl_shm_buffer *shm = surface->current_buffer != NULL
					? wl_shm_buffer_get(surface->current_buffer) : NULL;

				device_width = (shm != NULL) ? wl_shm_buffer_get_width(shm) : 0;
				device_height = (shm != NULL) ? wl_shm_buffer_get_height(shm) : 0;
			}
			footprint.width = device_width;
			footprint.height = device_height;
			/* THE SHADOW IS PART OF THE FOOTPRINT: it is drawn outside the frame, and a box that only clips to the
			 * window's own pixels would leave a shadow row unpainted for a window beside it. `AWS_SHADOW_DEPTH` is the
			 * reach, in logical points, and it is scaled like everything else. */
			if (surface->y + surface->frame_height > AWS_MENU_BAR_HEIGHT) {
				int depth = cpu_device_edge(AWS_SHADOW_DEPTH, scale);

				footprint.x -= depth;
				footprint.y -= depth;
				footprint.width += depth * 2;
				footprint.height += depth * 2;
			}
			if (!aws_box_intersects(box, &footprint)) {
				aws_surface_frame_done(surface, time);
				return;
			}
		}
	}
	{
		/* **THE SHADOW IS PLACED IN DEVICE PIXELS AND ITS BUDGETS ARE LOGICAL.** A window's frame is logical, so the
		 * rectangle and the reach are multiplied here — once, at the boundary — and everything inside the shadow's
		 * own loop is device pixels. The bar test is logical too, because a window that lives in the bar is a fact
		 * about where the client put it, not about how many pixels the screen has. */
		double scale = aws_output_scale(server);
		int shx = cpu_device_edge(surface->x, scale);
		int shy = cpu_device_edge(surface->y, scale);

		if (surface->y < 0 || surface->y + surface->frame_height > AWS_MENU_BAR_HEIGHT) {
			/* **AND A SHEET'S SHADOW STOPS AT THE SEAM.** The sheet's top edge is tucked into its parent's title bar by
			 * `sheet_seam` points (the client says so — see the protocol), and the rows above the BOTTOM of that tuck are
			 * the bar's, so a shadow there is a shadow ON the title bar: the one thing a sheet may not touch. `shy` is the
			 * sheet's own top edge in device pixels and the seam is added to it, rounded with it rather than separately, so
			 * the two agree about where the row is. The bar's rows are the floor every window has. */
			int clip_top = cpu_device_edge(AWS_MENU_BAR_HEIGHT, scale);

			if (surface->sheet_parent != NULL) {
				int seam = cpu_device_edge(surface->y + surface->sheet_seam, scale);

				if (seam > clip_top) {
					clip_top = seam;
				}
			}
			/* THE EDGES ROUND, THE SPAN IS THE DIFFERENCE: the same rule the composite uses, so a window's
			 * shadow and its pixels agree about where the window is at any scale. */
			aws_output_draw_shadow(backend, shx, shy,
					       cpu_device_edge(surface->x + surface->frame_width, scale) - shx,
					       cpu_device_edge(surface->y + surface->frame_height, scale) - shy,
					       cpu_device_edge(AWS_SHADOW_RADIUS, scale),
					       cpu_device_edge(AWS_SHADOW_DEPTH, scale), clip_top, box);
		}
	}
	composite_surface(backend, surface, box);
	/* AND THE FRAME CALLBACK GOES WITH THE COMPOSITE, NOT WITH THE COMMIT: the client may draw again only once its
	 * picture is on screen. */
	aws_surface_frame_done(surface, time);
}

/* **ONE RECTANGLE OF A FRAME, AS A WHOLE PASS.** A region's rectangles are disjoint, so each is cleared, walked and
 * given its pointer independently — the picture is the same in every pass, and the only thing a pass must not do is
 * touch a pixel outside its own box. `NULL` is the whole output. */
static void cpu_draw_rect(struct aws_server *server, const struct aws_box *box)
{
	struct aws_backend *backend = &server->backend;
	size_t count = (size_t)backend->width * (size_t)backend->height;
	size_t i;

	/* **THE BACKGROUND IS THE ONE WRITER THAT CLEARS RATHER THAN DRAWS, so a partial frame clears only its own rows.**
	 * Everything behind a window on those rows is repainted by the walk, and everything outside them already holds the
	 * last frame's picture — including the pixels a window's shadow painted. */
	if (box == NULL) {
		for (i = 0; i < count; i++) {
			backend->pixels[i] = AWS_BACKGROUND;
		}
	} else {
		int row;

		for (row = box->y; row < box->y + box->height && row < backend->height; row++) {
			size_t start = (size_t)row * (size_t)backend->width + (size_t)box->x;

			for (i = 0; i < (size_t)box->width && box->x + (int)i < backend->width; i++) {
				backend->pixels[start + i] = AWS_BACKGROUND;
			}
		}
	}
	aws_output_walk(server, cpu_emit, box);
	/* **AND THE POINTER IS OVER EVERYTHING, INCLUDING THE MENU BAR.** It is drawn last because that is what "above"
	 * means here: the walk has painted the desktop, the windows and the furniture in their own order, and a cursor is
	 * the one thing with no level. A backend with a host pointer of its own draws nothing (see `backend.owns_cursor`). */
	aws_cursor_draw_cpu_box(server, box);
}

static void cpu_draw(struct aws_server *server, const pixman_region32_t *damage)
{
	pixman_box32_t *rects;
	int count = 0;
	int i;

	if (damage == NULL) {
		cpu_draw_rect(server, NULL);
		return;
	}
	/* pixman's query is not const-correct — it hands back the region's own rectangles — so the cast is the price of
	 * a renderer signature that says it will not change the damage. */
	rects = pixman_region32_rectangles((pixman_region32_t *)damage, &count);
	for (i = 0; i < count; i++) {
		struct aws_box box;

		box.x = rects[i].x1;
		box.y = rects[i].y1;
		box.width = rects[i].x2 - rects[i].x1;
		box.height = rects[i].y2 - rects[i].y1;
		cpu_draw_rect(server, &box);
	}
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
