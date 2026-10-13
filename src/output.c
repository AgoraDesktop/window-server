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
/* **AND THE WALLPAPER, WHICH IN THE SAMPLE IS A GRADIENT AND IS ONE COLOUR HERE.** Argentum's desktop runs from a
 * warm amber at the left edge — (72,36,14) and (108,55,29) — down to near-black at the bottom corners, (18,14,5).
 * A flat fill is the honest first step and a vertical ramp is the next one; the samples are recorded in
 * `ARGENTUM.md` so the next person does not have to re-measure the mockup to find them. */
#define AWS_BACKGROUND 0xFF2A1D0Du

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
/* **THE ONE THING IN THE BAR THAT IS THE SERVER'S, AND THE FIRST THING HERE THAT IS DRAWN RATHER THAN FILLED.**
 * Every other pixel in this file is a background or a client's buffer; the System menu is neither. Its glyph is the
 * hamburger — U+2630 — which IS three horizontal bars, so its shape is arithmetic and no font is needed: three
 * bars of THICKNESS, two gaps of GAP, centred in a square of WIDTH. A font arrives with the first thing whose
 * shape is NOT already known. See DESIGN.md for why it is not the Apple logo. */
static void aws_output_draw_system_menu(struct aws_backend *backend)
{
	const int glyph_height = 3 * AWS_SYSTEM_MENU_BAR_THICKNESS + 2 * AWS_SYSTEM_MENU_BAR_GAP;
	const int left = (AWS_SYSTEM_MENU_WIDTH - AWS_SYSTEM_MENU_GLYPH_WIDTH) / 2;
	const int top = (AWS_MENU_BAR_HEIGHT - glyph_height) / 2;
	int bar, row, column;

	for (bar = 0; bar < 3; bar++) {
		const int first = top + bar * (AWS_SYSTEM_MENU_BAR_THICKNESS + AWS_SYSTEM_MENU_BAR_GAP);
		const int last = first + AWS_SYSTEM_MENU_BAR_THICKNESS;

		for (row = first; row < last; row++) {
			if (row < 0 || row >= backend->height) {
				continue;
			}
			for (column = left; column < left + AWS_SYSTEM_MENU_GLYPH_WIDTH; column++) {
				if (column < 0 || column >= backend->width) {
					continue;
				}
				backend->pixels[(size_t)row * (size_t)backend->width + (size_t)column] =
					AWS_SYSTEM_MENU_GLYPH_COLOUR;
			}
		}
	}
}

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
	/* **THE SERVER'S OWN FURNITURE, AND THE FIRST PIXELS DRAWN HERE THAT ARE NOT A CLIENT'S.** The menu bar
	 * strip is the WindowServer's on the Mac — it owns the bar and the SYSTEM menu at its left —, while everything
	 * to the right of that is drawn by CLIENTS and merely PLACED by the server (see DESIGN.md). The height is
	 * the same constant `aws_shell_reserved` subtracts to answer the work area, so the strip and the reserve
	 * cannot come apart.
	 *
	 * AND IT IS DRAWN FIRST, WHICH IS A DECISION RATHER THAN AN ORDER: a window may be given a frame that
	 * overlaps the bar, because the application's frame is honoured, and until the strip has a LEVEL of its own
	 * such a window covers it. The level is the next step and the honest place to leave it. */
	{
		int row, column;

		for (row = 0; row < AWS_MENU_BAR_HEIGHT && row < backend->height; row++) {
			for (column = 0; column < backend->width; column++) {
				backend->pixels[row * backend->width + column] = AWS_MENU_BAR_COLOUR;
			}
		}
	}
	aws_output_draw_system_menu(backend);
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
				/* **AND A WINDOW THE APPLICATION HAS TAKEN OFF THE SCREEN IS NOT COMPOSITED — AND IS NOT
				 * RELEASED EITHER.** `-orderOut:` is a real door, and without this the server painted every window
				 * a client had ever hidden: MEASURED, closing a menu left its panel on the glass, and the ones
				 * behind it accumulated until the run loop was repainting a stack of dead menus every pass. Its
				 * buffer is deliberately NOT released here: a hidden window is not drawing, and the release that
				 * would let it draw again belongs with the frame callback that comes when it is shown. */
				if (!surface->mapped || !surface->visible || surface->level != level) {
					/* **A SURFACE WITH A BUFFER THAT IS BEING SKIPPED IS A CLIENT WAITING FOR EVER.** The buffer
					 * belongs to the compositor from `attach` until this loop releases it — so a surface that never
					 * reaches the release has a client that cannot draw again, and it will look like an application
					 * that is merely slow. WHICH GUARD DID IT is the question, and this names it. */
					/* **AND ONLY A SURFACE AT THIS PASS'S OWN LEVEL IS A SURPRISE.** The first version of this
					 * fired for EVERY pass whose level differed from the surface's — which is every pass but one,
					 * and it printed sixty thousand lines of ordinary behaviour. MEASURED, and two rounds went
					 * into reading that noise as evidence. A surface that has a buffer, is mapped and visible,
					 * and is at the level being drawn, and STILL does not get composited: that is the one that
					 * would mean the range is wrong. */
					if (getenv("AWS_COMPOSITE_DEBUG") != NULL && surface->current_buffer != NULL &&
					    surface->level == level) {
						fprintf(stderr, "  DIAG composite: SKIPPED a surface WITH A BUFFER — mapped=%d "
								"visible=%d surface level=%d, this pass is level %d %s\n",
							surface->mapped, surface->visible, surface->level, level,
							(!surface->mapped) ? "<- NOT MAPPED"
							: (!surface->visible) ? "<- NOT VISIBLE"
							: "<- A DIFFERENT LEVEL, and no pass will reach it");
					}
					continue;
				}
				composite_surface(backend, surface);

		/* **THE BUFFER GOES BACK BEFORE THE FRAME CALLBACK, AND THE ORDER IS THE WHOLE THING.** A Wayland
		 * buffer belongs to the COMPOSITOR from `attach` until the compositor releases it, so a client that
		 * hears "draw again" while its buffer is still held has nowhere to draw: it takes the frame callback,
		 * finds the buffer busy, does nothing, and waits for a callback that will not come. MEASURED — this
		 * is what stopped the AppKit at three frames. Releasing first means the client that hears the callback
		 * can actually use what it hears. */
				/* **THE RELEASE IS NOT HERE ANY MORE — see the sweep below.** Releasing inside the loop
				 * released only the surfaces the loop REACHED, and released them exactly once; a surface it did
				 * not reach kept its buffer for ever, which is a client waiting for a frame that never comes. */
				/* AND THE FRAME CALLBACK GOES WITH THE COMPOSITE, NOT WITH THE COMMIT: the client may
				 * draw again only once its picture is on screen. */
				aws_surface_frame_done(surface, time);
			}
		}

		/* **ONE RELEASE PER SURFACE PER PASS, WHICHEVER LEVEL IT IS AT.** A Wayland buffer belongs to the
		 * compositor from `attach` until it is released, so a surface the level loop SKIPS keeps its buffer for
		 * ever — and its client waits for a frame callback that only comes when that buffer is released.
		 * MEASURED, from a live session: `frame_ready=0 busy=1` for a window whose paint was refused over and over,
		 * because the compositor had never let go. THE RELEASE IS NOT A PAINTING: a hidden window, a window at a
		 * level nothing draws, a window whose buffer the loop reached — all of them are released here, once, and
		 * the frames that were drawn were already sent their callbacks above. */
		wl_list_for_each(surface, &server->surfaces, link) {
			if (surface->current_buffer != NULL) {
				wl_buffer_send_release(surface->current_buffer);
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
