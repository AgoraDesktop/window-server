/*
 * server.c — the server's own lifecycle, and the one piece of state the event loop reads.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
 */

#include "server.h"

#include <stdlib.h>
#include <string.h>

struct aws_server *aws_server_create(const char *backend_name, const char *input_name, const char *renderer_name,
				     int width, int height)
{
	struct aws_server *server = calloc(1, sizeof *server);

	if (server == NULL) {
		return NULL;
	}
	server->display = wl_display_create();
	if (server->display == NULL) {
		free(server);
		return NULL;
	}
	server->loop = wl_display_get_event_loop(server->display);
	server->requested_width = width > 0 ? width : 1280;
	server->requested_height = height > 0 ? height : 720;
	wl_list_init(&server->surfaces);
	/* **AND THE OUTPUTS THAT HAVE TO HEAR ABOUT THE WORK AREA.** A client reserves a strip, the screen changes,
	 * and every bound `agora_output` is told — which needs the list to exist before any client can bind. */
	wl_list_init(&server->agora_outputs);
	/* AND THE CLIENTS THAT ASKED WHAT WINDOWS EXIST — a Dock, usually one. */
	wl_list_init(&server->window_lists);
	/* **AND THE MENUS STILL BEING WAITED FOR.** Empty until a Dock right-clicks a tile, and empty again as soon as
	 * every application has answered — nothing here outlives the answer it was made for. */
	wl_list_init(&server->dock_menus);
	/* **ZERO IS NEVER A WINDOW'S NUMBER**, so zero is free to mean "no window" in the list and in a lookup. */
	server->next_window_id = 1;

	/* **THE OS IS ASKED ONCE, BEFORE ANY FRAME EXISTS.** `os_identity_read` never fails the server: a machine with
	 * no recognisable `os-release` gets an empty ID and the resolver's hamburger, which is the same picture this
	 * compositor drew before detection existed. */
	aws_os_identity_read(&server->os_identity);
	server->system_menu_icon = aws_system_menu_icon_resolve(&server->os_identity);

	/* THE GLOBALS, IN THE ORDER A CLIENT BINDS THEM — which the protocol does not require and a reader does:
	 * the compositor a surface is made from, the shell that gives it a role, the output it is shown on, and the
	 * seat its input comes from. */
	aws_compositor_init(server);
	aws_xdg_shell_init(server);
	aws_output_init(server);
	aws_seat_init(server);
	aws_shell_init(server);
	/* THE BACKEND LAST, because an X11 one adds its connection's descriptor to the event loop that has to
	 * exist first. */
	if (aws_backend_init(server, backend_name, server->requested_width, server->requested_height) != 0) {
		wl_display_destroy(server->display);
		free(server);
		return NULL;
	}
	/* **THE RENDERER AFTER THE BACKEND, because it may need the native window a backend just made.** A backend
	 * that cannot offer one is not a failure: `auto` hands the server the CPU renderer, which every backend can run.
	 * A renderer asked for by name and impossible IS a failure, and `aws_renderer_init` says so. */
	if (aws_renderer_init(server, renderer_name) != 0) {
		aws_backend_shutdown(server);
		wl_display_destroy(server->display);
		free(server);
		return NULL;
	}
	/* **AND THE GPU-BUFFER PROTOCOL, WHICH EXISTS ONLY IF THE RENDERER CAN IMPORT.** It is created here and not
	 * with the globals above because its format list IS the renderer's answer, and a client cannot bind anything
	 * until `wl_display_add_socket_auto` runs in `main` — which is after this returns. */
	aws_dmabuf_init(server);
	/* INPUT LAST, because a source may need to know where the output ended up (the nested window's size) and
	 * because it has to be told whether the backend is feeding the seat by itself. */
	if (aws_input_init(server, input_name, server->backend.name) != 0) {
		aws_backend_shutdown(server);
		wl_display_destroy(server->display);
		free(server);
		return NULL;
	}
	return server;
}

void aws_server_destroy(struct aws_server *server)
{
	if (server == NULL) {
		return;
	}
	/* CLIENTS GO FIRST, because destroying the display with clients attached is a protocol error and not a
	 * cleanup — libwayland says so by refusing. */
	wl_display_destroy_clients(server->display);
	wl_display_destroy(server->display);
	/* **THE RENDERER GIVES ITS CONTEXT BACK BEFORE THE BACKEND GIVES ITS WINDOW BACK**, because an EGL surface
	 * names an X window and a swapped frame is not something to do after the window is gone. */
	aws_renderer_shutdown(server);
	aws_backend_shutdown(server);
	aws_input_shutdown(server);
	aws_seat_finish(server);
	free(server->dmabuf_formats);
	free(server);
}

void aws_server_schedule_render(struct aws_server *server)
{
	server->render_pending = 1;
}

int aws_server_set_system_menu_icon(struct aws_server *server, const char *name)
{
	struct aws_system_icon *icon;

	if (server == NULL || name == NULL || name[0] == '\0') {
		return -1;
	}
	if (strcmp(name, "auto") == 0) {
		server->system_menu_icon = aws_system_menu_icon_resolve(&server->os_identity);
		server->render_pending = 1;
		return 0;
	}
	icon = aws_system_icon_by_name(name);
	if (icon == NULL) {
		return -1;
	}
	server->system_menu_icon = icon;
	server->render_pending = 1;
	return 0;
}
