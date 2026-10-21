/*
 * work_area — does a client's reservation actually shrink the work area, and does everyone hear about it?
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
 *
 * WHAT IT PROVES, AND WHY IT IS A PROGRAM RATHER THAN A SCREENSHOT. `agora_window.set_reserved` is the door a Dock
 * asks through, and everything it does is INVISIBLE in a frame: no pixel moves, the window does not move, and the
 * only evidence is the number `agora_output.work_area` carries. So this connects, reserves a strip, and asserts the
 * recipe the server is supposed to follow:
 *
 *     work area = output - menu bar (the server's own furniture) - every client's reservation, summed by edge
 *
 * **AND IT ASSERTS THE TWO WAYS A RESERVATION DIES**, because a Dock that crashes must not leave a strip missing
 * for ever: destroying the ROLE releases it, and destroying the SURFACE with the role still attached releases it
 * too — the second is also the use-after-free the release path had to be ordered around (see
 * `surface_destroy_resource`).
 *
 * Every request is followed by a roundtrip, which is what makes this deterministic without sleeping: a sync makes
 * the server process everything sent before it, and every event it emitted on the way arrives before the sync's
 * callback. Nothing here waits for a clock.
 *
 *   work_area            (no arguments; the server is 1280x720 and the menu bar is 24, so the baseline is 696 tall)
 */

#include <wayland-client.h>
#include "agora-shell-client-protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct state {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_output *output;
	struct agora_shell *shell;
	struct agora_output *agora_output;

	int work_x, work_y, work_width, work_height;
	int work_area_events;	/* every `agora_output.work_area` this client saw, bound one included */
	int window_changes;	/* every `agora_window.work_area_changed` the CURRENT window saw */
	int failures;
};

/* ---------------------------------------------------------------------------------------------------
 * the doors the server speaks through
 * --------------------------------------------------------------------------------------------------- */

static void output_work_area(void *data, struct agora_output *output, int32_t x, int32_t y, int32_t width,
			     int32_t height)
{
	struct state *state = data;

	(void)output;
	state->work_x = (int)x;
	state->work_y = (int)y;
	state->work_width = (int)width;
	state->work_height = (int)height;
	state->work_area_events++;
}

static void output_menu_bar_area(void *data, struct agora_output *output, int32_t x, int32_t y, int32_t width,
				 int32_t height, uint32_t colour)
{
	(void)data; (void)output; (void)x; (void)y; (void)width; (void)height; (void)colour;
}

static const struct agora_output_listener output_listener = {
	/* TWO EVENTS, AND BOTH SLOTS ARE FILLED BECAUSE BOTH ARE ALWAYS SENT: `agora_output` declares `work_area`
	 * first and `menu_bar_area` second, in that order, and libwayland ABORTS on an event whose listener slot is
	 * NULL — MEASURED while writing this test: "listener function for opcode 1 of agora_output is NULL". The
	 * bar's region is not what this test measures, so its callback is named and does nothing. */
	output_work_area,
	output_menu_bar_area,
};

static void window_close(void *data, struct agora_window *window)
{
	(void)data; (void)window;
}

