/*
 * shell.c — agora_shell: the window management the Mac has, implemented.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
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
 *   3. THE WORK AREA IS THE SERVER'S TO SAY. A menu bar and a Dock are furniture, and only the server can answer
 *      what is LEFT of a screen — which is `NSScreen.visibleFrame`, and the reason an application cannot compute it
 *      for itself. The menu bar's strip is the server's own; the Dock's share is a CLIENT's request
 *      (`set_reserved`), because how tall a Dock is is not this file's to decide.
 *
 * WHAT IS DELIBERATELY NOT HERE: any notion of what a level MEANS. `kCGMainMenuWindowLevel` is a number an
 * application chooses; the server orders by it and does not care which number is the menu bar. A special case
 * for "the dock" or "the menu bar" in this file would be the server doing a toolkit's job, and the levels exist
 * so that it never has to. The same discipline is why a reservation carries an EDGE and a LENGTH rather than a
 * name: a client that says "the bottom 96 pixels are mine" gets the Dock without the server knowing what a Dock is.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agora-shell-server-protocol.h"

/* ---------------------------------------------------------------------------------------------------
 * the furniture the server owns
 * --------------------------------------------------------------------------------------------------- */

/* **A MENU BAR AND A DOCK ARE POLICY, AND POLICY IS WHY AN APPLICATION CANNOT COMPUTE ITS OWN WORK AREA.** The
 * server's own share is one strip at the top for the menu bar. **THE DOCK'S SHARE IS NOT A CONSTANT AND IS NOT
 * HERE ANY MORE**: the Dock is a CLIENT, it says how much it wants with `agora_window.set_reserved`, and this
 * file sums what the clients reserving each edge have asked for. A second constant for the Dock would be the
 * server deciding how tall a Dock is — which is a client's answer, and the one the protocol's `set_reserved`
 * exists to carry. */
/* `AWS_MENU_BAR_HEIGHT` lives in server.h, because the compositing loop fills a strip that tall and the two must
 * not disagree. */
void aws_shell_reserved(struct aws_server *server, int *x, int *y, int *width, int *height)
{
	struct aws_surface *surface;
	int top = AWS_MENU_BAR_HEIGHT;
	int bottom = 0;
	int left = 0;
	int right = 0;

	/* **EVERY CLIENT THAT RESERVED SOMETHING, SUMMED BY EDGE.** Additive rather than "the largest wins", because
	 * two panels on one edge each occupy their own strip; see `set_reserved` in `agora-shell.xml`. */
	wl_list_for_each(surface, &server->surfaces, link) {
		if (surface->reserved_amount <= 0) {
			continue;
		}
		switch (surface->reserved_edge) {
		case AGORA_WINDOW_EDGE_TOP:    top += surface->reserved_amount; break;
		case AGORA_WINDOW_EDGE_BOTTOM: bottom += surface->reserved_amount; break;
		case AGORA_WINDOW_EDGE_LEFT:   left += surface->reserved_amount; break;
		case AGORA_WINDOW_EDGE_RIGHT:  right += surface->reserved_amount; break;
		}
	}

	/* THE WORK AREA IS THE OUTPUT MINUS THE FURNITURE, in the output's own coordinates. */
	*x = left;
	*y = top;
	*width = server->backend.width - left - right;
	*height = server->backend.height - top - bottom;
	if (*width < 0) {
		*width = 0;
	}
	if (*height < 0) {
		*height = 0;
	}
}

/* **AND TELLING EVERYONE WHEN IT MOVED, WHICH IS THE HALF THAT WAS MISSING.** The work area used to be a constant
 * (menu bar height, nothing else), so a value sent at bind was true for ever and there was nothing to announce.
 * A client reserving a strip makes it a value that CHANGES, and the Mac's answer to a changed work area is not
 * that the server moves anybody's window — it is that `constrainFrameRect:toScreen:` runs again in the
 * application. That only works if the application is TOLD, which is what this sends: `work_area` to every bound
 * output (which is `NSScreen.visibleFrame`) and `work_area_changed` to every window. */
