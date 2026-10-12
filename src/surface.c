/*
 * surface.c — what a client draws through: wl_compositor, wl_surface and xdg-shell.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
 *
 * THE TWO STATES A SURFACE HAS, AND WHY THEY ARE TWO. Wayland is double-buffered at the SURFACE, not at the
 * buffer: a client ATTACHES a buffer and later COMMITS it, and everything between those two calls is pending.
 * So `pending` is what a client is proposing and `current` is what the last commit made true, and the
 * compositor only ever draws `current`. THAT IS ALSO WHY `wl_surface.frame` IS THE ONLY WAY A CLIENT KNOWS ITS
 * FRAME WAS TAKEN: the compositor sends the callback when it has composited, and an AppKit window paints on
 * that callback and nowhere else.
 *
 * THE CONFIGURE DANCE, which is the part of xdg-shell that is easy to get wrong. A toplevel is not shown
 * because a buffer was committed; it is shown because the client ACKNOWLEDGED a configure and then committed.
 * The sequence this implements is the short one:
 *
 *   client: get_xdg_surface, get_toplevel, commit (with NO buffer attached)
 *   server: xdg_toplevel.configure + xdg_surface.configure(serial)
 *   client: ack_configure(serial), attach, commit
 *   server: composite, and send the frame callbacks
 *
 * The first commit is the client saying "I exist and I am ready to be told"; a buffer committed before the ack
 * is a protocol error, so `configured` is what keeps this honest rather than optimistic.
 *
 * THIS VERSION HAS ONE ROLE AND NO POPUPS: `get_popup` is answered with a protocol error rather than ignored,
 * because a client told nothing will wait, and a client told "no" will use a toplevel.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-protocol.h>

#include "xdg-shell-server-protocol.h"

/* ---------------------------------------------------------------------------------------------------
 * buffers
 * --------------------------------------------------------------------------------------------------- */

/* A wl_buffer IS ONLY USABLE HERE IF IT IS SHARED MEMORY. Anything else — a dmabuf, a client-side buffer —
 * is refused rather than guessed at: compositing a buffer whose layout this does not understand would be a
 * picture that is subtly wrong, which is worse than a client that is told no. */
static int aws_buffer_is_supported(struct wl_resource *resource)
{
	struct wl_shm_buffer *shm = wl_shm_buffer_get(resource);

	if (shm == NULL) {
		wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FORMAT,
				       "this compositor composites shared memory only");
		return 0;
	}
	switch (wl_shm_buffer_get_format(shm)) {
	case WL_SHM_FORMAT_XRGB8888:
	case WL_SHM_FORMAT_ARGB8888:
		return 1;
	default:
		wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FORMAT, "unsupported pixel format");
		return 0;
	}
}

/* ---------------------------------------------------------------------------------------------------
 * the frame callbacks
 * --------------------------------------------------------------------------------------------------- */

/* SENT WHEN A FRAME HAS BEEN COMPOSITED, which is the whole contract of `wl_surface.frame`: the client may
 * draw again. Destroying the resource afterwards is what makes the callback a ONE-SHOT — a client that wants
 * another asks again, which is how an animation paces itself. */
/* TAKEN OUT OF THE LIST WHEN THE CLIENT DESTROYS THE CALLBACK, which a client is allowed to do. */
static void frame_destroyed(struct wl_listener *listener, void *data)
{
	struct aws_frame *frame = wl_container_of(listener, frame, destroy_listener);

	(void)data;
	wl_list_remove(&frame->link);
	free(frame);
}

static void surface_send_frame_callbacks(struct aws_surface *surface, uint32_t time)
{
	struct aws_frame *frame, *tmp;

	wl_list_for_each_safe(frame, tmp, &surface->frame_callbacks, link) {
		wl_callback_send_done(frame->resource, time);
		wl_resource_destroy(frame->resource);
	}
}

/* ---------------------------------------------------------------------------------------------------
 * the surface
 * --------------------------------------------------------------------------------------------------- */

/* THE CLIENT DESTROYED THE BUFFER IT COMMITTED: there is nothing to composite any more, and the surface goes
 * with it — a window whose buffer is gone is a window that is not there. */
static void buffer_destroyed(struct wl_listener *listener, void *data)
{
	struct aws_surface *surface = wl_container_of(listener, surface, buffer_listener);

	(void)data;
	surface->current_buffer = NULL;
	surface->has_buffer_listener = 0;
	surface->mapped = 0;
	aws_server_schedule_render(surface->server);
}

