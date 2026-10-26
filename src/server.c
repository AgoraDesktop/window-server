/*
 * server.c — the server's own lifecycle, and the one piece of state the event loop reads.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct aws_server *aws_server_create(const char *backend_name, const char *input_name, const char *renderer_name,
				     int width, int height, double scale, double ui_scale,
				     const char *cursor_theme, double cursor_size)
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
	wl_list_init(&server->shell_bindings);
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
	if (aws_backend_init(server, backend_name, server->requested_width, server->requested_height, scale,
			     ui_scale) != 0) {
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
	/* **AND NOW THE BACKEND KNOWS WHAT IS DRAWING.** A GPU backend waits until here to allocate anything that the
	 * renderer might make unnecessary: the DRM one would otherwise spend two framebuffers of dumb memory before a
	 * GBM swapchain takes the screen, which at 8K is 265 MB nobody needed. */
	if (server->backend.start_present != NULL && server->backend.start_present(server) != 0) {
		aws_renderer_shutdown(server);
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
	/* **AND THE POINTER'S ARTWORK LAST, BECAUSE BOTH SIDES OF THE DISPLAY HAVE TO EXIST TO TAKE IT.** The theme is the
	 * source of the pointer's pixels for the composited path (DRM) and, through `backend.apply_cursor`, for the path
	 * where a host draws it (nested X11) — so the backend has to have its window before the theme is handed over, and
	 * the renderer has to exist before a change can be scheduled. A theme that is not there is NOT a failed server:
	 * `aws_server_set_cursor_theme` says so and `cursor.c` draws Agora's own arrow, which is what every session
	 * without an installed theme uses. */
	server->cursor_size = (cursor_size > 0.0) ? cursor_size : AWS_CURSOR_DEFAULT_SIZE;
	aws_server_set_cursor_theme(server, cursor_theme);
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
	/* **THE RENDERER GIVES ITS CONTEXT BACK BEFORE THE BACKEND GIVES ITS WINDOW BACK**, because an EGL surface
	 * names an X window and a swapped frame is not something to do after the window is gone. */
	aws_renderer_shutdown(server);
	/* **AND THE BACKEND AND INPUT LET GO OF THE EVENT LOOP WHILE IT STILL EXISTS.** Their shutdown functions call
	 * `wl_event_source_remove`, and Wayland requires a source to be removed while the loop that owns it is alive —
	 * `wl_display_destroy` destroys the loop with the display. This function used to destroy the display FIRST and
	 * then remove sources from the loop it had just freed, which is an invalid list operation on every ordinary
	 * exit, measured or not. */
	aws_backend_shutdown(server);
	aws_input_shutdown(server);
	aws_seat_finish(server);
	/* **THE DISPLAY GOES LAST**, once nothing holds a source on its loop and no client resource names platform
	 * state that has been released. */
	wl_display_destroy(server->display);
	aws_cursor_finish(server);
	aws_cursor_theme_close(server->cursor_theme);
	free(server->dmabuf_formats);
	free(server);
}

void aws_server_schedule_render(struct aws_server *server)
{
	/* **EVERY CHANGE THAT IS NOT A CLIENT'S OWN PIXELS LANDS HERE, SO IT IS SAFE BY DEFAULT.** A geometry change, a menu
	 * bar title, the pointer on an output that draws its own cursor, a settings change — none of them knows what was
	 * under the pixels it moved, so the honest answer is the whole output. `aws_server_schedule_render_partial` is the
	 * other form, and only `surface_commit` — which HAS the client's rectangles — is allowed to ask for it.
	 *
	 * THE REASON IS NOT OVERWRITTEN, so a trigger that names itself (`aws_damage_full(server, "a window moved")`) keeps
	 * its name when the frame is asked for afterwards; a caller that says nothing gets the diagnostic's default. */
	aws_damage_full(server, NULL);
	server->render_pending = 1;
}

void aws_server_schedule_render_partial(struct aws_server *server)
{
	server->render_pending = 1;
}

