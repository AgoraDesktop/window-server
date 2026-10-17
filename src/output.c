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

#include <math.h>	/* sqrt — the one place a distance is needed */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* THE DESKTOP'S OWN COLOUR, for the area no client covers — and the one piece of "drawing" this version does.
 * A distinct colour rather than black, because a black background is indistinguishable from a client that
 * painted black, and this has to be checkable from a screenshot. */
/* **AND THE WALLPAPER: A LIGHT BLUE-GREEN "AQUA", WHICH IS A CHOICE AND NOT A SAMPLE.** Argentum's own desktop runs
 * from a warm amber at the left edge — (72,36,14) and (108,55,29) — down to near-black at the bottom corners,
 * (18,14,5); those numbers stay in `ARGENTUM.md`, where they are the record of the mockup, and THIS FILL NO LONGER
 * FOLLOWS THEM. The desktop is aqua BY REQUEST: (168,222,217), `#A8DED9` — light, and green-leaning, so it reads as
 * water rather than as sky. It is still ONE COLOUR rather than the sample's gradient, which is the honest first step
 * and a vertical ramp is still the next one. */
#define AWS_BACKGROUND 0xFFA8DED9u

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
 * bars of THICKNESS, two gaps of GAP, placed by their LEFT MARGIN in a square of WIDTH. A font arrives with the
 * first thing whose shape is NOT already known. See DESIGN.md for why it is not the Apple logo. */
