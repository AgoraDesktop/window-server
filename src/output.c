/*
 * output.c — the wl_output global, the composite ORDER, and the screenshot.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WHAT IS LEFT HERE AFTER THE RENDERER SPLIT, AND WHY IT IS THE RIGHT THING TO BE LEFT. This file used to be the
 * compositor: it walked the surfaces by level, drew shadows, blended client buffers and filled the menu bar. All of
 * that is DRAWING, and drawing is now a renderer's — `renderer-cpu.c` does it in a frame buffer and
 * `renderer-gl.c` does it in a GL context. What stays here is the part that is NOT drawing: **what** is composited,
 * **in what order**, and what every client is owed when a frame has happened.
 *
 * THE ORDER IS POLICY AND POLICY LIVES ONCE. A level walk answers "does the application's menu window sit above its
 * ordinary window", and a second copy of that loop in a second renderer is a second place for the answer to drift.
 * So `aws_output_walk` is the single walk, a renderer is called back for the bar and for each surface that is really
 * on the screen, and the CPU compositor and the GL one are indistinguishable from a client's point of view — which
 * is the acceptance test for this split.
 *
 * AND THE RELEASE IS NOT HERE AT ALL ANY MORE, BECAUSE IT WAS THE ONE THING THIS FILE DID THAT WAS NOT ITS OWN.
 * A buffer is released when the commit that replaces it arrives — `surface.c`, where the client's claim and the
 * compositor's are handed over — and never by a composite pass, which would be taking pixels it is still drawing.
 * See `aws_output_render` for what that cost and why it is now a client's swapchain.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wayland-server-protocol.h>

/* **A MILLISECOND ON THE COMPOSITOR'S OWN CLOCK, WHICH IS WHAT A FRAME CALLBACK CARRIES.** It is the same clock a
 * `wl_pointer` event uses, so a client lining up "the frame arrived" against "the pointer moved" is comparing one
 * thing. */
static uint32_t aws_now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u);
}

/* ---------------------------------------------------------------------------------------------------
 * the composite order
 * --------------------------------------------------------------------------------------------------- */

/* **THE ONE QUESTION BOTH HALVES OF THE COMPOSITOR ASK ABOUT A SHEET.** See `server.h` for what it means; the body is
 * three lines because the meaning is the interface: a sheet with a parent that is really on the screen has no place of
 * its own in the order. */
int aws_surface_drawn_with_parent(const struct aws_surface *surface)
{
	const struct aws_surface *parent;

	if (surface == NULL) {
		return 0;
	}
	parent = surface->sheet_parent;
	if (parent == NULL || parent == surface) {
		return 0;
	}
	return (parent->mapped && parent->visible && parent->current_buffer != NULL) ? 1 : 0;
}

/* **A SHEET IS DRAWN IMMEDIATELY ABOVE ITS PARENT, AND NOTHING CAN BE PUT BETWEEN THEM.** This is the one place the
 * pair's order is decided, and it is decided by ASKING rather than by maintaining a list: `-orderFront:` on a sheet
 * moves it to the top of the stack, a click on a third window moves that one above both, and a Dock's `activate_window`
 * on the parent moves the parent — every one of those reorders `server->surfaces`, and every one of them can leave a
 * window between a parent and its sheet if the list were the answer. It is not: a sheet whose parent is on the screen
 * has NO PLACE OF ITS OWN, so it is skipped where the list put it and emitted here, the moment its parent has been —
 * which makes "the sheet is the next thing above its parent" true by construction rather than by upkeep.
 *
 * A CHAIN IS BOUNDED. One sheet per window is the AppKit's rule and the server does not police it, so a client that
 * attached a chain would be drawn as one; the depth cap is what stops a cycle (a client CAN name its own parent back)
 * from turning a frame into a hang. */
static void walk_emit_sheets(struct aws_server *server, struct aws_surface *parent, aws_output_emit_fn emit,
			     uint32_t time, const struct aws_box *box, int depth)
{
	struct aws_surface *surface;

	if (depth > 8) {
		return;
	}
	wl_list_for_each(surface, &server->surfaces, link) {
		if (surface->sheet_parent != parent || !surface->mapped || !surface->visible) {
			continue;
		}
		emit(server, surface, time, box);
		walk_emit_sheets(server, surface, emit, time, box, depth + 1);
	}
}