void aws_shell_reserved_changed(struct aws_server *server)
{
	struct aws_agora_output *output, *tmp;
	struct aws_surface *surface;
	int x, y, width, height;

	aws_shell_reserved(server, &x, &y, &width, &height);
	wl_list_for_each_safe(output, tmp, &server->agora_outputs, link) {
		agora_output_send_work_area(output->resource, x, y, width, height);
	}
	wl_list_for_each(surface, &server->surfaces, link) {
		if (surface->agora_window != NULL) {
			agora_window_send_work_area_changed(surface->agora_window, x, y, width, height);
		}
	}
}

/* **AND RELEASING ONE, WHICH IS TWO CALLERS WITH ONE MEANING.** A window's role destroyed, or its surface gone:
 * either way the reservation was that surface's and it is not the screen's any more. A surface with nothing
 * reserved is left alone, so a window that never asked does not make the server announce a change that did not
 * happen. */
void aws_shell_release_reserve(struct aws_surface *surface)
{
	if (surface == NULL || surface->reserved_amount <= 0) {
		return;
	}
	surface->reserved_amount = 0;
	aws_shell_reserved_changed(surface->server);
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

/* **THE OUTPUT TRACKED FOR BROADCAST.** An `agora_output` is event-only, so there is no implementation struct to
 * hang it on — but a work-area change has to reach EVERY one of them, and that needs a list. The wrapper is the
 * list's (`struct aws_agora_output`, in server.h because the list is the server's); the resource is the client's. */
static void agora_output_destroy_resource(struct wl_resource *resource)
{
	struct aws_agora_output *output = wl_resource_get_user_data(resource);

	if (output != NULL) {
		wl_list_remove(&output->link);
		free(output);
	}
}

static void bind_agora_output(struct wl_client *client, void *data, uint32_t version, uint32_t id,
			      struct wl_resource *output_resource)
{
	struct aws_server *server = data;
	struct aws_agora_output *output;
	struct wl_resource *resource;
	int x, y, width, height;

	(void)output_resource;
	/* **THIS ONE'S VERSION IS ITS OWN.** It used to inherit the shell's, which was harmless while both were 1 and
	 * is not once they differ: an interface asked for at a version above its own breaks `since` on its events.
	 * The cap is what makes "the output grew no events" expressible instead of political. */
	if (version > (uint32_t)agora_output_interface.version) {
		version = (uint32_t)agora_output_interface.version;
	}
	resource = wl_resource_create(client, &agora_output_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	output = calloc(1, sizeof *output);
	if (output == NULL) {
		wl_resource_destroy(resource);
		wl_client_post_no_memory(client);
		return;
	}
	output->resource = resource;
	wl_list_insert(server->agora_outputs.prev, &output->link);
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
	 * fill and `NULL` is what says so. The user data is the list's wrapper instead, because the work area can
	 * change and every bound output has to hear it. */
	wl_resource_set_implementation(resource, NULL, output, agora_output_destroy_resource);
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
	/* AND A DOCK IS TOLD, because "visible" is one of the fields it reads. */
	aws_shell_window_list_changed(surface->server);
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
	aws_shell_window_list_changed(surface->server);
}

/* RAISE AND LOWER MOVE A WINDOW WITHIN ITS LEVEL, which is why they are a list operation and not a level
 * change: `-orderFront:` is not `-setLevel:`, and a window raised does not join the level above it. */
static void surface_bring_to_front(struct aws_server *server, struct aws_surface *surface)
{
	wl_list_remove(&surface->link);
	wl_list_insert(server->surfaces.prev, &surface->link);
}

static void window_raise(struct wl_client *client, struct wl_resource *resource)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (surface == NULL) {
		return;
	}
	surface_bring_to_front(surface->server, surface);
	aws_server_schedule_render(surface->server);
	aws_seat_pointer_recheck(surface->server);
	aws_shell_window_list_changed(surface->server);
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
	aws_shell_window_list_changed(surface->server);
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
	aws_shell_window_list_changed(surface->server);
}

/* **AND THE ONE A DRAG TAKES, WHICH IS A REAL GRAB.** See the protocol's `set_pointer_capture`: while this is
 * set the pointer goes to THIS surface wherever it is, because the window a hand is resizing moves out from under
 * it. One at a time, like the fallback above; destroying the surface, releasing the last button, or asking with
 * zero gives it back. */
static void window_set_reserved(struct wl_client *client, struct wl_resource *resource, uint32_t edge, int32_t amount)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);
	int was_reserved;

	(void)client;
	if (surface == NULL) {
		return;
	}
	/* AN UNKNOWN EDGE IS A CLIENT BUG, not something to guess at: a reserve on the wrong side is a window under
	 * the thing that reserved it, which is worse than a protocol error that names it. */
	if (edge > AGORA_WINDOW_EDGE_RIGHT) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_METHOD,
				       "set_reserved: no such edge %u", edge);
		return;
	}
	was_reserved = surface->reserved_amount > 0;
	surface->reserved_edge = (int)edge;
	surface->reserved_amount = amount > 0 ? (int)amount : 0;
	/* AND THE SCREEN IS ANNOUNCED ONLY IF IT ACTUALLY CHANGED, which is why a window that never reserved
	 * anything and sets 0 does not make every client re-constrain itself for nothing. */
	if (was_reserved || surface->reserved_amount > 0) {
		aws_shell_reserved_changed(surface->server);
	}
}

