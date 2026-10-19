/*
 * shell.c — agora_shell: the window management the Mac has, implemented.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
 *
 * THE CONTRACT IS `protocol/agora-shell.xml` AND THIS IS THE SERVER'S HALF OF IT. Three things it adds to what
 * `xdg-shell` can say, each measured against Apple's headers before it was written (see DESIGN.md):
 *
 *   1. THE APPLICATION STATES ITS FRAME AND IS HONOURED. `set_frame` writes down where a window goes and this
 *      file never second-guesses it. There is no cascade here, no tiling, and no adjustment — because on the Mac
 *      the only thing that adjusts a frame is `constrainFrameRect:toScreen:`, an NSWindow method, which is the
 *      APPLICATION's to do against a work area it is told.
 *
 *   2. ORDERING IS AN INTEGER LEVEL. `set_level` writes down an `NSWindow` level; the compositing loop walks
 *      levels in order and the mapping order inside one, which is what lets a menu window and a status item and
 *      the Dock be ordered against each other without this file knowing what any of them are.
 *
 *   3. THE WORK AREA IS THE SERVER'S TO SAY. A menu bar and a Dock are furniture the SERVER owns, so only it
 *      knows what is left of a screen — which is `NSScreen.visibleFrame`, and the reason an application cannot
 *      compute it for itself.
 *
 * WHAT IS DELIBERATELY NOT HERE: any notion of what a level MEANS. `kCGMainMenuWindowLevel` is a number an
 * application chooses; the server orders by it and does not care which number is the menu bar. A special case
 * for "the dock" or "the menu bar" in this file would be the server doing a toolkit's job, and the levels exist
 * so that it never has to.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agora-shell-server-protocol.h"

/* ---------------------------------------------------------------------------------------------------
 * the furniture the server owns
 * --------------------------------------------------------------------------------------------------- */

/* **A MENU BAR AND A DOCK ARE POLICY, AND POLICY IS WHY AN APPLICATION CANNOT COMPUTE ITS OWN WORK AREA.** These
 * two numbers are the whole of it for now: a strip at the top for the menu bar, and nothing at the bottom until
 * the Dock exists as a client that can say how much it wants. They are constants rather than configuration
 * because there is nothing yet to configure them from, and they are named so that the day there is, there is one
 * place to look. */
/* `AWS_MENU_BAR_HEIGHT` lives in server.h, because the compositing loop fills a strip that tall and the two must
 * not disagree. The Dock's reserve is still zero: the Dock is a CLIENT that has to ask, and nothing asks yet. */
#define AWS_DOCK_RESERVE 0

void aws_shell_reserved(struct aws_server *server, int *x, int *y, int *width, int *height)
{
	int top = AWS_MENU_BAR_HEIGHT;
	int bottom = AWS_DOCK_RESERVE;

	/* THE WORK AREA IS THE OUTPUT MINUS THE FURNITURE, in the output's own coordinates. */
	*x = 0;
	*y = top;
	*width = server->backend.width;
	*height = server->backend.height - top - bottom;
	if (*height < 0) {
		*height = 0;
	}
}

/* **THE MENU BAR'S USABLE REGION: THE STRIP MINUS THE SERVER'S OWN PART.** The server owns the bar and the System
 * menu's square; an application's own menus begin where that square ends and run to the output's right edge. It is
 * sent beside the work area because a client that has just asked where it may put things is asking both questions
 * at once — and they are two questions: the work area is a VERTICAL reserve for windows, and this is a HORIZONTAL
 * one for the bar. */
void aws_shell_menu_bar_area(struct aws_server *server, int *x, int *y, int *width, int *height)
{
	/* **THE SQUARE, THE AIR, AND THEN THE APPLICATION'S MENUS.** The gap is added here rather than folded into
	 * `AWS_SYSTEM_MENU_WIDTH` so that the two questions stay apart: how wide the System menu's own region is, and
	 * how much room the bar leaves after it. MEASURED before this: the glyph ended at x = 21 and the first
	 * application title began at x = 31, ten pixels of air in a bar that is one surface. */
	*x = AWS_SYSTEM_MENU_WIDTH + AWS_SYSTEM_MENU_GAP;
	*y = 0;
	*width = server->backend.width - *x;
	*height = AWS_MENU_BAR_HEIGHT;
	if (*width < 0) {
		*width = 0;
	}
}