static struct aws_surface *aws_surface_from_resource(struct wl_resource *resource)
{
	return resource != NULL ? wl_resource_get_user_data(resource) : NULL;
}

static void surface_destroy_resource(struct wl_resource *resource)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);
	struct aws_frame *frame, *tmp;

	if (surface == NULL) {
		return;
	}
	wl_list_for_each_safe(frame, tmp, &surface->frame_callbacks, link) {
		wl_resource_destroy(frame->resource);
	}
	if (surface->has_buffer_listener) {
		wl_list_remove(&surface->buffer_listener.link);
	}
	if (surface->xdg_toplevel != NULL) {
		/* The role goes with the surface: a window without a window is nothing, and a client that destroys
		 * the surface without destroying the toplevel is telling us the same thing. */
		wl_resource_set_user_data(surface->xdg_toplevel, NULL);
	}
	if (surface->xdg_surface != NULL) {
		wl_resource_set_user_data(surface->xdg_surface, NULL);
	}
	free(surface->title);
	free(surface->app_id);
	wl_list_remove(&surface->link);
	free(surface);
}

static void surface_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void surface_attach(struct wl_client *client, struct wl_resource *resource,
			   struct wl_resource *buffer_resource, int32_t x, int32_t y)
{
	struct aws_surface *surface = aws_surface_from_resource(resource);

	(void)client;
	(void)x;
	(void)y;
	if (surface == NULL) {
		return;
	}
	/* ATTACHING NOTHING IS HOW A SURFACE IS HIDDEN, and it is a real state rather than an error: the commit
	 * that follows it takes the surface off the screen. */
	if (buffer_resource != NULL && !aws_buffer_is_supported(buffer_resource)) {
		surface->pending_buffer = NULL;	/* the client has already been told */
		return;
	}
	surface->pending_buffer = buffer_resource;
}

static void surface_damage(struct wl_client *client, struct wl_resource *resource,
			   int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
	/* DAMAGE IS TRACKED NOWHERE, ON PURPOSE: this compositor recomposites the whole output on every frame, so
	 * a damage rectangle could only be used to AVOID work that is already not being done. It is where the
	 * optimisation goes when there is a reason for one. */
}

static void surface_damage_buffer(struct wl_client *client, struct wl_resource *resource,
				  int32_t x, int32_t y, int32_t width, int32_t height)
{
	surface_damage(client, resource, x, y, width, height);
}

static void surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t callback)
{
	struct aws_surface *surface = aws_surface_from_resource(resource);
	struct aws_frame *frame;

	if (surface == NULL) {
		return;
	}
	frame = calloc(1, sizeof *frame);
	if (frame == NULL) {
		return;
	}
	frame->resource = wl_resource_create(client, &wl_callback_interface, 1, callback);
	wl_resource_set_implementation(frame->resource, NULL, NULL, NULL);
	frame->destroy_listener.notify = frame_destroyed;
	wl_resource_add_destroy_listener(frame->resource, &frame->destroy_listener);
	wl_list_insert(surface->frame_callbacks.prev, &frame->link);
}

static void surface_set_opaque_region(struct wl_client *client, struct wl_resource *resource,
				      struct wl_resource *region)
{
	(void)client; (void)resource; (void)region;
}

static void surface_set_input_region(struct wl_client *client, struct wl_resource *resource,
				     struct wl_resource *region)
{
	(void)client; (void)resource; (void)region;
}

