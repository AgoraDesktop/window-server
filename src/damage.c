/*
 * damage.c — what the next frame must repaint, and why.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * THE CLIENT NAMES ITS RECTANGLES AND THIS TURNS THEM INTO A FRAME. `wl_surface.damage_buffer` is in BUFFER pixels with
 * a top-down origin — the client's own statement about which of its pixels changed — and a composite draws in DEVICE
 * pixels, which are neither the buffer's nor the logical pixels a window is placed in. There are three spaces in this
 * file's arithmetic and they are different numbers at every scale that is not exactly 1:
 *
 *   buffer  ÷ client's buffer_scale   ->  logical  ×  the output's scale  ->  device
 *
 * Mixing two of them up is a damage rectangle that looks plausible and misses the strip by a hair — at this machine's
 * 1.332 it is a band of stale pixels at every window's bottom edge, which is exactly the kind of bug a static
 * screenshot never shows. So the translation lives here, ONCE, and everything above it (the renderers, the cursor, the
 * frame loop) speaks the one space `struct aws_box` is documented in.
 *
 * AND THE FRAME IS A REGION, NOT A BOUNDING BOX. Two strips from one client, two surfaces, the pointer's old place and
 * its new one — a box around all of that repaints the gap between them, which is the work this file exists to avoid. The
 * union is a `pixman_region32_t`, and the renderers walk its rectangles one at a time, so a writer still only ever has
 * to clip to ONE box: the region is a scheduling decision, not a second clipping path. A bounding box remains the right
 * answer for the pointer's own sweep (`server->damage`), because consecutive positions overlap.
 *
 * AND "THE WHOLE BUFFER" IS A SENTENCE, NOT A RECTANGLE. Wayland's rule is that a commit which attaches a buffer and
 * damages nothing means every pixel; the surface's `damage_whole` keeps that meaning until the composite asks, because
 * the buffer's size may not be readable (a dma-buf) and the frame may be smaller than it (a status item).
 */

#include "server.h"

#include <pixman.h>

#include <stdlib.h>
#include <string.h>

#include <wayland-server-protocol.h>

/* ---------------------------------------------------------------------------------------------------
 * rectangles
 * --------------------------------------------------------------------------------------------------- */

int aws_box_empty(const struct aws_box *box)
{
	return box == NULL || box->width <= 0 || box->height <= 0;
}

static void box_add(struct aws_box *into, int x0, int y0, int x1, int y1)
{
	int right, bottom;

	if (x1 <= x0 || y1 <= y0) {
		return;
	}
	if (aws_box_empty(into)) {
		into->x = x0;
		into->y = y0;
		into->width = x1 - x0;
		into->height = y1 - y0;
		return;
	}
	right = into->x + into->width;
	bottom = into->y + into->height;
	if (x0 < into->x) {
		into->x = x0;
	}
	if (y0 < into->y) {
		into->y = y0;
	}
	if (x1 > right) {
		right = x1;
	}
	if (y1 > bottom) {
		bottom = y1;
	}
	into->width = right - into->x;
	into->height = bottom - into->y;
}

void aws_damage_box(struct aws_server *server, const struct aws_box *box)
{
	aws_box_union(&server->damage, box);
}

void aws_box_union(struct aws_box *into, const struct aws_box *add)
{
	if (aws_box_empty(add)) {
		return;
	}
	box_add(into, add->x, add->y, add->x + add->width, add->y + add->height);
}

/* CLIPPED TO THE OUTPUT, which is what turns a window half off the right edge into a box the renderers can take: a box
 * that names columns the framebuffer does not have would have every writer testing them one at a time. */
void aws_box_clip(struct aws_box *box, int width, int height)
{
	int x0, y0, x1, y1;

	if (aws_box_empty(box)) {
		box->x = 0;
		box->y = 0;
		box->width = 0;
		box->height = 0;
		return;
	}
	x0 = box->x < 0 ? 0 : box->x;
	y0 = box->y < 0 ? 0 : box->y;
	x1 = box->x + box->width;
	y1 = box->y + box->height;
	if (x1 > width) {
		x1 = width;
	}
	if (y1 > height) {
		y1 = height;
	}
	if (x1 <= x0 || y1 <= y0) {
		box->x = 0;
		box->y = 0;
		box->width = 0;
		box->height = 0;
		return;
	}
	box->x = x0;
	box->y = y0;
	box->width = x1 - x0;
	box->height = y1 - y0;
}