static void window_set_activatable(struct wl_client *client, struct wl_resource *resource, int32_t activatable)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (surface == NULL) {
		return;
	}
	surface->activatable = activatable ? 1 : 0;
	/* **AND IF IT HAD THE KEYBOARD, IT GIVES IT UP.** A window that switches itself off while it is in front
	 * would otherwise keep the focus it is no longer allowed to take — the one ordering a caller cannot do for
	 * itself, because it does not know who else is on the screen. */
	if (!surface->activatable && surface->server->keyboard_focus == surface) {
		aws_seat_reassign_keyboard_focus(surface->server);
	}
}

/* **THE DOOR AN APPLICATION NEEDS FOR ITS OWN KEYBOARD, WHICH ONLY A CLICK OR A SHELL COULD GIVE IT BEFORE.**
 * `-makeKeyWindow` is the application saying which of its windows receives the keys; the seat is the server's, so
 * the sentence has to cross. It is `activate_window` without the raise and without the permission to name somebody
 * else — a client can only ever say this about a window it owns, because the object is its own. */
static void window_take_focus(struct wl_client *client, struct wl_resource *resource)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (surface == NULL) {
		return;
	}
	/* THE SEAT REFUSES A NON-ACTIVATABLE WINDOW AND A NO-OP REPEAT on its own — see `aws_seat_set_keyboard_focus`. */
	aws_seat_set_keyboard_focus(surface->server, surface);
	aws_shell_window_list_changed(surface->server);
}

/* **AND THE ANSWER TO A DOCK'S QUESTION, WHICH IS THE FIRST MENU THIS SERVER HAS EVER CARRIED.** The application
 * hands over a description of its own items and this file does not read a byte of it: the format is the AppKit's
 * at both ends, and a compositor that parsed a menu would have an opinion about what an item is — the same mistake
 * a special case for "the dock" would be. What the server does is the one thing only it can: hand the answer to
 * the client that asked, which is a fact it keeps under the WINDOW ID rather than a pointer (see
 * `struct aws_dock_menu`), so a window destroyed mid-question is a question left unanswered rather than a crash.
 *
 * **EVERY PENDING ASK NAMING THIS WINDOW HEARS IT.** Two right-clicks on two docks, or on the same tile twice, are
 * two questions with one answer, and each asker gets it. An answer nobody asked for is ignored rather than refused:
 * the ask it belonged to may have timed out, or the Dock may have gone, and neither is the application's fault. */
static void window_set_dock_menu(struct wl_client *client, struct wl_resource *resource, const char *menu)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);
	struct aws_dock_menu *ask, *tmp;

	(void)client;
	if (surface == NULL || surface->id == 0) {
		return;
	}
	wl_list_for_each_safe(ask, tmp, &surface->server->dock_menus, link) {
		if (ask->window != surface->id) {
			continue;
		}
		/* THE NODE STAYS — IT IS THE CLIENT'S OBJECT — but it has been answered, so a second answer to the same
		 * question cannot be mistaken for this one's. */
		ask->window = 0;
		agora_dock_menu_send_menu(ask->resource, surface->app_id != NULL ? surface->app_id : "",
					  menu != NULL ? menu : "");
	}
}

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
	/* **AND `destroy` IS NOT LAST ANY MORE, WHICH IS THE PROTOCOL'S RULE AND NOT A TASTE.** A request declared
	 * after one with `since` 2 would itself have to be at version 2 or above — so `set_reserved` (the only
	 * version 2 request) comes after the destructor, and this table follows the XML's order exactly. */
	window_set_reserved,
	window_set_activatable,
	window_take_focus,
	window_set_dock_menu,
};

