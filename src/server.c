/*
 * server.c — the server's own lifecycle, and the one piece of state the event loop reads.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
 */

#include "server.h"

#include <stdlib.h>
#include <string.h>

struct aws_server *aws_server_create(const char *backend_name, int width, int height)
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

	/* THE GLOBALS, IN THE ORDER A CLIENT BINDS THEM — which the protocol does not require and a reader does:
	 * the compositor a surface is made from, the shell that gives it a role, the output it is shown on, and the
	 * seat its input comes from. */
	aws_compositor_init(server);
	aws_xdg_shell_init(server);
	aws_output_init(server);
	aws_seat_init(server);
	/* THE BACKEND LAST, because an X11 one adds its connection's descriptor to the event loop that has to
	 * exist first. */
	if (aws_backend_init(server, backend_name, server->requested_width, server->requested_height) != 0) {
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
	aws_backend_shutdown(server);
	aws_seat_finish(server);
	free(server);
}

void aws_server_schedule_render(struct aws_server *server)
{
	server->render_pending = 1;
}
