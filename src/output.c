/*
 * output.c — the output, the compositing, and the screenshot.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
 *
 * THE OUTPUT IS A FRAMEBUFFER IN THIS PROCESS. That is what "headless" means here and it is not a stand-in for
 * anything: a compositor's job is to produce a rectangle of pixels, and this version produces it in memory
 * instead of on a display. The rendering below never asks which backend it has — it is given a width, a height
 * and a place to put pixels, and a DRM/KMS backend or a nested-in-X11 backend supplies the same three things.
 *
 * THE WHOLE OUTPUT IS RECOMPOSITED EVERY FRAME. That is a decision and not laziness: damage is tracked nowhere
 * (see `surface_damage`), so there is nothing to use to avoid the work, and a partial composite whose damage
 * bookkeeping is wrong is a picture that is subtly stale — which is worse than a picture that is whole. The
 * optimisation is where a REASON for one arrives, and at 1280x720 of XRGB8888 it is 3.5 MB of copying.
 *
 * NO DRAWING LIBRARY IS TAKEN YET, which is why the compositing is a row loop. pixman would earn its place the
 * moment the compositor draws something of its OWN — the menu bar, the dock, a window frame — and this version
 * owns none of those; it puts client buffers on a background. That is the honest reason to have no dependency,
 * rather than a small one.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* THE DESKTOP'S OWN COLOUR, for the area no client covers — and the one piece of "drawing" this version does.
 * A distinct colour rather than black, because a black background is indistinguishable from a client that
 * painted black, and this has to be checkable from a screenshot. */
#define AWS_BACKGROUND 0xFF303338u

static uint32_t aws_now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u);
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
	int width, height, stride;
	int transparent;
	int row, column;

	if (shm == NULL) {
		return;
	}
	width = wl_shm_buffer_get_width(shm);
	height = wl_shm_buffer_get_height(shm);
	stride = wl_shm_buffer_get_stride(shm);
	transparent = wl_shm_buffer_get_format(shm) == WL_SHM_FORMAT_ARGB8888;

	wl_shm_buffer_begin_access(shm);
	base = wl_shm_buffer_get_data(shm);
	if (base == NULL) {
		wl_shm_buffer_end_access(shm);
		return;
	}
	for (row = 0; row < height; row++) {
		int oy = surface->y + row;
		const uint32_t *source;

		if (oy < 0 || oy >= output->height) {
			continue;
		}
		source = (const uint32_t *)(const void *)(base + (size_t)row * (size_t)stride);
		for (column = 0; column < width; column++) {
			int ox = surface->x + column;
			uint32_t pixel = source[column];

			if (ox < 0 || ox >= output->width) {
				continue;
			}
			if (transparent) {
				uint32_t alpha = pixel >> 24;

				if (alpha == 0) {
					continue;
				}
				if (alpha != 0xFFu) {
					/* SOURCE OVER, on the destination's own bits: the frame buffer is opaque, so there is
					 * no alpha to carry and the blend happens here or nowhere. */
					uint32_t destination = output->pixels[(size_t)oy * (size_t)output->width + (size_t)ox];
					uint32_t inverse = 255u - alpha;
					uint32_t r = (((pixel >> 16) & 0xFFu) * alpha + ((destination >> 16) & 0xFFu) * inverse) / 255u;
					uint32_t g = (((pixel >> 8) & 0xFFu) * alpha + ((destination >> 8) & 0xFFu) * inverse) / 255u;
					uint32_t b = ((pixel & 0xFFu) * alpha + (destination & 0xFFu) * inverse) / 255u;

					pixel = 0xFF000000u | (r << 16) | (g << 8) | b;
				}
			}
			output->pixels[(size_t)oy * (size_t)output->width + (size_t)ox] = pixel | 0xFF000000u;
		}
	}
	wl_shm_buffer_end_access(shm);
}