int aws_box_intersects(const struct aws_box *box, const struct aws_box *other)
{
	if (aws_box_empty(box) || aws_box_empty(other)) {
		return 0;
	}
	return box->x < other->x + other->width && other->x < box->x + box->width &&
	       box->y < other->y + other->height && other->y < box->y + box->height;
}

/* ---------------------------------------------------------------------------------------------------
 * a surface's ledger
 * --------------------------------------------------------------------------------------------------- */

struct pixman_region32 *aws_damage_region_new(void)
{
	pixman_region32_t *region = calloc(1, sizeof *region);

	if (region == NULL) {
		return NULL;
	}
	pixman_region32_init(region);
	return region;
}

void aws_damage_region_free(struct pixman_region32 *region)
{
	if (region == NULL) {
		return;
	}
	pixman_region32_fini(region);
	free(region);
}

void aws_damage_region_add(struct pixman_region32 *region, int x, int y, int width, int height)
{
	if (region == NULL || width <= 0 || height <= 0) {
		return;
	}
	pixman_region32_union_rect(region, region, x, y, (unsigned)width, (unsigned)height);
}

/* ---------------------------------------------------------------------------------------------------
 * the triggers
 * --------------------------------------------------------------------------------------------------- */

void aws_damage_full(struct aws_server *server, const char *reason)
{
	server->damage_full = 1;
	if (reason != NULL) {
		server->damage_reason = reason;
	}
}

/* ---------------------------------------------------------------------------------------------------
 * a surface's damage, translated
 * --------------------------------------------------------------------------------------------------- */

/* **THE WINDOW'S LOGICAL SIZE, AS THE COMPOSITE COMPUTES IT.** The buffer divided by the client's scale, then clipped
 * to the frame when the frame is the smaller of the two — a status item's 320x200 buffer in a 64x24 box is the case
 * that made `composite_surface` clip, and a damage rectangle in buffer pixels may be larger than the window it belongs
 * to. A dma-buf's size is not read here (this file must not need the import path to add up rectangles): its frame is
 * the answer, which is also what the GL renderer clips the quad to. */
static void surface_extent(const struct aws_surface *surface, int *width, int *height)
{
	double client = surface->buffer_scale > 0.0 ? surface->buffer_scale : 1.0;
	int logical_width = surface->frame_width;
	int logical_height = surface->frame_height;

	if (surface->current_buffer != NULL) {
		struct wl_shm_buffer *shm = wl_shm_buffer_get(surface->current_buffer);

		if (shm != NULL) {
			int buffer_width = (int)((double)wl_shm_buffer_get_width(shm) / client + 0.5);
			int buffer_height = (int)((double)wl_shm_buffer_get_height(shm) / client + 0.5);

			if (logical_width <= 0 || (buffer_width > 0 && buffer_width < logical_width)) {
				logical_width = buffer_width;
			}
			if (logical_height <= 0 || (buffer_height > 0 && buffer_height < logical_height)) {
				logical_height = buffer_height;
			}
		}
	}
	*width = logical_width > 0 ? logical_width : 0;
	*height = logical_height > 0 ? logical_height : 0;
}

/* **ONE RECTANGLE, IN BUFFER PIXELS, BECOME A DEVICE RECTANGLE ON THE OUTPUT.** The edges ROUND OUTWARD — floor on the
 * way in, ceil on the way out — because a device pixel that is only partly covered by the client's rectangle may have
 * been touched by it, and half a device pixel of over-paint is a slower frame while half a device pixel of under-paint
 * is a stale line. It is the same rule `libagora-app` uses on the client side of the wire, which is what keeps the
 * contract in one piece. */
static void add_buffer_rect(struct aws_server *server, const struct aws_surface *surface, double buffer_x,
			    double buffer_y, double buffer_width, double buffer_height,
			    pixman_region32_t *region)
{
	double client = surface->buffer_scale > 0.0 ? surface->buffer_scale : 1.0;
	double out = server->backend.scale > 0.0 ? server->backend.scale : 1.0;
	int x0, y0, x1, y1;

	x0 = aws_edge_floor(((double)surface->x + buffer_x / client) * out);
	y0 = aws_edge_floor(((double)surface->y + buffer_y / client) * out);
	x1 = aws_edge_ceil(((double)surface->x + (buffer_x + buffer_width) / client) * out);
	y1 = aws_edge_ceil(((double)surface->y + (buffer_y + buffer_height) / client) * out);
	if (x1 <= x0 || y1 <= y0) {
		return;
	}
	/* **AND THE RECTANGLE IS ONE RECTANGLE OF THE FRAME.** pixman unions it with what is already there, so two
	 * disjoint strips stay two rectangles — which is the whole difference between a region and a box. */
	pixman_region32_union_rect(region, region, x0, y0, (unsigned)(x1 - x0), (unsigned)(y1 - y0));
}