/* ---------------------------------------------------------------------------------------------------
 * agora_output
 * --------------------------------------------------------------------------------------------------- */

static void agora_output_destroy_resource(struct wl_resource *resource)
{
	(void)resource;
}

static void bind_agora_output(struct wl_client *client, void *data, uint32_t version, uint32_t id,
			      struct wl_resource *output_resource)
{
	struct aws_server *server = data;
	struct wl_resource *resource = wl_resource_create(client, &agora_output_interface, version, id);
	int x, y, width, height;

	(void)output_resource;
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	/* ONE OUTPUT, SO WHICH ONE WAS ASKED FOR IS NOT A QUESTION YET — and when it is, the answer will be to keep
	 * this per `wl_output` rather than to guess here. */
	aws_shell_reserved(server, &x, &y, &width, &height);
	/* SENT ON BIND, because a client that has just asked for the work area is a client that is about to place
	 * itself against it. */
	agora_output_send_work_area(resource, x, y, width, height);
	aws_shell_menu_bar_area(server, &x, &y, &width, &height);
	/* **AND WHAT COLOUR IT IS**, because the application's menus are drawn in this bar and a menu drawn on a panel
	 * of its own colour is a seam across what is meant to be one surface. */
	agora_output_send_menu_bar_area(resource, x, y, width, height, AWS_MENU_BAR_COLOUR & 0xFFFFFFu);
	/* **AN EVENT-ONLY INTERFACE HAS NO IMPLEMENTATION STRUCT**, which is not an inconvenience but the generated
	 * header telling the truth: the server never RECEIVES anything on this object, so there is no vtable to
	 * fill and `NULL` is what says so. */
	wl_resource_set_implementation(resource, NULL, server, agora_output_destroy_resource);
}

/* ---------------------------------------------------------------------------------------------------
 * agora_window
 * --------------------------------------------------------------------------------------------------- */

static void window_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

/* **THE ITEMS, PACKED FROM THE RIGHT EDGE LEFTWARDS — THE MAC'S ORDER.** The first item made is the RIGHTMOST,
 * which is why the walk is in MAPPING order: that list is the stacking order, and an item nobody raised keeps the
 * order it was made in. Each is as tall as the menu bar, because a status item IS the menu bar's height by
 * definition — a client that could say otherwise could put a window under the bar it is drawn in. */
void aws_shell_place_status_items(struct aws_server *server)
{
	struct aws_surface *surface;
	/* The screen's size is the BACKEND's — `struct aws_output` is only the wl_output resource. */
	int right = server->backend.width;

	wl_list_for_each(surface, &server->surfaces, link) {
		if (surface->status_length <= 0) {
			continue;
		}
		right -= surface->status_length;
		if (right < 0) {
			right = 0;	/* an item wider than the screen goes to the left edge and is clipped there */
		}
		surface->x = right;
		surface->y = 0;
		surface->frame_width = surface->status_length;
		surface->frame_height = AWS_MENU_BAR_HEIGHT;
	}
	aws_server_schedule_render(server);
	/* AN ITEM JUST MOVED, so the surface under the pointer may be a different one now. */
	aws_seat_pointer_recheck(server);
}

/* **THE APPLICATION'S FRAME, WRITTEN DOWN AND HONOURED.** Not a hint, not a request to be negotiated: this is the
 * Mac's `setFrame:display:` and the server's whole part in it is remembering the answer. */
static void window_set_frame(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
			     int32_t width, int32_t height)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (surface == NULL) {
		return;
	}
	surface->x = (int)x;
	surface->y = (int)y;
	surface->frame_width = (int)width;
	surface->frame_height = (int)height;
	/* AND A STATUS ITEM IS PUT BACK WHERE IT BELONGS. `set_frame` is not how an item is placed, so a client that
	 * calls it anyway does not get to move one — the server's answer wins because there is no other answer it
	 * could have. */
	if (surface->status_length > 0) {
		aws_shell_place_status_items(surface->server);
		return;
	}
	aws_server_schedule_render(surface->server);
	aws_seat_pointer_recheck(surface->server);
}

/* **THE ONE KIND OF WINDOW THE SERVER PLACES, AND THE HEADER SAYS WHY.** `NSStatusItem` has a LENGTH and no
 * POSITION: an application does not know what else is in the status bar, so it cannot know where its own item
 * goes. The application says how long; the server packs. That is why this is a KIND of window and not a frame.
 *
 * `length` is a width in output coordinates — the two sentinel lengths AppKit has are its own business. Zero or
 * less takes the window back OUT of the status area, and it is an ordinary window again, placed by its frame. */
