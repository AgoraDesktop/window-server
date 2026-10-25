/*
 * cursor.c — the arrow the compositor draws when nobody else supplies one.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **THE SHAPE IS POINTS, NOT PIXELS.** An arrow rasterized on a 1x output and then magnified is a blocky arrow next
 * to device-resolution text, so the outline below is in POINTS and the raster is made for the output's own scale —
 * the same reason the menu bar's mark is rasterized at the size it is drawn. The scale here is FRACTIONAL (the
 * measured 1.332 of a 96-dpi panel), and the device box and the hotspot both ROUND, because a cursor that landed on
 * a half pixel would be a cursor drawn twice.
 *
 * THE TWO-TONE LOOK IS TWO LAYERS, NOT TWO SHAPES. A black arrow alone disappears against a dark window and a white
 * outline alone is washed out on a light one, so the image is WHITE FILLED FIRST and BLACK DRAWN OVER IT at one
 * device pixel of separation: the white fringe is the arrow's own coverage dilated by one pixel, and the inside is
 * the arrow itself. Both layers carry antialiased coverage, so a diagonal edge is a blend and not a staircase.
 */

#include "server.h"
#include "cursor.h"

#include <stdlib.h>
#include <string.h>

/* THE ARROW, IN POINTS: the tip, down the left edge, the notch, the tail's outer edge, the tail's end, back up to
 * the body, and out to the right shoulder. 11 x 18 points is the size a desktop pointer has been since forever;
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

/* THE RASTER, FOR ONE SCALE. Everything here is device pixels except the polygon, which is looked up in points and
 * divided by the scale at the sample: that is what makes the fringe one DEVICE pixel wide at every scale. */
static void cursor_rasterize(struct aws_cursor *cursor, double scale)
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

int aws_cursor_visible(struct aws_server *server)
{
	return server->backend.owns_cursor && server->pointer_inside;
}

void aws_cursor_ensure(struct aws_server *server)
{
	double scale = server->backend.scale;

	if (scale <= 0.0) {
		scale = 1.0;
	}
	if (server->cursor.pixels != NULL && server->cursor.scale == scale) {
		return;
	}
	free(server->cursor.pixels);
	memset(&server->cursor, 0, sizeof server->cursor);
	cursor_rasterize(&server->cursor, scale);
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
	struct aws_backend *backend = &server->backend;
	const struct aws_cursor *cursor;
	int left, top, row, column;

	if (!aws_cursor_visible(server)) {
		return;
	}
	aws_cursor_ensure(server);
	cursor = &server->cursor;
	if (cursor->pixels == NULL) {
		return;
	}
	left = cursor_device_edge((double)server->pointer_x * backend->scale) - cursor->hotspot_x;
	top = cursor_device_edge((double)server->pointer_y * backend->scale) - cursor->hotspot_y;
	/* CLIPPED TO THE OUTPUT PER PIXEL rather than by shrinking the loop: the pointer may be at the very edge, and a
	 * cursor that vanished there would be a pointer that left before it did. */
	for (row = 0; row < cursor->height; row++) {
		int y = top + row;

		if (y < 0 || y >= backend->height) {
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
