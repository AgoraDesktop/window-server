/*
 * surface_lifetime — when the window that had the keyboard is destroyed, who has it afterwards?
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **THE BUG THIS FILE EXISTS FOR WAS AN ORDERING, NOT A LEAK.** `surface_destroy_resource` told the seat that a
 * surface was going away BEFORE taking it out of `server->surfaces`. Forget a focused surface and the seat
 * immediately asks "who should have the keyboard now?", and the answer is the topmost mapped surface — which was
 * still the surface being destroyed, because it had not left the list yet. The seat then named a dying surface as
 * the keyboard focus, the surface was unlinked and freed, and every later focus decision read a pointer to freed
 * memory. A crash that only appears on the second window close is the shape of it.
 *
 * **THE OBSERVABLE HALF IS A KEYBOARD EVENT, so this probe is a client and asserts what a client can see.** Two
 * windows map: A first (the server's initial-focus rule gives it the keyboard), then B on top of it (a newly
 * mapped window takes the keyboard). Destroy B's SURFACE while it is still the topmost mapped candidate — the
 * exact condition the ordering needs — and A must be told it has the keyboard. It never is on the unfixed source,
 * because the reassignment chose B.
 *
 * **IT DRIVES THE REAL DESTRUCTOR.** `wl_surface_destroy` on a mapped surface goes through
 * `surface_destroy_resource` in `surface.c`, the function with the ordering in it; this is not a mock of the seat's
 * focus helper. The role object outlives the surface by contract, so it is destroyed afterwards and must survive
 * its surface being gone.
 *
 * **THE SECOND HALF OF THE ISSUE IS THE SUCCESSOR CASE.** After B is destroyed the remaining candidate is A, a
 * DIFFERENT client's window — the no-successor case (nothing left to focus) is the other branch and it is not
 * reachable from one process with two windows in it, so it is not claimed here.
 *
 *   surface_lifetime              (no arguments)
 */

#define _GNU_SOURCE

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
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;

	struct wl_surface *a;
	struct wl_surface *b;

	/* what the keyboard listener last said, keyed by surface */
	struct wl_surface *focus;
	int enters;
	int leaves;
	int failures;
};

static void fail(struct state *state, const char *what)
{
	printf("FAIL: %s\n", what);
	state->failures++;
}

/* ---------------------------------------------------------------------------------------------------
 * the keyboard's own account of who has focus
 * --------------------------------------------------------------------------------------------------- */

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size)
{
	(void)data; (void)keyboard; (void)format; (void)size;
	if (fd >= 0) {
		close(fd);
	}
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
			   struct wl_surface *surface, struct wl_array *keys)
{
	struct state *state = data;

	(void)keyboard; (void)serial; (void)keys;
	state->focus = surface;
	state->enters++;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface)
{
	struct state *state = data;

	(void)keyboard; (void)serial;
	if (state->focus == surface) {
		state->focus = NULL;
	}
	state->leaves++;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
			 uint32_t key, uint32_t state_)
{
	(void)data; (void)keyboard; (void)serial; (void)time; (void)key; (void)state_;
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed,
			       uint32_t latched, uint32_t locked, uint32_t group)
{
	(void)data; (void)keyboard; (void)serial;
	(void)depressed; (void)latched; (void)locked; (void)group;
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
	(void)data; (void)keyboard; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	keyboard_keymap,
	keyboard_enter,
	keyboard_leave,
	keyboard_key,
	keyboard_modifiers,
	keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
	struct state *state = data;

	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && state->keyboard == NULL) {
		state->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(state->keyboard, &keyboard_listener, state);
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
	seat_capabilities,
	seat_name,
};

/* ---------------------------------------------------------------------------------------------------
 * the probe's windows
 * --------------------------------------------------------------------------------------------------- */

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
		state->shell = wl_registry_bind(registry, name, &agora_shell_interface, version < 6 ? version : 6);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		state->seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
		wl_seat_add_listener(state->seat, &seat_listener, state);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_global_remove,
};

/* A BUFFER, BECAUSE MAPPED IS WHAT MAKES A SURFACE A FOCUS CANDIDATE. One flat colour is enough; nothing here looks
 * at a pixel. */