/* THE CONFIGURE DANCE'S SECOND HALF, and the place a client is allowed to attach a buffer. */
static void surface_commit(struct wl_client *client, struct wl_resource *resource)
{
	struct aws_surface *surface = aws_surface_from_resource(resource);
	struct aws_server *server;

	(void)client;
	if (surface == NULL) {
		return;
	}
	server = surface->server;

	/* A TOPLEVEL'S FIRST COMMIT IS A REQUEST TO BE CONFIGURED, not a picture: it arrives with no buffer
	 * attached, and answering it is what lets the client draw at all. */
	/* **ONCE PER SURFACE, NOT ONCE PER COMMIT.** A client commonly commits an empty surface more than once
	 * before it draws — `AGWindowCreate` commits to ask for a role and `AGWindowShow` commits again — and a
	 * compositor that answered every one of those with a configure would be talking to a client that has to
	 * acknowledge all of them before it can draw. MEASURED: three configures went out for one window. */
	if (surface->xdg_toplevel != NULL && !surface->configure_sent) {
		struct wl_array states;
		uint32_t serial = wl_display_next_serial(server->display);

		wl_array_init(&states);
		/* 0x0 MEANS "YOU CHOOSE": placement and sizing are POLICY, and this version has none — so the client
		 * keeps the size it asked for and the compositor takes the size it is given. */
		surface->configure_sent = 1;
		xdg_toplevel_send_configure(surface->xdg_toplevel, 0, 0, &states);
		xdg_surface_send_configure(surface->xdg_surface, serial);
		wl_array_release(&states);
		wl_display_flush_clients(server->display);
		return;
	}

	/* THE COMMIT ITSELF: pending becomes current, and a surface that has something to show is placed. */
	if (surface->has_buffer_listener) {
		wl_list_remove(&surface->buffer_listener.link);
		surface->has_buffer_listener = 0;
	}
	surface->current_buffer = surface->pending_buffer;
	surface->pending_buffer = NULL;
	if (surface->current_buffer != NULL) {
		surface->buffer_listener.notify = buffer_destroyed;
		wl_resource_add_destroy_listener(surface->current_buffer, &surface->buffer_listener);
		surface->has_buffer_listener = 1;
		if (!surface->mapped) {
			/* **THERE IS NO CASCADE HERE, AND THAT IS THE POINT.** A cascade is a PLACEMENT POLICY, and on
			 * the Mac the application's frame is honoured — the only thing that adjusts it is the
			 * application's own `constrainFrameRect:toScreen:`. So there is no offset computed, no counting
			 * of windows and no "put it somewhere plausible": a client that says nothing gets the origin,
			 * which is what an application that never set a frame would get anyway. */
			surface->mapped = 1;
			/* **A NEW WINDOW TAKES THE KEYBOARD IF NOTHING HAS IT.** A window that is mapped but never
			 * clicked would otherwise receive no key at all — a person could see it and not type into it.
			 * This is the initial-focus half of the policy; `aws_seat_set_keyboard_focus` carries the rest. */
			if (server->keyboard_focus == NULL) {
				aws_seat_set_keyboard_focus(server, surface);
			}
			/* MAPPED SURFACES ARE COMPOSITED IN MAPPING ORDER — the list is the stacking, and this version
			 * has no other: raising and lowering arrive with the window-management policy. */
			wl_list_remove(&surface->link);
			wl_list_insert(server->surfaces.prev, &surface->link);
		}
		aws_server_schedule_render(server);
	} else {
		surface->mapped = 0;
		aws_server_schedule_render(server);
	}
}

static void surface_set_buffer_transform(struct wl_client *client, struct wl_resource *resource, int32_t transform)
{
	(void)client; (void)resource; (void)transform;
}

static void surface_set_buffer_scale(struct wl_client *client, struct wl_resource *resource, int32_t scale)
{
	(void)client; (void)resource; (void)scale;
}

static const struct wl_surface_interface surface_implementation = {
	surface_destroy,
	surface_attach,
	surface_damage,
	surface_frame,
	surface_set_opaque_region,
	surface_set_input_region,
	surface_commit,
	surface_set_buffer_transform,
	surface_set_buffer_scale,
	surface_damage_buffer,
};

static void compositor_create_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct aws_server *server = wl_resource_get_user_data(resource);
	struct aws_surface *surface = calloc(1, sizeof *surface);

	if (surface == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	surface->server = server;
	surface->resource = wl_resource_create(client, &wl_surface_interface,
					       wl_resource_get_version(resource), id);
	if (surface->resource == NULL) {
		free(surface);
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_init(&surface->frame_callbacks);
	wl_list_insert(server->surfaces.prev, &surface->link);
	wl_resource_set_implementation(surface->resource, &surface_implementation, surface,
				       surface_destroy_resource);
}

static void region_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void region_add(struct wl_client *client, struct wl_resource *resource,
		       int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void region_subtract(struct wl_client *client, struct wl_resource *resource,
			    int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static const struct wl_region_interface region_implementation = {
	region_destroy,
	region_add,
	region_subtract,
};

static void compositor_create_region(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct wl_resource *region = wl_resource_create(client, &wl_region_interface,
							wl_resource_get_version(resource), id);

	if (region == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	/* A REGION IS ACCEPTED AND EMPTY. Nothing here does anything with one yet — an opaque region is an
	 * optimisation, and an input region belongs to hit testing, which belongs with input. A region that lied
	 * about being non-empty would be worse than one that is empty, and the client cannot tell the difference
	 * for the parts this does do. */
	wl_resource_set_implementation(region, &region_implementation, NULL, NULL);
}

static const struct wl_compositor_interface compositor_implementation = {
	compositor_create_surface,
	compositor_create_region,
};

static void bind_compositor(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client, &wl_compositor_interface,
							  version < 4 ? version : 4, id);

	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &compositor_implementation, data, NULL);
}

void aws_compositor_init(struct aws_server *server)
{
	/* wl_shm IS NOT IMPLEMENTED HERE: libwayland owns it (`wl_display_init_shm`), including the pool
	 * bookkeeping and the shared-memory mapping, so a compositor gets the buffer API and not the plumbing. */
	wl_display_init_shm(server->display);
	wl_global_create(server->display, &wl_compositor_interface, 4, server, bind_compositor);
}

/* ---------------------------------------------------------------------------------------------------
 * xdg-shell: the one role this version knows
 * --------------------------------------------------------------------------------------------------- */

static struct aws_surface *aws_surface_for_xdg(struct wl_resource *xdg_surface)
{
	return wl_resource_get_user_data(xdg_surface);
}

static void xdg_toplevel_destroy(struct wl_client *client, struct wl_resource *resource)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (surface != NULL) {
		surface->xdg_toplevel = NULL;
	}
	wl_resource_set_user_data(resource, NULL);
	wl_resource_destroy(resource);
}

static void xdg_toplevel_set_parent(struct wl_client *client, struct wl_resource *resource,
				    struct wl_resource *parent)
{
	(void)client; (void)resource; (void)parent;
}

static void xdg_toplevel_set_title(struct wl_client *client, struct wl_resource *resource, const char *title)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (surface == NULL) {
		return;
	}
	free(surface->title);
	surface->title = strdup(title != NULL ? title : "");
}

