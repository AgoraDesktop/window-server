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
#define AWS_MENU_BAR_HEIGHT 24
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
	aws_server_schedule_render(surface->server);
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

static const struct agora_window_interface agora_window_implementation = {
	window_set_frame,
	window_set_level,
	window_raise,
	window_lower,
	window_set_title,
	window_set_app_id,
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
