/*
 * cursor.h — the pointer the compositor draws itself.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **WHY THE COMPOSITOR OWNS A CURSOR AT ALL.** A nested or windowed backend has a host pointer already on the
 * glass; a DRM output has nothing but the pixels this process scans out, so a compositor with no cursor code has a
 * desktop with no visible pointer — which is working input and an invisible hand. This is the FALLBACK pointer: it
 * is shown wherever a client has not supplied one, and it is a compositor-owned bitmap rather than a plane because
 * it has to be visible over every client surface, including an empty desktop, and only a composited quad is.
 *
 * WHAT IT IS NOT: an implementation of `wl_pointer.set_cursor`. A client's cursor surface is still accepted and
 * ignored (see `pointer_set_cursor` in seat.c) — that is the protocol feature, and this is the safety net that makes
 * the shipping output usable without it.
 */

#ifndef AGORA_WINDOW_SERVER_CURSOR_H
#define AGORA_WINDOW_SERVER_CURSOR_H

#include <stdint.h>

struct aws_server;

/* THE ARROW, RASTERIZED FOR ONE SCALE AND KEPT UNTIL THE SCALE CHANGES. `pixels` is 0xAARRGGBB with straight
 * (non-premultiplied) alpha, which is what both renderers already blend. `hotspot` is the point of the IMAGE that
 * sits on the pointer's own position, in DEVICE pixels. */
struct aws_cursor {
	double scale;			/* the scale `pixels` was rasterized for; 0 until there is an image */
	int width;
	int height;
	int hotspot_x;
	int hotspot_y;
	uint32_t *pixels;
};

/* EVERY ANSWER BELOW IS THE SERVER'S, AND EVERY ONE OF THEM IS SAFE TO CALL WHEN THERE IS NOTHING TO DRAW. */

/* Whether a pointer belongs on the glass at all: the backend has no host cursor of its own AND the pointer is on
 * the output. Asked by both renderers, so the two agree about when a cursor exists. */
int aws_cursor_visible(struct aws_server *server);

/* Rasterize the arrow for the output's current scale if the cached image is for a different one. */
void aws_cursor_ensure(struct aws_server *server);

/* Draw the ARROW OVER THE FRAME, which is the caller's last act after the scene has been painted. Device-pixel
 * placement, fractional-scale rounding and clipping to the output are all here, so the CPU and GL paths cannot
 * disagree about where the pointer is. */
void aws_cursor_draw_cpu(struct aws_server *server);

/* The image, for a renderer that uploads it as a texture rather than blending it. NULL when there is nothing to
 * draw. */
const struct aws_cursor *aws_cursor_image(struct aws_server *server);

/* Give the raster back. Called from the server's own destruction. */
void aws_cursor_finish(struct aws_server *server);

#endif /* AGORA_WINDOW_SERVER_CURSOR_H */