static void xdg_toplevel_set_app_id(struct wl_client *client, struct wl_resource *resource, const char *app_id)
{
	struct aws_surface *surface = wl_resource_get_user_data(resource);

	(void)client;
	if (surface == NULL) {
		return;
	}
	free(surface->app_id);
	surface->app_id = strdup(app_id != NULL ? app_id : "");
}

static void xdg_toplevel_show_window_menu(struct wl_client *client, struct wl_resource *resource,
					  struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y)
{
	(void)client; (void)resource; (void)seat; (void)serial; (void)x; (void)y;
}

static void xdg_toplevel_move(struct wl_client *client, struct wl_resource *resource,
			      struct wl_resource *seat, uint32_t serial)
{
	(void)client; (void)resource; (void)seat; (void)serial;
}

static void xdg_toplevel_resize(struct wl_client *client, struct wl_resource *resource,
				struct wl_resource *seat, uint32_t serial, uint32_t edges)
{
	(void)client; (void)resource; (void)seat; (void)serial; (void)edges;
}

static void xdg_toplevel_set_max_size(struct wl_client *client, struct wl_resource *resource,
				      int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)width; (void)height;
}

static void xdg_toplevel_set_min_size(struct wl_client *client, struct wl_resource *resource,
				      int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)width; (void)height;
}

static void xdg_toplevel_set_maximized(struct wl_client *client, struct wl_resource *resource)
{
	(void)client; (void)resource;
}

static void xdg_toplevel_unset_maximized(struct wl_client *client, struct wl_resource *resource)
{
	(void)client; (void)resource;
}

static void xdg_toplevel_set_fullscreen(struct wl_client *client, struct wl_resource *resource,
					struct wl_resource *output)
{
	(void)client; (void)resource; (void)output;
}

static void xdg_toplevel_unset_fullscreen(struct wl_client *client, struct wl_resource *resource)
{
	(void)client; (void)resource;
}

static void xdg_toplevel_set_minimized(struct wl_client *client, struct wl_resource *resource)
{
	(void)client; (void)resource;
}

static const struct xdg_toplevel_interface xdg_toplevel_implementation = {
	xdg_toplevel_destroy,
	xdg_toplevel_set_parent,
	xdg_toplevel_set_title,
	xdg_toplevel_set_app_id,
	xdg_toplevel_show_window_menu,
	xdg_toplevel_move,
	xdg_toplevel_resize,
	xdg_toplevel_set_max_size,
	xdg_toplevel_set_min_size,
	xdg_toplevel_set_maximized,
	xdg_toplevel_unset_maximized,
	xdg_toplevel_set_fullscreen,
	xdg_toplevel_unset_fullscreen,
	xdg_toplevel_set_minimized,
};

static void xdg_surface_destroy(struct wl_client *client, struct wl_resource *resource)
{
	struct aws_surface *surface = aws_surface_for_xdg(resource);

	(void)client;
	if (surface != NULL) {
		surface->xdg_surface = NULL;
	}
	wl_resource_set_user_data(resource, NULL);
	wl_resource_destroy(resource);
}