static void aws_output_draw_system_menu(struct aws_backend *backend)
{
	const int glyph_height = 3 * AWS_SYSTEM_MENU_BAR_THICKNESS + 2 * AWS_SYSTEM_MENU_BAR_GAP;
	const int left = AWS_SYSTEM_MENU_LEFT_MARGIN;
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
 * ============================================================================================ */

/* The shadow's three numbers. The DEPTH is how far it reaches from the frame's edge and is the one a person
 * notices; the ALPHA is what it starts at AT that edge, before the falloff takes it to nothing; the RADIUS is the
 * frame's own corner, and it MIRRORS THE CLIENT'S `NS_CELL_BEZEL_RADIUS` — the protocol does not carry the radius
 * yet, so this is the one number in this file that is a copy rather than a fact, and it is named as one. */
#define AWS_SHADOW_DEPTH  10
#define AWS_SHADOW_ALPHA  120
#define AWS_SHADOW_RADIUS 4
/* **AND THE RADIUS IS AN ARGUMENT, BECAUSE THE SHADOW FOLLOWS THE SHAPE THAT CASTS IT.** A window's corners are
 * rounded and its shadow's are too; the MENU BAR's are square and its shadow's must be as well. MEASURED, passing
 * the window's radius for the bar's shadow painted a dark smudge INSIDE the bar's own corners — the distance
 * function reads a square corner as being outside a rounded shape, which is true and is not what a bar wants. */

/* ONE PIXEL OF SHADOW: black, over what is already there, at `alpha` of 255. */
static uint32_t aws_shadow_pixel(uint32_t destination, uint32_t alpha)
{
	uint32_t inverse = 255u - alpha;
	uint32_t r = ((destination >> 16) & 0xFFu) * inverse / 255u;
	uint32_t g = ((destination >> 8) & 0xFFu) * inverse / 255u;
	uint32_t b = (destination & 0xFFu) * inverse / 255u;

	return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* **AND THE SHADOW IS THE DISTANCE TO A ROUNDED RECTANGLE, WHICH IS WHY IT HAS ROUNDED CORNERS.** A rectangle's
 * shadow under a rounded window shows four square shoulders — the corners are exactly where a shadow is most
 * visible, because that is where it is widest. The signed distance to a rounded rectangle is three lines: clamp the
 * point into the rectangle's INNER box (the rectangle shrunk by the radius on every side), measure from there, and
 * take the radius off — negative inside the shape, positive outside, and the same expression at every corner. The
 * ALPHA then falls off as the square of the distance, which is what makes a hard edge read as a soft one. */
static void aws_output_draw_shadow(struct aws_backend *output, int x, int y, int w, int h, int radius)
{
	int left = x - AWS_SHADOW_DEPTH, top = y - AWS_SHADOW_DEPTH;
	int right = x + w + AWS_SHADOW_DEPTH, bottom = y + h + AWS_SHADOW_DEPTH;
	int halfW = w / 2, halfH = h / 2;
	int cx = x + halfW, cy = y + halfH;
	int innerW = halfW - radius, innerH = halfH - radius;
	int row, column;

	if (w <= 0 || h <= 0) {
		return;
	}
	for (row = top; row < bottom; row++) {
		int dy;

		/* **AND NOTHING BEHIND THE MENU BAR CASTS A SHADOW ONTO IT.** The bar is the server's own furniture and the
		 * topmost thing on the screen; a pop-down menu opens with its top edge AT the bar's bottom, so its shadow
		 * would otherwise paint UPWARDS across the strip — a dark band above every open menu, which reads as a dirty
		 * bar rather than as a shadow under a window. THE BAR'S OWN SHADOW FALLS BELOW THE STRIP and is unaffected by
		 * this line, because its rectangle begins at the strip's own bottom edge. */
		if (row < AWS_MENU_BAR_HEIGHT || row >= output->height) {
			continue;
		}
		dy = (row > cy ? row - cy : cy - row) - innerH;
		if (dy < 0) {
			dy = 0;
		}
		for (column = left; column < right; column++) {
			int dx, distance;
			double t, falloff;
			uint32_t alpha;

			if (column < 0 || column >= output->width) {
				continue;
			}
			dx = (column > cx ? column - cx : cx - column) - innerW;
			if (dx < 0) {
				dx = 0;
			}
			/* THE INTEGER SQUARE ROOT IS ENOUGH: a distance in pixels is the unit a falloff wants, and
			 * `sqrt` here would be the only double in the compositor's inner loop. */
			distance = (int)sqrt((double)(dx * dx + dy * dy)) - radius;
			if (distance <= 0 || distance > AWS_SHADOW_DEPTH) {
				continue;	/* inside the frame, or past the shadow's reach */
			}
			t = 1.0 - (double)distance / (double)AWS_SHADOW_DEPTH;
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

/* **THE BAR, DRAWN AT ITS OWN LEVEL** — the strip's fill, the SYSTEM menu at its left, and its shadow below.
 * Called from the level walk in `aws_output_render` at `AWS_MENU_BAR_LEVEL`, so the desktop and ordinary windows
 * are painted first and the application's own menu window (at the same level, later in the list) is over it. */
static void aws_output_draw_menu_bar(struct aws_backend *backend)
{
	int row, column;

	for (row = 0; row < AWS_MENU_BAR_HEIGHT && row < backend->height; row++) {
		for (column = 0; column < backend->width; column++) {
			backend->pixels[row * backend->width + column] = AWS_MENU_BAR_COLOUR;
		}
	}
	aws_output_draw_system_menu(backend);
	/* **AND THE BAR'S OWN SHADOW, WHICH FALLS BELOW IT ON WHATEVER IS UNDER IT.** The strip's rectangle is expanded
	 * and the falloff computed the same way, and the sides that run off the top, the left and the right are dropped
	 * by the bounds check — and `aws_output_draw_shadow` stops at the bar's bottom edge, so a pop-down menu's shadow
	 * is never painted up across the bar. */
	aws_output_draw_shadow(backend, 0, 0, backend->width, AWS_MENU_BAR_HEIGHT, 0);
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
	 * to the right of that is drawn by CLIENTS and merely PLACED by the server (see DESIGN.md). It is drawn at its
	 * own LEVEL, in the walk below, by `aws_output_draw_menu_bar`. */
	/* MAPPING ORDER IS THE STACKING: the list is walked front-to-back, so the last mapped is on top. A
	 * compositor that has raised a window has reordered this list; this version never reorders it. */
	/* **BY LEVEL FIRST, THEN BY MAPPING ORDER WITHIN ONE.** A level is an `NSWindow` level — an integer the
	 * APPLICATION chose — so this loop is what makes a menu window sit above ordinary windows and a status item
	 * above that, without this file knowing what any of those numbers mean. Within a level the list order is the
	 * stacking, which is what raise and lower change. */
	{
		/* **AND THE WALK IS OVER THE LEVELS THAT ARE PRESENT, NOT OVER EVERY NUMBER BETWEEN THEM.** A level
		 * is an arbitrary integer, and the desktop's is `INT32_MIN + 20` while an ordinary window's is 0 — so
		 * a walk from `lowest` to `highest` is two billion passes PER FRAME. MEASURED: with a desktop-level
		 * window and a normal one the server stopped compositing entirely, and the frame the harness waited for
		 * never came. Each pass now finds the NEXT level above the last one drawn, so the cost is the number of
		 * levels PRESENT and nothing else. */
		int have = 0, lastLevel = 0, barDrawn = 0;

		for (;;) {
			int level = 0, found = 0;
			wl_list_for_each(surface, &server->surfaces, link) {
				if (!surface->mapped) {
					continue;
				}
				if (have && surface->level <= lastLevel) {
					continue;
				}
				if (!found || surface->level < level) {
					level = surface->level;
					found = 1;
				}
			}
			if (!found) {
				break;
			}
			/* **THE BAR, BEFORE THE FIRST LEVEL AT OR ABOVE IT.** Everything already painted is BELOW the bar — the
			 * desktop, ordinary windows — and is covered by it; everything painted after (the application's own menu
			 * window and the status items, at or above this level) sits IN it. */
			if (!barDrawn && level >= AWS_MENU_BAR_LEVEL) {
				aws_output_draw_menu_bar(backend);
				barDrawn = 1;
			}
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
				/* **AND NOTHING THAT LIVES IN THE MENU BAR CASTS A SHADOW — ONE RULE, TWO CASES.** `agora-shell.xml` gives
				 * the server the shadow because a shadow falls on WHAT IS BEHIND, and the bar's own contents have nothing
				 * behind them: they are ON the strip the server paints. The two that do this are different in every other
				 * way and identical here — a STATUS ITEM (`NSStatusItem`, placed by `shell.c` because it has a length and
				 * no position, `status_length` non-zero) and THE APPLICATION'S OWN MENU TITLES (an ordinary client window
				 * that happens to be given the bar's rows). A shadow around either is a dark smudge INSIDE the bar.
				 *
				 * SO THE TEST IS THE GEOMETRY AND NOT THE KIND. The first version asked `status_length`, which caught the
				 * item and let the menu titles through — MEASURED, a shadow under the application's menu. Any surface
				 * lying ENTIRELY inside the bar's rows is part of the bar, whichever client drew it and whatever it is
				 * called.
				 *
				 * AND A SHADOW GOES DOWN BEFORE THE WINDOW DOES, which is the only order that means anything: it is a
				 * shadow ON THE THINGS BEHIND, so anything painted after it would be painted over it. */
				if (surface->y < 0 || surface->y + surface->frame_height > AWS_MENU_BAR_HEIGHT) {
					aws_output_draw_shadow(backend, surface->x, surface->y, surface->frame_width,
							       surface->frame_height, AWS_SHADOW_RADIUS);
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
			lastLevel = level;
			have = 1;
		}
		/* **A SCREEN WITH NO WINDOW AT OR ABOVE THE BAR STILL HAS A BAR.** A desktop-only session has nothing at
		 * `AWS_MENU_BAR_LEVEL`, so the walk above never reaches the draw. */
		if (!barDrawn) {
			aws_output_draw_menu_bar(backend);
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
