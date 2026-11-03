/*
 * system_menu — the square's chip, photographed: claim the System menu and say it is up.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **THE CHIP IS THE ONE THING ABOUT THE SYSTEM MENU THAT CAN BE CHECKED WITHOUT A MENU.** Everything else is a client's
 * window — text, a tracking loop, a highlight — and the AppKit's own suite covers that. The chip is the SERVER's pixels:
 * the square lights up while the menu is down, drawn from the numbers in `server.h`, and this probe is what turns it on
 * for a photograph. It claims the square and sends `set_system_menu_open` with a rectangle nobody reads — the chip's
 * geometry comes from the square's own mark, not from the menu.
 *
 *   system_menu            (no arguments; WAYLAND_DISPLAY names the server's socket)
 *
 * It says what it did on stdout and then holds the connection open for `--hold` seconds (two by default), because the
 * server writes its screenshot when it is stopped and a probe that left first would leave nothing to photograph.
 */

#define _GNU_SOURCE

#include <wayland-client.h>
#include "agora-shell-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct state {
	struct wl_display *display;
	struct wl_registry *registry;
	struct agora_shell *shell;
	int failures;
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
			    uint32_t version)
{
	struct state *state = data;

	if (strcmp(interface, agora_shell_interface.name) != 0) {
		return;
	}
	/* ASKED FOR AT THE HIGHEST VERSION THIS PROBE UNDERSTANDS, capped by the server's — the same rule the AppKit's seam
	 * records, because a request sent above the bind's version is a protocol error that kills the client. */
	state->shell = wl_registry_bind(registry, name, &agora_shell_interface, version < 20 ? version : 20);
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_global_remove,
};

int main(int argc, char **argv)
{
	struct state state;
	int hold = 2;
	int i;

	memset(&state, 0, sizeof state);
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
			hold = atoi(argv[++i]);
		}
	}
	state.display = wl_display_connect(NULL);
	if (state.display == NULL) {
		fprintf(stderr, "system_menu: no display (is WAYLAND_DISPLAY set?)\n");
		return 2;
	}
	state.registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(state.registry, &registry_listener, &state);
	wl_display_roundtrip(state.display);
	if (state.shell == NULL) {
		fprintf(stderr, "system_menu: this server has no agora_shell\n");
		return 1;
	}
	if (wl_proxy_get_version((struct wl_proxy *)state.shell) < 20) {
		fprintf(stderr, "system_menu: agora_shell is version %d, and the chip needs 20\n",
			wl_proxy_get_version((struct wl_proxy *)state.shell));
		return 1;
	}
	agora_shell_claim_system_menu(state.shell);
	/* THE RECTANGLE IS THE MENU'S AND THIS PROBE HAS NONE; the server reads `open` for the chip and the rectangle only
	 * when a press has to be told apart from a dismissal. */
	agora_shell_set_system_menu_open(state.shell, 1, 0, 24, 169, 180);
	wl_display_flush(state.display);
	wl_display_roundtrip(state.display);
	printf("system_menu: the square is claimed and its menu is reported up\n");
	fflush(stdout);
	sleep((unsigned)hold);
	wl_display_disconnect(state.display);
	return 0;
}