static void xdg_surface_get_toplevel(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct aws_surface *surface = aws_surface_for_xdg(resource);
	struct wl_resource *toplevel;

	if (surface == NULL) {
		return;
	}
	/* ONE ROLE, AND A SURFACE THAT ALREADY HAS ONE CANNOT HAVE ANOTHER: the protocol says so, and enforcing it
	 * is what stops a surface being a window and a cursor at once. */
	if (surface->has_buffer_listener) {
		wl_list_remove(&surface->buffer_listener.link);
	}
	if (surface->xdg_toplevel != NULL) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
				       "this surface already has a role");
		return;
	}
	toplevel = wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
	if (toplevel == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	surface->xdg_toplevel = toplevel;
	wl_resource_set_implementation(toplevel, &xdg_toplevel_implementation, surface, NULL);
}

static void xdg_surface_get_popup(struct wl_client *client, struct wl_resource *resource, uint32_t id,
				  struct wl_resource *parent, struct wl_resource *positioner)
{
	(void)id; (void)parent; (void)positioner;
	/* REFUSED BY NAME RATHER THAN IGNORED. A popup this compositor cannot place would be a menu that never
	 * appears, and a client that is told so will use a toplevel instead — which is a window that appears. */
	wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
			       "popups are not implemented in this version of the window server");
	(void)client;
}

static void xdg_surface_set_window_geometry(struct wl_client *client, struct wl_resource *resource,
					    int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void xdg_surface_ack_configure(struct wl_client *client, struct wl_resource *resource, uint32_t serial)
{
	struct aws_surface *surface = aws_surface_for_xdg(resource);

	(void)client;
	(void)serial;
	if (surface != NULL) {
		/* THE ACK IS WHAT UNLOCKS THE BUFFER. After it the client may attach and commit, and before it a
		 * commit with a buffer is a protocol error — which is the rule this flag exists to respect. */
		surface->configured = 1;
	}
}

static const struct xdg_surface_interface xdg_surface_implementation = {
	xdg_surface_destroy,
	xdg_surface_get_toplevel,
	xdg_surface_get_popup,
	xdg_surface_set_window_geometry,
	xdg_surface_ack_configure,
};

static void xdg_wm_base_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void xdg_wm_base_create_positioner(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	(void)id;
	wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
			       "positioners exist for popups, and popups are not implemented");
	(void)client;
}

static void xdg_wm_base_get_xdg_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id,
					struct wl_resource *surface_resource)
{
	struct aws_surface *surface = aws_surface_from_resource(surface_resource);
	struct wl_resource *xdg_surface;

	if (surface == NULL) {
		return;
	}
	/* ONE ROLE, AND THE TWO ROLE PROTOCOLS DISAGREE ABOUT WHO DECIDES THE SIZE — so a surface that is already
	 * an `agora_window` cannot also be an `xdg_toplevel`, and saying so is kinder than a surface quietly obeying
	 * two contradictory models. */
	if (surface->agora_window != NULL) {
		wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
				       "this surface already has an agora_window role");
		return;
	}
	if (surface->xdg_surface != NULL) {
		wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
				       "this surface already has an xdg_surface");
		return;
	}
	xdg_surface = wl_resource_create(client, &xdg_surface_interface, wl_resource_get_version(resource), id);
	if (xdg_surface == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	surface->xdg_surface = xdg_surface;
	wl_resource_set_implementation(xdg_surface, &xdg_surface_implementation, surface, NULL);
}

static void xdg_wm_base_pong(struct wl_client *client, struct wl_resource *resource, uint32_t serial)
{
	(void)client; (void)resource; (void)serial;
}

static const struct xdg_wm_base_interface xdg_wm_base_implementation = {
	xdg_wm_base_destroy,
	xdg_wm_base_create_positioner,
	xdg_wm_base_get_xdg_surface,
	xdg_wm_base_pong,
};

static void bind_xdg_wm_base(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client, &xdg_wm_base_interface,
							  version < 2 ? version : 2, id);

	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &xdg_wm_base_implementation, data, NULL);
}

void aws_xdg_shell_init(struct aws_server *server)
{
	wl_global_create(server->display, &xdg_wm_base_interface, 2, server, bind_xdg_wm_base);
}

/* ---------------------------------------------------------------------------------------------------
 * the frame callbacks the compositor sends
 * --------------------------------------------------------------------------------------------------- */

void aws_surface_frame_done(struct aws_surface *surface, uint32_t time)
{
	surface_send_frame_callbacks(surface, time);
}
