/*
 * window_list — what a Dock is told about other applications' windows, and what it can do about them.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
 *
 * **THE CLAIM IS A LOOP, NOT A NUMBER IN A FRAME.** A shell component's data source is `agora_window_list`: a
 * snapshot of every window the server has, re-sent when the set changes. This probe is the smallest possible shell
 * — it asks for the list, makes two windows of its own to have something to look at, and then does the three things
 * a Dock does with a row:
 *
 *   `activate_window`  raises that window in its level AND gives it the keyboard, which is what a click on a tile
 *                      is — the application's menu bar follows the keyboard, so this is the observable half;
 *   `close_window`     sends `agora_window.close` to the window's APPLICATION, which decides (the event was declared
 *                      in version 1 and never sent until this door existed);
 *   an id nobody answers  is ignored, because a Dock racing a window that just closed must not die.
 *
 * **AND THE PROBE MAKES ITS OWN WINDOWS RATHER THAN SHARING THE SERVER WITH ANOTHER PROCESS.** Two clients would
 * make every assertion a race; one process that is both the shell and the applications is a sequence. The windows
 * carry real buffers because `active` means "this window has the keyboard", and the server only gives the keyboard
 * to a window that has been mapped — which is what a committed buffer does.
 *
 *   window_list            (no arguments)
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

#define MAX_ROWS 16

struct row {
	unsigned int id;
	char app_id[64];
	char title[64];
	int level;
	int visible;
	int active;
};

struct state {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct agora_shell *shell;
	struct agora_window_list *list;

	struct row rows[MAX_ROWS];
	int count;		/* rows in the CURRENT snapshot */
	int in_snapshot;
	int snapshots;		/* how many complete snapshots have landed */
	int failures;

	int closes;		/* `agora_window.close` events any of this probe's windows received */
};

/* ---------------------------------------------------------------------------------------------------
 * the window list
 * --------------------------------------------------------------------------------------------------- */

static void list_begin(void *data, struct agora_window_list *list)
{
	struct state *state = data;

	(void)list;
	state->in_snapshot = 1;
	state->count = 0;
}

static void list_window(void *data, struct agora_window_list *list, uint32_t id, const char *app_id,
			const char *title, int32_t level, int32_t visible, int32_t active)
{
	struct state *state = data;
	struct row *row;

	(void)list;
	if (state->count >= MAX_ROWS) {
		return;
	}
	row = &state->rows[state->count++];
	row->id = id;
	snprintf(row->app_id, sizeof row->app_id, "%s", app_id != NULL ? app_id : "");
	snprintf(row->title, sizeof row->title, "%s", title != NULL ? title : "");
	row->level = (int)level;
	row->visible = (int)visible;
	row->active = (int)active;
}

static void list_done(void *data, struct agora_window_list *list)
{
	struct state *state = data;

	(void)list;
	state->in_snapshot = 0;
	state->snapshots++;
}

static const struct agora_window_list_listener list_listener = {
	list_begin,
	list_window,
	list_done,
};

/* ---------------------------------------------------------------------------------------------------
 * the probe's own windows
 * --------------------------------------------------------------------------------------------------- */

static void window_close(void *data, struct agora_window *window)
{
	struct state *state = data;

	(void)window;
	state->closes++;
}

static void window_work_area_changed(void *data, struct agora_window *window, int32_t x, int32_t y, int32_t width,
				     int32_t height)
{
	(void)data; (void)window; (void)x; (void)y; (void)width; (void)height;
}

static void window_pointer_global(void *data, struct agora_window *window, int32_t x, int32_t y)
{
	(void)data; (void)window; (void)x; (void)y;
}

/* IN THE PROTOCOL'S OWN ORDER: `close`, `work_area_changed`, `pointer_global`. */
static const struct agora_window_listener window_listener = {
	window_close,
	window_work_area_changed,
	window_pointer_global,
};

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
		state->shell = wl_registry_bind(registry, name, &agora_shell_interface, version < 4 ? version : 4);
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

/* A BUFFER, BECAUSE `active` NEEDS THE WINDOW MAPPED: the server gives the keyboard to a surface that has one, and
 * to nothing else. One flat colour is enough — nothing here looks at pixels. */
static struct wl_buffer *make_buffer(struct state *state, int width, int height, uint32_t colour)
{
	int stride = width * 4;
	size_t size = (size_t)stride * (size_t)height;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	char name[64];
	uint32_t *pixels;
	int fd;