/* **A CHANGE MADE DURING STARTUP DOES NOT NEED A FRAME, AND ASKING FOR ONE USED TO BREAK THE STARTUP DEADLINE.**
 * `--idle-timeout` exists to end a server that never composites — no client, a client that never mapped — and the
 * deadline is disarmed by the FIRST frame, so a setting applied between `aws_server_create` and the loop would
 * composite one frame of nothing, disarm the deadline, and leave exactly the hang the deadline is for. MEASURED, as
 * `window-server-dpi-override`, `-ui-scale` and `-idle-timeout` timing out at once after the theme started scheduling a
 * render at startup. A change made while the loop IS running still asks for a frame: there a frame is the only way the
 * change reaches the glass. */
static void server_schedule_if_running(struct aws_server *server)
{
	if (server->running) {
		aws_damage_full(server, "a setting changed");
		aws_server_schedule_render(server);
	}
}

/* **THE POINTER'S THEME IS A SETTING, NOT A BUILD FACT.** A session on a machine that installed the vendored theme gets
 * the theme; a session pointed at another directory with `AGORA_CURSOR_THEME` or `--cursor-theme` gets that one; a
 * session with neither, and no installed theme, gets the arrow `cursor.c` draws. Everything that could disagree about
 * the change is handled in one place: the cached image is invalidated by bumping the generation, the backend is told
 * to re-apply it where a host draws the pointer, and a frame is scheduled where the compositor does — so a change is
 * on the glass without waiting for a client to repaint. */
int aws_server_set_cursor_theme(struct aws_server *server, const char *directory)
{
	const char *path = (directory != NULL && directory[0] != '\0') ? directory : aws_cursor_theme_default_directory();
	struct aws_cursor_theme *theme;

	if (server == NULL) {
		return -1;
	}
	aws_cursor_theme_close(server->cursor_theme);
	/* THE FILE'S OWN ARROW IS READ AFTER THE POINTER IS FIRST ASKED FOR, not here: opening a theme is a directory check
	 * and nothing more, so a theme with no `left_ptr` still falls back per draw rather than failing a whole session. */
	theme = aws_cursor_theme_open(path);
	server->cursor_theme = theme;
	server->cursor_generation++;
	if (server->backend.apply_cursor != NULL) {
		server->backend.apply_cursor(server);
	}
	server_schedule_if_running(server);
	if (theme == NULL) {
		fprintf(stderr, "window-server: no cursor theme at %s — the pointer is Agora's drawn arrow\n", path);
		return -1;
	}
	/* **AND THE THEME HAS TO HAVE THE SHAPE THE COMPOSITOR ASKS FOR.** Opening a theme is a directory check; whether it
	 * contains an arrow is a different question, and a startup line that promised `left_ptr` from a theme without one
	 * would be a claim nothing keeps. Asking here also means a broken theme is named at startup rather than at the first
	 * pointer motion — and it caches the image the first frame will composite. */
	if (aws_cursor_theme_pointer(server) == NULL) {
		fprintf(stderr, "window-server: the cursor theme at %s has no %s for a %g-point pointer — the pointer is "
				"Agora's drawn arrow\n", aws_cursor_theme_directory(theme), AWS_CURSOR_ARROW_NAME,
			server->cursor_size);
		return -1;
	}
	fprintf(stderr, "window-server: the pointer is %s from the cursor theme at %s (%g points)\n",
		AWS_CURSOR_ARROW_NAME, aws_cursor_theme_directory(theme), server->cursor_size);
	return 0;
}

void aws_server_set_cursor_size(struct aws_server *server, double points)
{
	if (server == NULL || !(points > 0.0) || points == server->cursor_size) {
		return;
	}
	server->cursor_size = points;
	server->cursor_generation++;
	if (server->backend.apply_cursor != NULL) {
		server->backend.apply_cursor(server);
	}
	server_schedule_if_running(server);
}

int aws_server_set_system_menu_icon(struct aws_server *server, const char *name)
{
	struct aws_system_icon *icon;

	if (server == NULL || name == NULL || name[0] == '\0') {
		return -1;
	}
	if (strcmp(name, "auto") == 0) {
		server->system_menu_icon = aws_system_menu_icon_resolve(&server->os_identity);
		server_schedule_if_running(server);
		return 0;
	}
	icon = aws_system_icon_by_name(name);
	if (icon == NULL) {
		return -1;
	}
	server->system_menu_icon = icon;
	server_schedule_if_running(server);
	return 0;
}