void aws_output_walk(struct aws_server *server, aws_output_emit_fn emit, const struct aws_box *box)
{
	struct aws_surface *surface;
	uint32_t time = aws_now_ms();
	/* **AND THE WALK IS OVER THE LEVELS THAT ARE PRESENT, NOT OVER EVERY NUMBER BETWEEN THEM.** A level is an
	 * arbitrary integer, and the desktop's is `INT32_MIN + 20` while an ordinary window's is 0 — so a walk from
	 * `lowest` to `highest` is two billion passes PER FRAME. MEASURED: with a desktop-level window and a normal
	 * one the server stopped compositing entirely, and the frame the harness waited for never came. Each pass
	 * now finds the NEXT level above the last one drawn, so the cost is the number of levels PRESENT and nothing
	 * else. */
	int have = 0, lastLevel = 0, barDrawn = 0;

	for (;;) {
		int level = 0, found = 0;

		wl_list_for_each(surface, &server->surfaces, link) {
			if (!surface->mapped) {
				continue;
			}
			/* **AND A SHEET HAS NO LEVEL OF ITS OWN IN THE WALK, BECAUSE IT IS DRAWN WITH ITS PARENT.** Skipping it
			 * here is what keeps a sheet at a higher level (an `NSAlert`'s panel is at `NSModalPanelWindowLevel`)
			 * from costing an empty pass of its own — and, more importantly, from making the menu bar's row be drawn
			 * before the pair it belongs above. */
			if (aws_surface_drawn_with_parent(surface)) {
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
			emit(server, NULL, time, box);
			barDrawn = 1;
		}
		wl_list_for_each(surface, &server->surfaces, link) {
			/* **AND A WINDOW THE APPLICATION HAS TAKEN OFF THE SCREEN IS NOT COMPOSITED.** `-orderOut:` is a real
			 * door, and without this the server painted every window a client had ever hidden: MEASURED, closing a
			 * menu left its panel on the glass, and the ones behind it accumulated until the run loop was repainting
			 * a stack of dead menus every pass. Its buffer is left alone: a hidden window's last frame is still its
			 * content, and the commit that replaces it is what gives it back (see `surface_commit`). */
			/* **AND A SHEET IS NOT DRAWN IN ITS OWN PASS AT ALL** — it is emitted by the pass that draws its parent,
			 * the moment the parent has been drawn. See `walk_emit_sheets`. */
			if (aws_surface_drawn_with_parent(surface)) {
				continue;
			}
			if (!surface->mapped || !surface->visible || surface->level != level) {
				/* **A SURFACE WITH A BUFFER THAT IS BEING SKIPPED IS A CLIENT WAITING FOR EVER.** WHICH GUARD DID
				 * IT is the question, and this names it — but only for a surface at THIS pass's own level, because
				 * firing for every other level printed sixty thousand lines of ordinary behaviour. MEASURED. */
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
			emit(server, surface, time, box);
			walk_emit_sheets(server, surface, emit, time, box, 0);
		}
		lastLevel = level;
		have = 1;
	}
	/* **A SCREEN WITH NO WINDOW AT OR ABOVE THE BAR STILL HAS A BAR.** A desktop-only session has nothing at
	 * `AWS_MENU_BAR_LEVEL`, so the walk above never reaches the draw. */
	if (!barDrawn) {
		emit(server, NULL, time, box);
	}
}

/* ---------------------------------------------------------------------------------------------------
 * the frame, and who draws it
 * --------------------------------------------------------------------------------------------------- */

/* **A FRAME THAT PAINTS NOTHING STILL OWES THE CALLBACKS.** `wl_surface.frame` is a promise, and a client that attaches,
 * commits and waits for one does not draw again until it arrives — so a frame skipped because nothing is damaged must
 * still complete what the walk would have completed. The set is the walk's own condition (mapped, visible, a buffer),
 * and it is spelled out here rather than by walking with a do-nothing emitter because the walk would also draw. */
void aws_output_complete_frames(struct aws_server *server)
{
	struct aws_surface *surface;
	uint32_t time = aws_now_ms();

	wl_list_for_each(surface, &server->surfaces, link) {
		if (surface->mapped && surface->visible && surface->current_buffer != NULL) {
			aws_surface_frame_done(surface, time);
		}
	}
}

/* **WHAT THIS FRAME IS FOR, IN ONE LINE OF TEXT.** A damage bug is invisible in a screenshot of a static window — the
 * window is right, the pixels around it are stale — and obvious in a line of text, so the diagnostic prints the box the
 * frame painted and, for a full frame, the trigger that asked for it. It is compiled in and gated on the environment,
 * like `AWS_RENDER_DEBUG`, because the alternative is a build nobody can reproduce a report against. */
static void damage_report(const char *what, int surfaces, const struct aws_box *box, const char *reason)
{
	if (getenv("WINDOW_SERVER_DAMAGE_DIAG") == NULL) {
		return;
	}
	if (strcmp(what, "full") == 0) {
		fprintf(stderr, "damage: FULL (%s)\n", reason != NULL ? reason : "a render was requested");
	} else if (strcmp(what, "box") == 0) {
		fprintf(stderr, "damage: %d surface(s), box %dx%d at (%d,%d)\n", surfaces, box->width,
			box->height, box->x, box->y);
	} else {
		fprintf(stderr, "damage: nothing to paint, no frame\n");
	}
}

int aws_output_render(struct aws_server *server)
{
	struct aws_box box;
	int full = 0;
	const char *reason = NULL;
	int surfaces;
	int rendered = 1;

	/* **THE LEDGERS ARE CONSUMED HERE, ONCE, WHATEVER HAPPENS NEXT.** A frame that is skipped does not keep the
	 * rectangles for later: they described the buffer as it was committed, and a later frame's buffer is a different
	 * picture. */
	surfaces = aws_damage_frame_begin(server, &box, &full, &reason);
	if (!full) {
		aws_box_clip(&box, server->backend.width, server->backend.height);
		if (aws_box_empty(&box)) {
			rendered = 0;
		}
	}
	if (!rendered) {
		aws_output_complete_frames(server);
		damage_report("nothing", 0, &box, reason);
		return 0;
	}
	/* **THE RENDERER'S, AND NOTHING ELSE'S.** A server always has one — `aws_renderer_init` guarantees a CPU
	 * renderer even when a GL context could not be made — so a NULL here is a lifecycle bug rather than a
	 * configuration, and drawing nothing is the honest response to it. **NULL MEANS THE WHOLE OUTPUT**, and a box means
	 * only that box; no renderer may draw less than either. */
	if (server->renderer != NULL && server->renderer->draw != NULL) {
		server->renderer->draw(server, full ? NULL : &box);
	}
	damage_report(full ? "full" : "box", surfaces, &box, reason);
	/* **THE FRAME IS SPENT, so the next one starts from nothing.** `damage_full` is a statement about ONE frame; leaving
	 * it set would make every frame full forever, which is the bug this whole file exists to avoid. */
	server->damage_full = 0;
	server->damage_reason = NULL;
	/* **AND NO BUFFER IS RELEASED HERE, BECAUSE A SURFACE'S CONTENT IS NOT THIS PASS'S TO GIVE UP.** This used to
	 * release every surface's buffer after every composite, which reads as harmless and is not: a release is a
	 * promise that the compositor has finished with those pixels, so the client repaints them in place — and the
	 * compositor went on DRAWING them, because a released buffer is still the surface's `current_buffer`. MEASURED,
	 * as the blink when the keyboard moved between two windows: a composite landed in the middle of a whole-frame
	 * repaint and put a half-drawn window on the glass, in both windows at once, for as long as the paint took
	 * (about eight captured frames at 60 Hz).
	 *
	 * THE RELEASE BELONGS WHERE THE COMPOSITOR'S CLAIM ON THE PIXELS ENDS, and that is `surface_commit`: a buffer
	 * is given back when the commit that replaces it arrives, when the client detaches it, and never while it is
	 * what the screen is showing. A client that wants to paint while its last frame is still on the glass keeps a
	 * second buffer for it — the swapchain every Wayland client has — which is what `AGWindowServer-Wayland.c`
	 * now does. A release from here would be this pass taking a buffer it is still about to draw. */
	return 1;
}

/* ---------------------------------------------------------------------------------------------------
 * the screenshot, which is this version's only way of being checked
 * --------------------------------------------------------------------------------------------------- */

/* PPM AND NOT PNG: the point is to be checkable, and a binary PPM is four lines of header and the pixels.
 * `tests/inspect.py` reads it with the same image library it reads a screenshot from any other compositor —
 * which is what makes the two comparable.
 *
 * **AND THE GL RENDERER FILLS THE SAME BUFFER BEFORE IT SWAPS**, because a screenshot is a question about what was
 * composited and not about who composited it: `glReadPixels` puts the frame back into `backend->pixels` when
 * `--screenshot` asked for one, so the CPU and GL paths answer the same file. See `renderer-gl.c`. */
int aws_output_write_ppm(struct aws_server *server, const char *path)
{
	struct aws_backend *backend = &server->backend;
	FILE *file = fopen(path, "wb");

	if (file == NULL || backend->pixels == NULL) {
		if (file != NULL) {
			fclose(file);
		}
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
	/* **THE MODE IS THE LOGICAL SIZE AND THE SCALE IS THE FACTOR BETWEEN IT AND THE FRAMEBUFFER.** A client composes
	 * for `mode` pixels and reads `scale` to decide whether to render at 2x — which is exactly the pair a HiDPI
	 * client needs, and the reason the mode is not the framebuffer's size: a 2560-device-pixel output at scale 2 is a
	 * 1280-pixel screen, and telling the client 2560 would make every window half the size it should be. */
	wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
			    (int32_t)aws_output_width(server), (int32_t)aws_output_height(server), 60000);
	if (wl_resource_get_version(resource) >= 2) {
		/* **THE INTEGER FORM, WHICH IS THE FORM THE PROTOCOL HAS.** A 1.5x output is advertised as 2: the client
		 * renders at 2x and the compositor downsamples 2 -> 1.5, which is sharp. Telling it 1.5 is not possible, and
		 * telling it 1 would be a client rendering at the wrong resolution on purpose. */
		wl_output_send_scale(resource, aws_output_scale_integer(server));
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
