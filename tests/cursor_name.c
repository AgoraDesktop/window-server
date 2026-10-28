/*
 * cursor_name — `agora_window.set_cursor`, driven by a client that has a window to say it about.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **THE PROBLEM THIS DOOR EXISTS FOR IS THAT THE POINTER'S SHAPE HAD NO WIRE AT ALL.** An application resolved the
 * right `NSCursor` from its own view tree, installed it, and the answer died there: the compositor drew its theme's
 * arrow over every window because nothing could tell it otherwise. `set_cursor` is the sentence that crosses — an
 * Xcursor NAME, resolved by the compositor against the theme it already has — and this probe is the part of it a
 * CLIENT can observe.
 *
 * **WHAT A CLIENT CAN AND CANNOT SEE, SAID OUT LOUD.** The shape reaches the glass through the compositor's own
 * pointer, so no event tells this process what is being drawn over it: the claim is asserted from the SERVER's side,
 * through `AWS_CURSOR_DIAG`, which names the window and what its name resolved to (`src/cursor.c`). That is why this
 * runner greps the server's log rather than the probe's output — a cursor is exactly the thing a nested session hands
 * to the host, and the existing cursor suite says so in as many words. What the PROBE asserts is the half a client
 * owns: that the global advertises the version that carries the request (a request above the bound version is a
 * protocol error that kills the connection, so this is not a formality) and that a mapped window can send all four
 * kinds of name — a shape, the default, none at all, and one this theme does not have — without the connection dying.
 *
 *   cursor_name        (no arguments; WAYLAND_DISPLAY says which server)
 */

#include <wayland-client.h>
#include "agora-shell-client-protocol.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct state {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct agora_shell *shell;
	struct wl_surface *surface;
	struct agora_window *window;
	uint32_t shell_version;		/* what the compositor said this global is */
	int failures;
};

static void fail(struct state *state, const char *what)
{
	printf("FAIL: %s\n", what);
	state->failures++;
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
			    uint32_t version)
{
	struct state *state = data;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
						     version < 4 ? version : 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, agora_shell_interface.name) == 0) {
		/* **THE HIGHEST VERSION THIS PROBE KNOWS, WHICH HERE IS THE REQUEST'S OWN.** A window made from a shell
		 * bound below 11 is capped to that version, and its `set_cursor` would then be a request the server does not
		 * implement — a protocol error, not an ignored call. */
		state->shell_version = version;
		state->shell = wl_registry_bind(registry, name, &agora_shell_interface, version < 11 ? version : 11);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_global_remove,
};

/* A BUFFER, BECAUSE A SURFACE WITH NO ROLE AND NO BUFFER IS NOT A WINDOW. One flat colour; nothing here looks at a
 * pixel. */
static struct wl_buffer *make_buffer(struct state *state, int width, int height, uint32_t colour)
{
	int stride = width * 4;
	size_t size = (size_t)stride * (size_t)height;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	char name[64];
	uint32_t *pixels;
	size_t i;
	int fd;

	snprintf(name, sizeof name, "/agora-cursor-name-%d", (int)getpid());
	fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		return NULL;
	}
	shm_unlink(name);
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return NULL;
	}
	pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	for (i = 0; i < size / sizeof *pixels; i++) {
		pixels[i] = colour;
	}
	pool = wl_shm_create_pool(state->shm, fd, (int32_t)size);
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

/* The window's events are not what this probe is about; one no-op per signature is enough. */
static void window_ignored(void *data, struct agora_window *window)
{
	(void)data;
	(void)window;
}

static void window_item_ignored(void *data, struct agora_window *window, int32_t item)
{
	(void)data;
	(void)window;
	(void)item;
}

static void window_work_area_changed(void *data, struct agora_window *window, int32_t x, int32_t y,
				     int32_t width, int32_t height)
{
	(void)data;
	(void)window;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}

static void window_pointer_global(void *data, struct agora_window *window, int32_t x, int32_t y)
{
	(void)data;
	(void)window;
	(void)x;
	(void)y;
}

static const struct agora_window_listener window_listener = {
	window_ignored,           /* close */
	window_work_area_changed,
	window_pointer_global,
	window_ignored,           /* reopen */
	window_ignored,           /* dock_menu_requested */
	window_item_ignored,      /* dock_menu_action */
};