void aws_output_render(struct aws_server *server)
{
	struct aws_backend *backend = &server->backend;
	struct aws_surface *surface;
	size_t count = (size_t)backend->width * (size_t)backend->height;
	size_t i;
	uint32_t time = aws_now_ms();

	for (i = 0; i < count; i++) {
		backend->pixels[i] = AWS_BACKGROUND;
	}
	/* MAPPING ORDER IS THE STACKING: the list is walked front-to-back, so the last mapped is on top. A
	 * compositor that has raised a window has reordered this list; this version never reorders it. */
	/* **BY LEVEL FIRST, THEN BY MAPPING ORDER WITHIN ONE.** A level is an `NSWindow` level — an integer the
	 * APPLICATION chose — so this loop is what makes a menu window sit above ordinary windows and a status item
	 * above that, without this file knowing what any of those numbers mean. Within a level the list order is the
	 * stacking, which is what raise and lower change. */
	{
		int lowest = 0, highest = 0, level, first = 1;

		wl_list_for_each(surface, &server->surfaces, link) {
			if (!surface->mapped) {
				continue;
			}
			if (first || surface->level < lowest) {
				lowest = surface->level;
			}
			if (first || surface->level > highest) {
				highest = surface->level;
			}
			first = 0;
		}
		for (level = lowest; level <= highest; level++) {
			wl_list_for_each(surface, &server->surfaces, link) {
				if (!surface->mapped || surface->level != level) {
					continue;
				}
				composite_surface(backend, surface);

		/* **THE BUFFER GOES BACK BEFORE THE FRAME CALLBACK, AND THE ORDER IS THE WHOLE THING.** A Wayland
		 * buffer belongs to the COMPOSITOR from `attach` until the compositor releases it, so a client that
		 * hears "draw again" while its buffer is still held has nowhere to draw: it takes the frame callback,
		 * finds the buffer busy, does nothing, and waits for a callback that will not come. MEASURED — this
		 * is what stopped the AppKit at three frames. Releasing first means the client that hears the callback
		 * can actually use what it hears. */
				if (surface->current_buffer != NULL) {
					wl_buffer_send_release(surface->current_buffer);
				}
				/* AND THE FRAME CALLBACK GOES WITH THE COMPOSITE, NOT WITH THE COMMIT: the client may
				 * draw again only once its picture is on screen. */
				aws_surface_frame_done(surface, time);
			}
		}
	}
}

/* ---------------------------------------------------------------------------------------------------
 * the screenshot, which is this version's only way of being checked
 * --------------------------------------------------------------------------------------------------- */

/* PPM AND NOT PNG: the point is to be checkable, and a binary PPM is four lines of header and the pixels.
 * `tests/inspect.py` reads it with the same image library it reads a screenshot from any other compositor —
 * which is what makes the two comparable. */
int aws_output_write_ppm(struct aws_server *server, const char *path)
{
	struct aws_backend *backend = &server->backend;
	FILE *file = fopen(path, "wb");

	if (file == NULL) {
		return -1;
	}
	fprintf(file, "P6\n%d %d\n255\n", backend->width, backend->height);
	for (size_t i = 0; i < (size_t)backend->width * (size_t)backend->height; i++) {
		unsigned char rgb[3];

		rgb[0] = (unsigned char)((backend->pixels[i] >> 16) & 0xFFu);
		rgb[1] = (unsigned char)((backend->pixels[i] >> 8) & 0xFFu);
		rgb[2] = (unsigned char)(backend->pixels[i] & 0xFFu);
		if (fwrite(rgb, 1, 3, file) != 3) {
			fclose(file);
			return -1;
		}
	}
	fclose(file);
	return 0;
}

/* ---------------------------------------------------------------------------------------------------
 * wl_output
 * --------------------------------------------------------------------------------------------------- */

static void output_release(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_output_interface output_implementation = {
	output_release,
};

static void bind_output(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct aws_server *server = data;
	struct wl_resource *resource = wl_resource_create(client, &wl_output_interface,
							  version < 2 ? version : 2, id);

	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &output_implementation, data, NULL);
	/* THE FOUR EVENTS A CLIENT NEEDS TO KNOW WHAT THE SCREEN IS, in the order the protocol wants them:
	 * geometry, then the mode, then (v2) the scale, then `done` — and `done` is what makes the properties
	 * readable, so a client that reads before it will read nothing. */
	wl_output_send_geometry(resource, 0, 0, 0, 0, WL_OUTPUT_SUBPIXEL_UNKNOWN, "Agora", "headless",
				WL_OUTPUT_TRANSFORM_NORMAL);
	wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
			    server->backend.width, server->backend.height, 60000);
	if (wl_resource_get_version(resource) >= 2) {
		wl_output_send_scale(resource, 1);
		wl_output_send_done(resource);
	}
	if (server->output.resource == NULL) {
		server->output.resource = resource;
	}
}

void aws_output_init(struct aws_server *server)
{
	/* THE PIXELS ARE THE BACKEND'S NOW — see backend.c — and this owns only the global that describes them. */
	wl_global_create(server->display, &wl_output_interface, 2, server, bind_output);
}