/* A SURFACE'S CONTRIBUTION, CLAMPED TO EVERYTHING THAT IS NOT PART OF ITS WINDOW: the buffer's own pixels, and the
 * frame's device extent when the buffer is bigger than the window. */
static void add_surface_damage(struct aws_server *server, const struct aws_surface *surface,
			       pixman_region32_t *region)
{
	double client = surface->buffer_scale > 0.0 ? surface->buffer_scale : 1.0;
	int logical_width, logical_height;
	int limit_width, limit_height;
	double x0, y0, x1, y1;

	surface_extent(surface, &logical_width, &logical_height);
	if (logical_width <= 0 || logical_height <= 0) {
		return;
	}
	limit_width = aws_edge_ceil((double)logical_width * client);
	limit_height = aws_edge_ceil((double)logical_height * client);
	if (surface->current_buffer != NULL) {
		struct wl_shm_buffer *shm = wl_shm_buffer_get(surface->current_buffer);

		if (shm != NULL) {
			if (wl_shm_buffer_get_width(shm) < limit_width) {
				limit_width = wl_shm_buffer_get_width(shm);
			}
			if (wl_shm_buffer_get_height(shm) < limit_height) {
				limit_height = wl_shm_buffer_get_height(shm);
			}
		}
	}
	if (surface->damage_whole) {
		add_buffer_rect(server, surface, 0.0, 0.0, (double)limit_width, (double)limit_height, region);
		return;
	}
	/* **EVERY RECTANGLE THE CLIENT NAMED, EACH CLAMPED TO THE BUFFER IN BUFFER PIXELS** — the space the protocol named
	 * them in. Two strips stay two rectangles here and in the frame, which is what makes the gap between them cost
	 * nothing. */
	{
		int count = 0;
		int i;
		pixman_box32_t *rects = pixman_region32_rectangles(surface->damage, &count);

		for (i = 0; i < count; i++) {
			x0 = rects[i].x1 < 0 ? 0.0 : (double)rects[i].x1;
			y0 = rects[i].y1 < 0 ? 0.0 : (double)rects[i].y1;
			x1 = (double)rects[i].x2;
			y1 = (double)rects[i].y2;
			if (x1 > (double)limit_width) {
				x1 = (double)limit_width;
			}
			if (y1 > (double)limit_height) {
				y1 = (double)limit_height;
			}
			if (x1 <= x0 || y1 <= y0) {
				continue;
			}
			add_buffer_rect(server, surface, x0, y0, x1 - x0, y1 - y0, region);
		}
	}
}

/* **THE LEDGER IS CONSUMED WHATEVER THE SURFACE TURNED OUT TO BE.** A hidden window's damage is not drawn, and keeping
 * it would be rectangles waiting for a frame that may never come; showing it again is a full redraw anyway (§4 step 2),
 * so the safe place for a skipped ledger is the floor. */
int aws_damage_frame_begin(struct aws_server *server, pixman_region32_t *region, int *full, const char **reason)
{
	struct aws_surface *surface;
	int count = 0;

	/* **THE CALLER OWNS THE REGION AND MUST `pixman_region32_fini` IT.** Initialising it here rather than making the
	 * caller remember to is one less way to hand a renderer an uninitialised region, and a `pixman_region32_t` that is
	 * never fini'd is a leak the tests under LSan would find. */
	pixman_region32_init(region);
	wl_list_for_each(surface, &server->surfaces, link) {
		if (!surface->damage_valid) {
			continue;
		}
		if (surface->mapped && surface->visible && surface->current_buffer != NULL) {
			add_surface_damage(server, surface, region);
			count++;
		}
		surface->damage_valid = 0;
		surface->damage_whole = 0;
		if (surface->damage != NULL) {
			pixman_region32_clear(surface->damage);
		}
	}
	/* AND WHAT THE COMPOSITOR ITSELF NAMED — the pointer's old and new rectangles. */
	if (!aws_box_empty(&server->damage)) {
		pixman_region32_union_rect(region, region, server->damage.x, server->damage.y,
					   (unsigned)server->damage.width, (unsigned)server->damage.height);
	}
	server->damage.x = 0;
	server->damage.y = 0;
	server->damage.width = 0;
	server->damage.height = 0;

	*full = server->damage_full;
	*reason = server->damage_reason;
	return count;
}