static void agora_window_destroy_resource(struct wl_resource *resource)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	/* THE ROLE GOES WITH THE OBJECT, and the SURFACE STAYS: the contract says a surface may be given another
	 * role afterwards, which is what `destroy` means here rather than destroying the surface too. **AND A
	 * RESERVATION GOES WITH IT** — it was this object's — but it goes only after the surface has stopped
	 * naming this object, so the announcement that follows is not sent to the thing being destroyed. */
	if (surface != NULL) {
		surface->agora_window = NULL;
		aws_shell_release_reserve(surface);
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
	window = wl_resource_create(client, &agora_window_interface,
				    wl_resource_get_version(resource) > agora_window_interface.version
					    ? agora_window_interface.version
					    : wl_resource_get_version(resource),
				    id);
	if (window == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	/* **AND THE CAP IS APPLIED WHERE THE OBJECT IS MADE, NOT AFTER IT.** Wayland hands a child the version of the
	 * object that made it, which is how a shell bound at 1 keeps handing out version 1 windows — and the child
	 * may not claim a version its own interface has not got. Both numbers are therefore read once, here, so the
	 * proxy the client holds and the resource the server holds agree about which requests exist. */
	surface->agora_window = window;
	/* **AND THE NUMBER A SHELL COMPONENT WILL KNOW IT BY.** A Dock holds no proxy for somebody else's window, so
	 * this is the handle `activate_window` and `close_window` take; zero is never handed out, so zero can mean
	 * "no window" in the list and in a lookup. */
	surface->id = surface->server->next_window_id++;
	if (surface->server->next_window_id == 0) {
		surface->server->next_window_id = 1;	/* wrapped: zero is the sentinel and may not be an id */
	}
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

/* **THE WINDOW LIST: A SNAPSHOT, SENT WHENEVER WHAT IT WOULD SAY CHANGES.** Every door in this file that moves a
 * window, renames it, shows or hides it, or changes what application it belongs to calls this — and so does the
 * seat when the keyboard moves, and the surface's own mapping. It is the Dock's data source, and it is a snapshot
 * rather than a delta because a desktop has thirty windows and a person is what changes them: a client that
 * missed one is corrected by the next, with no sequence number to get wrong.
 *
 * **XDG-STYLE TOPLEVELS ARE NOT IN IT.** They have no server id and no `agora_window` to ask to close, so they
 * would be rows a shell could not act on — and a list that offers actions that cannot be taken is worse than a
 * shorter list. */
void aws_shell_window_list_changed(struct aws_server *server)
{
	struct aws_window_list *list, *tmp;
	struct aws_surface *surface;

	if (wl_list_empty(&server->window_lists)) {
		return;
	}
	wl_list_for_each_safe(list, tmp, &server->window_lists, link) {
		agora_window_list_send_begin(list->resource);
		wl_list_for_each(surface, &server->surfaces, link) {
			if (surface->agora_window == NULL || surface->id == 0) {
				continue;
			}
			agora_window_list_send_window(list->resource, surface->id,
						     surface->app_id != NULL ? surface->app_id : "",
						     surface->title != NULL ? surface->title : "",
						     surface->level, surface->visible ? 1 : 0,
						     server->keyboard_focus == surface ? 1 : 0);
		}
		agora_window_list_send_done(list->resource);
	}
}

/* **AND THE TWO THINGS A SHELL DOES TO SOMEBODY ELSE'S WINDOW, BY THE NUMBER THE LIST GAVE IT.** A Dock holds no
 * proxy for a window that is not its own, so the id is the whole handle. An id nothing answers is ignored: a Dock
 * clicking a tile as its window closes is ordinary, and a protocol error there would kill the Dock rather than
 * drawing one stale tile. */
static struct aws_surface *surface_for_id(struct aws_server *server, uint32_t id)
{
	struct aws_surface *surface;

	if (id == 0) {
		return NULL;
	}
	wl_list_for_each(surface, &server->surfaces, link) {
		if (surface->id == id && surface->agora_window != NULL) {
			return surface;
		}
	}
	return NULL;
}

static void shell_activate_window(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct aws_server *server = wl_resource_get_user_data(resource);
	struct aws_surface *surface = surface_for_id(server, id);

	(void)client;
	if (surface == NULL) {
		return;
	}
	/* **RAISE AND FOCUS TOGETHER, WHICH IS THE PAIR A CLICK ON THE WINDOW ITSELF WOULD HAVE MADE.** `raise` is
	 * only the stacking half and the focus is the server's to decide, so a Dock click is expressed here as the
	 * pair — which is what makes the application's own menu bar follow. */
	surface_bring_to_front(server, surface);
	aws_seat_set_keyboard_focus(server, surface);
	aws_server_schedule_render(server);
	aws_seat_pointer_recheck(server);
	aws_shell_window_list_changed(server);
}

static void shell_close_window(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct aws_server *server = wl_resource_get_user_data(resource);
	struct aws_surface *surface = surface_for_id(server, id);

	(void)client;
	if (surface == NULL) {
		return;
	}
	/* THE APPLICATION DECIDES, which is why this is an EVENT and not a destroy: `-windowShouldClose:` and
	 * everything behind it runs in the application, and a client that refuses stays open. */
	agora_window_send_close(surface->agora_window);
}

/* **THE OTHER HALF OF A CLICK THAT HAS NOTHING TO RAISE.** `activate_window` moves a window; this asks the
 * application behind one to show something. The window is the ADDRESS and not the subject — both sides know each
 * other by window ids and by nothing else — so what gets opened is the application's answer, not ours. */
static void shell_reopen_window(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct aws_server *server = wl_resource_get_user_data(resource);
	struct aws_surface *surface = surface_for_id(server, id);

	(void)client;
	if (surface == NULL || surface->agora_window == NULL) {
		return;
	}
	/* **AN EVENT NOBODY DECLARED MUST NOT BE SENT.** The window belongs to a client that may have bound the shell
	 * at an older version than the one asking — a Dock at 5 can see a window made by a client at 3 — and Wayland
	 * treats an unknown event as a protocol error on the receiving connection, which would kill that application
	 * rather than leave it un-reopened. */
	if (wl_resource_get_version(surface->agora_window) < 5) {
		return;
	}
	agora_window_send_reopen(surface->agora_window);
}

/* ===================================================================================================
 * THE DOCK MENU: ONE QUESTION, ONE ANSWER, AND ONE CHOICE SENT HOME.
 *
 * **THE SERVER'S WHOLE PART IN A MENU IS CARRYING IT.** The description is the AppKit's at both ends — the Dock
 * composes the panel, the application owns the items — so nothing here parses a byte of it. What the server does
 * is the part neither client can do for itself: it knows WHICH window an application owns and WHICH client asked
 * about it, and it keeps the answer's address so the round trip closes.
 *
 * **AND IT IS A ROUND TRIP ON PURPOSE.** A tile's menu is a picture of what the application can do at this moment
 * ("Open Recent" changes with the documents; a Play/Pause row changes with the player), so it is asked for when a
 * person right-clicks and not cached anywhere. The Mac's Dock asks its applications the same way, which is why
 * `-applicationDockMenu:` is a method rather than a stored property.
 * =================================================================================================== */

/* The asker's own object, and nothing else to clean up: the node lives exactly as long as the resource does. */
static void dock_menu_destroy_resource(struct wl_resource *resource)
{
	struct aws_dock_menu *ask = wl_resource_get_user_data(resource);

	if (ask != NULL) {
		wl_list_remove(&ask->link);
		free(ask);
	}
}

/* **THE ONE REQUEST THIS OBJECT HAS, AND IT MUST BE HANDLED RATHER THAN LEFT OUT.** A resource whose
 * implementation is NULL has no function pointer for the opcode a client's `destroy` carries, and libwayland
 * dereferences it — MEASURED, as a compositor that segfaulted the moment a dock put its answer object down. The
 * destructor is the whole vtable because it is the whole interface. */
static void dock_menu_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct agora_dock_menu_interface agora_dock_menu_implementation = {
	dock_menu_destroy,
};

static void shell_get_dock_menu(struct wl_client *client, struct wl_resource *resource, uint32_t id, uint32_t window)
{
	struct aws_server *server = wl_resource_get_user_data(resource);
	struct aws_surface *surface = surface_for_id(server, window);
	struct wl_resource *menu_resource;
	struct aws_dock_menu *ask;

	menu_resource = wl_resource_create(client, &agora_dock_menu_interface, 1, id);
	if (menu_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	ask = calloc(1, sizeof *ask);
	if (ask == NULL) {
		wl_resource_destroy(menu_resource);
		wl_client_post_no_memory(client);
		return;
	}
	ask->resource = menu_resource;
	ask->window = window;
	wl_list_insert(server->dock_menus.prev, &ask->link);
	wl_resource_set_implementation(menu_resource, &agora_dock_menu_implementation, ask, dock_menu_destroy_resource);

	/* **A QUESTION THE SERVER CANNOT DELIVER IS ANSWERED NOW, EMPTY.** The window may be gone, may be an
	 * `xdg_toplevel` with no `agora_window` at all, or may belong to a client bound below version 6 — and in every
	 * one of those cases "this application has no dock menu" is the honest answer, said immediately rather than
	 * left for a timeout to say later. A shell component is a client of a protocol that grows, and it must not
	 * hang because it asked an older one. */
	if (surface == NULL || surface->agora_window == NULL ||
	    wl_resource_get_version(surface->agora_window) < 6) {
		ask->window = 0;
		agora_dock_menu_send_menu(menu_resource, "", "");
		return;
	}
	agora_window_send_dock_menu_requested(surface->agora_window);
}

/* **AND THE OTHER DIRECTION: THE ITEM, BACK TO THE APPLICATION THAT MADE IT.** The Dock drew the panel and knows
 * only the id the application gave the item, so the id is what crosses; turning it back into an `NSMenuItem` and
 * sending its action is the application's, because only it knows what choosing one means. */
static void shell_dock_menu_action(struct wl_client *client, struct wl_resource *resource, uint32_t id, int32_t item)
{
	struct aws_server *server = wl_resource_get_user_data(resource);
	struct aws_surface *surface = surface_for_id(server, id);

	(void)client;
	if (surface == NULL || surface->agora_window == NULL) {
		return;
	}
	/* An event a resource's version does not have is a protocol error on that client's connection — see
	 * `shell_reopen_window`, which is the same rule one version down. */
	if (wl_resource_get_version(surface->agora_window) < 6) {
		return;
	}
	agora_window_send_dock_menu_action(surface->agora_window, item);
}

static void window_list_destroy_resource(struct wl_resource *resource)
{
	struct aws_window_list *list = wl_resource_get_user_data(resource);

	if (list != NULL) {
		wl_list_remove(&list->link);
		free(list);
	}
}

static void shell_get_window_list(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct aws_server *server = wl_resource_get_user_data(resource);
	struct aws_window_list *list;
	struct wl_resource *list_resource;

	list_resource = wl_resource_create(client, &agora_window_list_interface,
					   wl_resource_get_version(resource) > agora_window_list_interface.version
						   ? agora_window_list_interface.version
						   : wl_resource_get_version(resource),
					   id);
	if (list_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	list = calloc(1, sizeof *list);
	if (list == NULL) {
		wl_resource_destroy(list_resource);
		wl_client_post_no_memory(client);
		return;
	}
	list->resource = list_resource;
	wl_list_insert(server->window_lists.prev, &list->link);
	/* **EVENT-ONLY, LIKE THE OUTPUT**: the server never receives anything on this object, so `NULL` is the vtable
	 * and the user data is the list's own node. */
	wl_resource_set_implementation(list_resource, NULL, list, window_list_destroy_resource);
	/* AND THE FIRST SNAPSHOT IS SENT NOW, because a client that has just asked is a client about to draw. */
	aws_shell_window_list_changed(server);
}

static const struct agora_shell_interface agora_shell_implementation = {
	shell_get_window,
	shell_get_output,
	shell_get_window_list,
	shell_activate_window,
	shell_close_window,
	shell_reopen_window,
	shell_get_dock_menu,
	shell_dock_menu_action,
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
	wl_global_create(server->display, &agora_shell_interface, 6, server, bind_agora_shell);
}