static struct wl_buffer *make_buffer(struct state *state, int width, int height, uint32_t colour)
{
	int stride = width * 4;
	size_t size = (size_t)stride * (size_t)height;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	char name[64];
	uint32_t *pixels;
	int fd;

	snprintf(name, sizeof name, "/agora-surface-lifetime-%d", (int)getpid());
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
	for (size_t i = 0; i < size / sizeof *pixels; i++) {
		pixels[i] = colour;
	}
	pool = wl_shm_create_pool(state->shm, fd, (int32_t)size);
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

/* The no-op handlers differ only in signature; two of them cover every slot the probe does not care about. */
static void window_ignored(void *data, struct agora_window *window)
{
	(void)data; (void)window;
}

static void window_reopen_ignored(void *data, struct agora_window *window, int32_t item)
{
	(void)data; (void)window; (void)item;
}

static void window_work_area_changed(void *data, struct agora_window *window, int32_t x, int32_t y,
				     int32_t width, int32_t height)
{
	(void)data; (void)window; (void)x; (void)y; (void)width; (void)height;
}

static void window_pointer_global(void *data, struct agora_window *window, int32_t x, int32_t y)
{
	(void)data; (void)window; (void)x; (void)y;
}

static const struct agora_window_listener window_listener = {
	window_ignored,           /* close     — nothing here closes a window but the probe */
	window_work_area_changed,
	window_pointer_global,
	window_ignored,           /* reopen */
	window_ignored,           /* dock_menu_requested */
	window_reopen_ignored,    /* dock_menu_action  */
};

/* ONE ROUNDTRIP, WHICH IS WHAT MAKES THE ASSERTIONS A SEQUENCE: a sync makes the server handle every request sent
 * before it, and every event emitted on the way arrives before the callback. */
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
	struct agora_window *window_a;
	struct agora_window *window_b;
	struct wl_buffer *buffer_a;
	struct wl_buffer *buffer_b;

	memset(&state, 0, sizeof state);
	state.display = wl_display_connect(NULL);
	if (state.display == NULL) {
		printf("FAIL: could not connect\n");
		return 1;
	}
	state.registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(state.registry, &registry_listener, &state);
	if (wl_display_roundtrip(state.display) == -1) {
		printf("FAIL: the first roundtrip died\n");
		return 1;
	}
	if (state.compositor == NULL || state.shm == NULL || state.shell == NULL || state.seat == NULL) {
		printf("FAIL: the server is missing a global this test needs\n");
		return 1;
	}
	/* **AND A SECOND ROUNDTRIP, BECAUSE THE KEYBOARD OBJECT ARRIVES AS A REPLY.** The seat's capabilities event is
	 * what says there is a keyboard, and it comes back after the bind — so `state.keyboard` does not exist yet at
	 * the end of the first roundtrip. */
	if (settle(&state) != 0) {
		return 1;
	}
	if (state.keyboard == NULL) {
		printf("FAIL: the seat advertised no keyboard\n");
		return 1;
	}

	/* **WINDOW A MAPS FIRST AND TAKES THE KEYBOARD**, which is the server's initial-focus rule, and it is the
	 * state the test needs A to hold again at the end. */
	state.a = wl_compositor_create_surface(state.compositor);
	window_a = agora_shell_get_window(state.shell, state.a);
	agora_window_add_listener(window_a, &window_listener, &state);
	agora_window_set_app_id(window_a, "io.github.agoradesktop.lifetime.a");
	agora_window_set_title(window_a, "A");
	agora_window_set_frame(window_a, 0, 40, 200, 100);
	buffer_a = make_buffer(&state, 200, 100, 0x00FF0000u);
	if (buffer_a == NULL) {
		printf("FAIL: could not make window A's buffer\n");
		return 1;
	}
	wl_surface_attach(state.a, buffer_a, 0, 0);
	wl_surface_damage_buffer(state.a, 0, 0, 200, 100);
	wl_surface_commit(state.a);
	if (settle(&state) != 0) {
		return 1;
	}
	if (state.focus != state.a) {
		fail(&state, "A mapped and did not get the keyboard");
	} else {
		printf("ok: A mapped and has the keyboard\n");
	}