static int settle(struct state *state)
{
	if (wl_display_roundtrip(state->display) == -1) {
		fail(state, "the connection died");
		return -1;
	}
	return 0;
}

int main(void)
{
	struct state state;
	struct wl_buffer *buffer;

	memset(&state, 0, sizeof state);
	state.display = wl_display_connect(NULL);
	if (state.display == NULL) {
		printf("FAIL: could not connect\n");
		return 1;
	}
	state.registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(state.registry, &registry_listener, &state);
	if (settle(&state) != 0) {
		return 1;
	}
	if (state.compositor == NULL || state.shm == NULL || state.shell == NULL) {
		printf("FAIL: the server is missing a global this test needs\n");
		return 1;
	}

	/* 1. **THE GLOBAL CARRIES THE REQUEST.** Eleven is `set_cursor`'s version, and it is the SHELL's number that
	 * governs it because a window inherits the version of the object that made it. */
	if (state.shell_version >= 11) {
		printf("ok: agora_shell advertises version %u, which carries set_cursor\n", state.shell_version);
	} else {
		char why[128];

		snprintf(why, sizeof why, "agora_shell is version %u, so no window can carry a cursor name",
			 state.shell_version);
		fail(&state, why);
	}

	/* 2. **AND A MAPPED WINDOW CAN NAME ITS SHAPE.** Mapping first so the server has a real window with a number of
	 * its own — the diagnostic names that number, so a request from a window that was never mapped would be a weaker
	 * claim than the one a real application makes. */
	state.surface = wl_compositor_create_surface(state.compositor);
	state.window = agora_shell_get_window(state.shell, state.surface);
	agora_window_add_listener(state.window, &window_listener, &state);
	agora_window_set_app_id(state.window, "io.github.agoradesktop.cursor-name");
	agora_window_set_title(state.window, "cursor name");
	agora_window_set_frame(state.window, 0, 40, 200, 100);
	buffer = make_buffer(&state, 200, 100, 0x00204060u);
	if (buffer == NULL) {
		printf("FAIL: could not make the window's buffer\n");
		return 1;
	}
	wl_surface_attach(state.surface, buffer, 0, 0);
	wl_surface_damage_buffer(state.surface, 0, 0, 200, 100);
	wl_surface_commit(state.surface);
	if (settle(&state) != 0) {
		return 1;
	}

	/* 3. **THE FOUR NAMES THE PROTOCOL DEFINES.** A shape the theme has, `none` (no pointer at all), a name no theme
	 * has, and the empty string that means "the default". Each is sent on its own so the SERVER's log carries four
	 * distinct answers; the probe's claim is that none of them killed the connection, which is what an unknown request
	 * or an unparseable argument would do. */
	agora_window_set_cursor(state.window, "text");
	if (settle(&state) != 0) {
		return 1;
	}
	agora_window_set_cursor(state.window, "none");
	if (settle(&state) != 0) {
		return 1;
	}
	agora_window_set_cursor(state.window, "agora-no-such-cursor");
	if (settle(&state) != 0) {
		return 1;
	}
	agora_window_set_cursor(state.window, "");
	if (settle(&state) != 0) {
		return 1;
	}
	printf("ok: a mapped window named four shapes and the connection stayed up\n");

	/* **AND A NAME THAT COULD NOT BE A THEME ENTRY IS REFUSED RATHER THAN TRUNCATED** into a different name that
	 * might exist. The server says so in its log and the client is not disconnected: a cosmetic request is not a
	 * reason to kill an application. */
	{
		char long_name[160];

		memset(long_name, 'x', sizeof long_name - 1);
		long_name[sizeof long_name - 1] = '\0';
		agora_window_set_cursor(state.window, long_name);
		if (settle(&state) != 0) {
			return 1;
		}
		printf("ok: an over-long shape name was refused without taking the connection with it\n");
	}

	wl_surface_destroy(state.surface);
	agora_window_destroy(state.window);
	wl_display_roundtrip(state.display);

	wl_shm_destroy(state.shm);
	wl_compositor_destroy(state.compositor);
	agora_shell_destroy(state.shell);
	wl_registry_destroy(state.registry);
	wl_display_disconnect(state.display);

	printf("cursor_name: %d failure(s)\n", state.failures);
	return state.failures == 0 ? 0 : 1;
}