static void window_set_status_item(struct wl_client *client, struct wl_resource *resource, int32_t length)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (surface == NULL) {
		return;
	}
	surface->status_length = (int)length > 0 ? (int)length : 0;
	aws_shell_place_status_items(surface->server);
}

/* **AND WHETHER IT IS ON THE SCREEN AT ALL.** Not a hint and not negotiated: the application says and the server
 * writes it down, exactly as it does with the frame and the level. */
static void window_set_visible(struct wl_client *client, struct wl_resource *resource, int32_t visible)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (surface == NULL) {
		return;
	}
	surface->visible = visible ? 1 : 0;
	aws_server_schedule_render(surface->server);
	/* SHOWN OR HIDDEN, the set of surfaces the pointer can be over has changed. */
	aws_seat_pointer_recheck(surface->server);
	/* **AND THE KEYBOARD MOVES OFF A WINDOW THAT WAS JUST HIDDEN, OR BACK ONTO ONE THAT WAS SHOWN WHILE NOTHING HAD
	 * IT.** Either way a visible window is key, so a desktop is never left with no menu bar. */
	aws_seat_reassign_keyboard_focus(surface->server);
}

/* AN INTEGER, AND THE SERVER DOES NOT CARE WHICH ONE. The level names belong to the window server's vocabulary
 * and are chosen by the application; what this file does with the number is order by it. */
static void window_set_level(struct wl_client *client, struct wl_resource *resource, int32_t level)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (surface == NULL) {
		return;
	}
	surface->level = (int)level;
	aws_server_schedule_render(surface->server);
	aws_seat_pointer_recheck(surface->server);
}

/* RAISE AND LOWER MOVE A WINDOW WITHIN ITS LEVEL, which is why they are a list operation and not a level
 * change: `-orderFront:` is not `-setLevel:`, and a window raised does not join the level above it. */
static void window_raise(struct wl_client *client, struct wl_resource *resource)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (surface == NULL) {
		return;
	}
	wl_list_remove(&surface->link);
	wl_list_insert(surface->server->surfaces.prev, &surface->link);
	aws_server_schedule_render(surface->server);
	aws_seat_pointer_recheck(surface->server);
}

static void window_lower(struct wl_client *client, struct wl_resource *resource)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (surface == NULL) {
		return;
	}
	wl_list_remove(&surface->link);
	wl_list_insert(&surface->server->surfaces, &surface->link);
	aws_server_schedule_render(surface->server);
	aws_seat_pointer_recheck(surface->server);
}

/* **THE POINTER FALLBACK A POPUP NEEDS.** While this is set, a pointer event that would go to NO surface — the
 * pointer is over the desktop — is delivered to THIS one instead, with coordinates relative to it, so a menu can
 * hear the click that lands outside it and dismiss. It is NOT a real grab: a surface under the pointer still gets
 * the event, because the menu bar is a separate window from the menu and the pointer has to be able to cross back
 * to it. Destroying the surface releases it too (see `aws_seat_surface_gone`). */
static void window_set_pointer_grab(struct wl_client *client, struct wl_resource *resource, int32_t grab)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (surface == NULL) {
		return;
	}
	surface->pointer_grab = grab ? 1 : 0;
	if (grab) {
		/* ONE AT A TIME: a second menu taking the fallback takes it FROM the first, which is what a client
		 * opening a new popup before closing the last is asking for. */
		surface->server->pointer_grab = surface;
	} else if (surface->server->pointer_grab == surface) {
		surface->server->pointer_grab = NULL;
	}
	/* THE FOCUS IS RE-DECIDED NOW, because whether the pointer is over a surface does not change but WHERE an
	 * event would go does: taking the fallback while the pointer is already over the desktop must deliver to the
	 * surface that just asked, and giving it back must stop. */
	aws_seat_pointer_recheck(surface->server);
}

static void window_set_title(struct wl_client *client, struct wl_resource *resource, const char *title)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (surface == NULL) {
		return;
	}
	free(surface->title);
	surface->title = strdup(title != NULL ? title : "");
}