	snprintf(name, sizeof name, "/agora-window-list-%d", (int)getpid());
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

/* ---------------------------------------------------------------------------------------------------
 * the assertions
 * --------------------------------------------------------------------------------------------------- */

static void fail(struct state *state, const char *what)
{
	printf("FAIL: %s\n", what);
	state->failures++;
}

/* ONE ROUNDTRIP, WHICH IS WHAT MAKES EVERY ASSERTION BELOW DETERMINISTIC: a sync makes the server handle every
 * request sent before it, and every snapshot it emitted on the way arrives before the callback. */
static int settle(struct state *state)
{
	if (wl_display_roundtrip(state->display) == -1) {
		fail(state, "the connection died");
		return -1;
	}
	return 0;
}

static const struct row *find(const struct state *state, unsigned int id)
{
	int i;

	for (i = 0; i < state->count; i++) {
		if (state->rows[i].id == id) {
			return &state->rows[i];
		}
	}
	return NULL;
}

int main(void)
{
	struct state state;
	struct wl_surface *surface_a;
	struct wl_surface *surface_b;
	struct agora_window *window_a;
	struct agora_window *window_b;
	struct wl_buffer *buffer_a;
	struct wl_buffer *buffer_b;
	const struct row *row;
	unsigned int id_a = 0;
	unsigned int id_b = 0;

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
	if (state.compositor == NULL || state.shm == NULL || state.shell == NULL) {
		printf("FAIL: the server is missing a global this test needs\n");
		return 1;
	}

	/* **THE SHELL ASKS, AND THE FIRST SNAPSHOT COMES BACK BEFORE ANY WINDOW EXISTS.** That empty snapshot is the
	 * one a client clearing its model on `begin` needs, so it is asserted rather than assumed. */
	state.list = agora_shell_get_window_list(state.shell);
	agora_window_list_add_listener(state.list, &list_listener, &state);
	if (settle(&state) != 0) {
		return 1;
	}
	if (state.snapshots < 1 || state.count != 0) {
		fail(&state, "the first snapshot was not an empty one");
	} else {
		printf("ok: the first snapshot is empty\n");
	}

	/* **WINDOW A, WHICH MAPS AND THEREFORE HAS THE KEYBOARD.** A is the application that is "in front" with no
	 * click at all — the server's initial-focus rule — and that is what `active` has to say. */
	surface_a = wl_compositor_create_surface(state.compositor);
	window_a = agora_shell_get_window(state.shell, surface_a);
	agora_window_add_listener(window_a, &window_listener, &state);
	agora_window_set_app_id(window_a, "io.github.agoradesktop.probe.a");
	agora_window_set_title(window_a, "A");
	agora_window_set_frame(window_a, 0, 40, 200, 100);
	buffer_a = make_buffer(&state, 200, 100, 0x00FF0000u);
	if (buffer_a == NULL) {
		fail(&state, "could not make window A's buffer");
		return 1;
	}
	wl_surface_attach(surface_a, buffer_a, 0, 0);
	wl_surface_damage_buffer(surface_a, 0, 0, 200, 100);
	wl_surface_commit(surface_a);
	if (settle(&state) != 0) {
		return 1;
	}
	if (state.count != 1) {
		fail(&state, "the snapshot does not have exactly one window after A mapped");
		return 1;
	}
	row = &state.rows[0];
	id_a = row->id;
	if (id_a == 0 || strcmp(row->app_id, "io.github.agoradesktop.probe.a") != 0 || strcmp(row->title, "A") != 0) {
		fail(&state, "the row for A has the wrong identity");
	} else if (row->visible != 1 || row->active != 1 || row->level != 0) {
		fail(&state, "the row for A has the wrong state — a mapped first window is visible and active");
	} else {
		printf("ok: A is listed with its app id, title, level, and the keyboard\n");
	}

	/* **AND WINDOW B, WHICH DOES NOT TAKE THE KEYBOARD FROM A.** Focus is a click's or an activation's; a second
	 * window mapping must not steal it, and the list has to say so. */
	surface_b = wl_compositor_create_surface(state.compositor);
	window_b = agora_shell_get_window(state.shell, surface_b);
	agora_window_add_listener(window_b, &window_listener, &state);
	agora_window_set_app_id(window_b, "io.github.agoradesktop.probe.b");
	agora_window_set_title(window_b, "B");
	agora_window_set_frame(window_b, 240, 40, 200, 100);
	buffer_b = make_buffer(&state, 200, 100, 0x0000FF00u);
	if (buffer_b == NULL) {
		fail(&state, "could not make window B's buffer");
		return 1;
	}
	wl_surface_attach(surface_b, buffer_b, 0, 0);
	wl_surface_damage_buffer(surface_b, 0, 0, 200, 100);
	wl_surface_commit(surface_b);
	if (settle(&state) != 0) {
		return 1;
	}
	if (state.count != 2) {
		fail(&state, "the snapshot does not have exactly two windows after B mapped");
		return 1;
	}
	row = find(&state, id_a);
	if (row == NULL || row->active != 1) {
		fail(&state, "A lost the keyboard when B mapped");
	}
	/* AND THE SECOND ROW IS B, WHICH IS THE STACKING ORDER: the list runs back to front, and B mapped last. */
	id_b = state.rows[1].id;
	if (strcmp(state.rows[1].app_id, "io.github.agoradesktop.probe.b") != 0 || state.rows[1].active != 0) {
		fail(&state, "B is not listed last, or is wrongly marked active");
	} else {
		printf("ok: B is listed behind A and did not take the keyboard\n");
	}

	/* **A CLICK ON B'S TILE.** This is the whole of what the Dock asks for. */
	agora_shell_activate_window(state.shell, id_b);
	if (settle(&state) != 0) {
		return 1;
	}
	row = find(&state, id_a);
	if (row == NULL || row->active != 0) {
		fail(&state, "activating B left A active");
	}
	row = find(&state, id_b);
	if (row == NULL || row->active != 1) {
		fail(&state, "activating B did not give it the keyboard");
	} else {
		printf("ok: activate_window moved the keyboard from A to B\n");
	}
	/* AND THE RAISE HALF: B is now the front of the stacking order, so it is the LAST row. Activating a window
	 * that is already in front cannot show this, which is why it is B that is activated and not A. */
	if (state.rows[state.count - 1].id != id_b) {
		fail(&state, "activating B did not raise it to the front of the list");
	} else {
		printf("ok: and it is now the front of the stacking order\n");
	}

	/* **A QUIT FROM THE TILE'S MENU.** The application is ASKED, not destroyed: the event lands on B's own
	 * listener and the window is still there until B decides. */
	agora_shell_close_window(state.shell, id_b);
	if (settle(&state) != 0) {
		return 1;
	}
	if (state.closes != 1) {
		fail(&state, "close_window did not deliver agora_window.close to B");
	} else if (find(&state, id_b) == NULL) {
		fail(&state, "close_window destroyed the window instead of asking");
	} else {
		printf("ok: close_window asked, and the window is the application's to close\n");
	}

	/* **AND AN ID NOBODY ANSWERS IS IGNORED.** A Dock clicking a tile as its window closes is ordinary; a protocol
	 * error there would kill the Dock over one stale tile. */
	agora_shell_activate_window(state.shell, id_b + 1000);
	agora_shell_close_window(state.shell, id_b + 1000);
	if (settle(&state) != 0) {
		return 1;
	}
	if (state.closes != 1) {
		fail(&state, "an unknown id was answered by SOME window");
	} else {
		printf("ok: unknown ids are ignored rather than errors\n");
	}

	/* **A WINDOW THAT MAY NOT ACTIVATE REFUSES THE KEYBOARD, AND A DOCK NEEDS THAT.** `set_activatable 0` is what
	 * keeps a click on the Dock from taking the menu bar with it. Switching B off while it HAS the keyboard must
	 * also hand the keyboard back — the one ordering a client cannot do for itself, because it does not know who
	 * else is on the screen. */
	agora_window_set_activatable(window_b, 0);
	if (settle(&state) != 0) {
		return 1;
	}
	row = find(&state, id_a);
	if (row == NULL || row->active != 1) {
		fail(&state, "a window that stopped being activatable kept the keyboard");
	} else {
		printf("ok: a window made non-activatable gives the keyboard back\n");
	}
	agora_shell_activate_window(state.shell, id_b);
	if (settle(&state) != 0) {
		return 1;
	}
	row = find(&state, id_b);
	if (row == NULL || row->active != 0) {
		fail(&state, "activate_window gave a non-activatable window the keyboard");
	} else {
		printf("ok: and activate_window cannot give it the keyboard either\n");
	}
	/* AND SAID BACK ON, IT ACTIVATES NORMALLY — the door is a state, not a one-way door. */
	agora_window_set_activatable(window_b, 1);
	agora_shell_activate_window(state.shell, id_b);
	if (settle(&state) != 0) {
		return 1;
	}
	row = find(&state, id_b);
	if (row == NULL || row->active != 1) {
		fail(&state, "a window made activatable again did not take the keyboard");
	} else {
		printf("ok: and it takes it again once it is activatable\n");
	}

	/* **AND THE LIST SHRINKS WHEN A WINDOW GOES.** The surface is destroyed with the role still on it, which is
	 * the path both the reservation and the list have to survive. */
	wl_surface_destroy(surface_b);
	if (settle(&state) != 0) {
		return 1;
	}
	if (state.count != 1 || find(&state, id_a) == NULL) {
		fail(&state, "the snapshot did not shrink to A after B's surface was destroyed");
	} else {
		printf("ok: destroying B's surface removed its row\n");
	}

	printf("%s: %d failure(s), %d snapshot(s)\n", state.failures == 0 ? "PASS" : "FAIL", state.failures,
	       state.snapshots);
	wl_display_disconnect(state.display);
	return state.failures == 0 ? 0 : 1;
}