	/* **WINDOW B MAPS ON TOP OF A AND DOES NOT TAKE THE KEYBOARD FROM IT.** Focus follows a click or an
	 * activation, not a map, when a window already has it — so B is now the topmost MAPPED surface while A is
	 * still the focused one. */
	state.b = wl_compositor_create_surface(state.compositor);
	window_b = agora_shell_get_window(state.shell, state.b);
	agora_window_add_listener(window_b, &window_listener, &state);
	agora_window_set_app_id(window_b, "io.github.agoradesktop.lifetime.b");
	agora_window_set_title(window_b, "B");
	agora_window_set_frame(window_b, 40, 60, 200, 100);
	buffer_b = make_buffer(&state, 200, 100, 0x0000FF00u);
	if (buffer_b == NULL) {
		printf("FAIL: could not make window B's buffer\n");
		return 1;
	}
	wl_surface_attach(state.b, buffer_b, 0, 0);
	wl_surface_damage_buffer(state.b, 0, 0, 200, 100);
	wl_surface_commit(state.b);
	if (settle(&state) != 0) {
		return 1;
	}
	if (state.focus != state.a) {
		fail(&state, "B mapping stole the keyboard from A");
	} else {
		printf("ok: B mapped on top without taking the keyboard from A\n");
	}

	/* **AND NOW A IS RAISED BACK OVER B**, which is the state the ordering bug needs and the one the probe was
	 * wrong to try to reach by mapping order: the surface that must be destroyed is the TOPMOST mapped candidate
	 * AND the one the keyboard names, with a live window underneath it to receive the reassignment. `raise` moves
	 * the window within its level and deliberately does not touch the focus, so this leaves A in both roles. */
	agora_window_raise(window_a);
	if (settle(&state) != 0) {
		return 1;
	}
	if (state.focus != state.a) {
		fail(&state, "raising A moved the keyboard");
	} else {
		printf("ok: A is raised over B and still has the keyboard\n");
	}

	/* **THE DESTRUCTOR UNDER TEST.** A is the focused surface AND the topmost mapped one when this runs — the
	 * real `surface_destroy_resource`, reached by a real `wl_surface_destroy` on a real mapped surface. */
	wl_surface_destroy(state.a);
	if (settle(&state) != 0) {
		return 1;
	}

	/* **AND THE KEYBOARD MUST BE B'S.** On the unfixed ordering the reassignment ran while A was still in the
	 * surface list, chose A, and B was never told anything. */
	if (state.focus == state.b) {
		printf("ok: destroying the focused topmost window hands the keyboard to the window underneath\n");
	} else {
		fail(&state, "the keyboard is not B's after the topmost window was destroyed — the reassignment "
			     "chose the dying surface");
	}
	if (state.focus == state.a) {
		fail(&state, "the keyboard names the DESTROYED surface");
	}

	/* **THE ROLE OUTLIVES THE SURFACE, WHICH IS THE CONTRACT**, so destroying it afterwards must be harmless —
	 * its user data was cleared when the surface went. This is the second use-after-free the same teardown could
	 * have had, and it is here so a future "clear the role last" cannot pass unnoticed. */
	agora_window_destroy(window_a);
	if (settle(&state) != 0) {
		return 1;
	}
	if (state.focus != state.b) {
		fail(&state, "destroying the orphaned role moved the keyboard");
	} else {
		printf("ok: the role outlives its surface and is destroyed cleanly\n");
	}

	wl_surface_destroy(state.b);
	agora_window_destroy(window_b);
	wl_display_roundtrip(state.display);

	wl_keyboard_destroy(state.keyboard);
	wl_seat_destroy(state.seat);
	wl_shm_destroy(state.shm);
	wl_compositor_destroy(state.compositor);
	wl_registry_destroy(state.registry);
	wl_display_disconnect(state.display);

	printf("surface_lifetime: %d case(s) failed\n", state.failures);
	return state.failures == 0 ? 0 : 1;
}