static void window_work_area_changed(void *data, struct agora_window *window, int32_t x, int32_t y, int32_t width,
				     int32_t height)
{
	struct state *state = data;

	(void)window; (void)x; (void)y; (void)width; (void)height;
	state->window_changes++;
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
	} else if (strcmp(interface, wl_output_interface.name) == 0 && state->output == NULL) {
		state->output = wl_registry_bind(registry, name, &wl_output_interface, version < 2 ? version : 2);
	} else if (strcmp(interface, agora_shell_interface.name) == 0) {
		/* **ASKED FOR AT 2, WHICH IS THE VERSION `set_reserved` ARRIVED IN.** The server caps a child at the
		 * shell's version, so binding at 1 here would hand out a version 1 window and the request below
		 * would be a no-op — which is exactly the failure mode this test would otherwise have to guess at. */
		state->shell = wl_registry_bind(registry, name, &agora_shell_interface, version < 2 ? version : 2);
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

/* ---------------------------------------------------------------------------------------------------
 * the assertions
 * --------------------------------------------------------------------------------------------------- */

/* ONE ROUNDTRIP, THEN THE NUMBER. Why this is not a sleep: a `wl_display.sync` makes the server handle every
 * request sent before it, and every event the server emitted on the way is delivered before the callback. So by the
 * time this returns, the work area is whatever the request above it made it. */
static void expect(struct state *state, const char *what, int x, int y, int width, int height)
{
	if (wl_display_roundtrip(state->display) == -1) {
		printf("FAIL: %s: the connection died\n", what);
		state->failures++;
		return;
	}
	if (state->work_x != x || state->work_y != y || state->work_width != width || state->work_height != height) {
		printf("FAIL: %s: expected %d,%d %dx%d, got %d,%d %dx%d\n", what, x, y, width, height, state->work_x,
		       state->work_y, state->work_width, state->work_height);
		state->failures++;
		return;
	}
	printf("ok: %s: %d,%d %dx%d\n", what, state->work_x, state->work_y, state->work_width, state->work_height);
}

int main(void)
{
	struct state state;
	struct wl_surface *surface;
	struct wl_surface *left_surface;
	struct agora_window *window;
	struct agora_window *left_window;
	int failures;

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
	if (state.compositor == NULL || state.output == NULL || state.shell == NULL) {
		printf("FAIL: the server is missing a global this test needs\n");
		return 1;
	}
	/* **THE OUTPUT'S COMPANION, WHICH IS WHERE `NSScreen.visibleFrame` COMES FROM.** It is a separate object from
	 * `wl_output` on purpose: that one describes what the screen IS, this one what is LEFT of it. */
	state.agora_output = agora_shell_get_output(state.shell, state.output);
	agora_output_add_listener(state.agora_output, &output_listener, &state);

	/* THE BASELINE: 1280 x (720 - 24), with nothing reserved by anybody. */
	expect(&state, "nothing reserved", 0, 24, 1280, 696);

	surface = wl_compositor_create_surface(state.compositor);
	window = agora_shell_get_window(state.shell, surface);
	agora_window_add_listener(window, &window_listener, &state);

	/* **A DOCK-SHAPED RESERVATION.** 96 pixels along the bottom, which is the one thing a Dock has to be able to
	 * say and the one thing the server could not previously be told. */
	agora_window_set_reserved(window, AGORA_WINDOW_EDGE_BOTTOM, 96);
	expect(&state, "96 pixels reserved at the bottom", 0, 24, 1280, 600);
	/* AND THE WINDOW ITSELF WAS TOLD, not only the output: `work_area_changed` is the per-window form and it was
	 * declared-but-never-sent before this. */
	if (state.window_changes != 1) {
		printf("FAIL: work_area_changed: expected 1, got %d\n", state.window_changes);
		state.failures++;
	} else {
		printf("ok: work_area_changed arrived once for the change\n");
	}

	/* **A SECOND WINDOW, BECAUSE A WINDOW HAS ONE RESERVATION AND TWO EDGES ARE TWO OWNERS.** A panel on the left
	 * and a Dock at the bottom are two reservations and neither may cancel the other. */
	left_surface = wl_compositor_create_surface(state.compositor);
	left_window = agora_shell_get_window(state.shell, left_surface);
	agora_window_set_reserved(left_window, AGORA_WINDOW_EDGE_LEFT, 40);
	expect(&state, "and 40 pixels at the left, from a second window", 40, 24, 1240, 600);

	/* AND ONE WINDOW ASKING AGAIN MOVES ITS STRIP rather than adding a second one — the Dock dragged from the
	 * bottom to the side. This window goes from the left edge to the right one, and the bottom window's 96 is
	 * untouched while its own 40 is gone. */
	agora_window_set_reserved(left_window, AGORA_WINDOW_EDGE_RIGHT, 40);
	expect(&state, "the left window moving to the right, the bottom one still reserved", 0, 24, 1240, 600);

	/* AND GIVING ONE BACK leaves the other one standing. */
	agora_window_set_reserved(left_window, AGORA_WINDOW_EDGE_RIGHT, 0);
	expect(&state, "the right one released, the bottom one still reserved", 0, 24, 1280, 600);
	wl_surface_destroy(left_surface);

	/* **THE ROLE GOES, THE RESERVATION GOES WITH IT.** A client that exits without saying so — which is what a
	 * crash is — must not leave the screen short of a strip. */
	agora_window_destroy(window);
	expect(&state, "the role destroyed", 0, 24, 1280, 696);

	/* **AND THE OTHER DOOR: THE SURFACE GOES WITH THE ROLE STILL ON IT.** This is the path that used to leave a
	 * freed surface behind the role's own destroy handler, and the reservation has to die here too. */
	window = agora_shell_get_window(state.shell, surface);
	agora_window_set_reserved(window, AGORA_WINDOW_EDGE_RIGHT, 64);
	expect(&state, "a new role reserving 64 at the right", 0, 24, 1216, 696);
	wl_surface_destroy(surface);
	expect(&state, "the surface destroyed with the role still attached", 0, 24, 1280, 696);

	failures = state.failures;
	printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
	wl_display_disconnect(state.display);
	return failures == 0 ? 0 : 1;
}