static void window_set_app_id(struct wl_client *client, struct wl_resource *resource, const char *app_id)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (surface == NULL) {
		return;
	}
	free(surface->app_id);
	surface->app_id = strdup(app_id != NULL ? app_id : "");
}

/* **AND THE ONE A DRAG TAKES, WHICH IS A REAL GRAB.** See the protocol's `set_pointer_capture`: while this is
 * set the pointer goes to THIS surface wherever it is, because the window a hand is resizing moves out from under
 * it. One at a time, like the fallback above; destroying the surface, releasing the last button, or asking with
 * zero gives it back. */
static void window_set_pointer_capture(struct wl_client *client, struct wl_resource *resource, int32_t capture)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (surface == NULL) {
		return;
	}
	surface->pointer_capture = capture ? 1 : 0;
	if (capture) {
		surface->server->pointer_capture = surface;
	} else if (surface->server->pointer_capture == surface) {
		surface->server->pointer_capture = NULL;
	}
	/* THE FOCUS IS RE-DECIDED NOW, for the same reason the fallback does it: whether an event is delivered here
	 * does not depend on where the pointer is any more, so a hold taken over one surface has to take the pointer
	 * from whatever had it before the next motion arrives. */
	aws_seat_pointer_recheck(surface->server);
}

static const struct agora_window_interface agora_window_implementation = {
	window_set_frame,
	window_set_status_item,
	window_set_level,
	window_raise,
	window_lower,
	window_set_title,
	window_set_app_id,
	window_set_visible,	/* the protocol's order: this door is declared just before `set_pointer_grab` */
	window_set_pointer_grab,
	window_set_pointer_capture,
	window_destroy,
};

static void agora_window_destroy_resource(struct wl_resource *resource)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	/* THE ROLE GOES WITH THE OBJECT, and the SURFACE STAYS: the contract says a surface may be given another
	 * role afterwards, which is what `destroy` means here rather than destroying the surface too. */
	if (surface != NULL) {
		surface->agora_window = NULL;
	}
}

static void shell_get_window(struct wl_client *client, struct wl_resource *resource, uint32_t id,
			     struct wl_resource *surface_resource)
{
	struct aws_surface *surface = wl_resource_get_user_data(surface_resource);
	struct wl_resource *window;

	if (surface == NULL) {
		return;
	}
	/* **ONE ROLE PER SURFACE, AND THE TWO PROTOCOLS DISAGREE ABOUT WHO DECIDES THE SIZE** — so a surface that
	 * is already an `xdg_toplevel` cannot also be an `agora_window`, and saying so is kinder than a surface
	 * that quietly obeys two contradictory models. */
	if (surface->xdg_surface != NULL || surface->xdg_toplevel != NULL || surface->agora_window != NULL) {
		wl_resource_post_error(resource, AGORA_SHELL_ERROR_ALREADY_HAS_ROLE,
				       "this surface already has a role");
		return;
	}
	window = wl_resource_create(client, &agora_window_interface, wl_resource_get_version(resource), id);
	if (window == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	surface->agora_window = window;
	wl_resource_set_implementation(window, &agora_window_implementation, surface,
				       agora_window_destroy_resource);
}

/* **THE POINTER'S ABSOLUTE POSITION, WHICH A CLIENT-SIDE DRAG NEEDS AND CANNOT DERIVE.** Sent alongside the
 * relative `wl_pointer.motion` (see the protocol's `pointer_global` for the measured reason), and a no-op for a
 * surface with no agora window — one that is still an `xdg_toplevel`, or has no role yet. */
void aws_window_send_pointer_global(struct aws_surface *surface, int x, int y)
{
	if (surface == NULL || surface->agora_window == NULL) {
		return;
	}
	agora_window_send_pointer_global(surface->agora_window, x, y);
}

static void shell_get_output(struct wl_client *client, struct wl_resource *resource, uint32_t id,
			     struct wl_resource *output_resource)
{
	bind_agora_output(client, wl_resource_get_user_data(resource), wl_resource_get_version(resource), id,
			  output_resource);
}

static const struct agora_shell_interface agora_shell_implementation = {
	shell_get_window,
	shell_get_output,
};

static void bind_agora_shell(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client, &agora_shell_interface, version, id);

	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &agora_shell_implementation, data, NULL);
}

void aws_shell_init(struct aws_server *server)
{
	wl_global_create(server->display, &agora_shell_interface, 1, server, bind_agora_shell);
}
