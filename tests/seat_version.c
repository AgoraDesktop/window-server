/*
 * seat_version — which keyboard events may a client of version N actually receive?
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **THE CLAIM IS ABOUT THE WIRE, NOT ABOUT THE COMPOSITOR'S POLICY.** `wl_keyboard.repeat_info` is a version-4
 * event. The server advertises its seat at version 5, but a client binds whatever version it chooses, and a
 * keyboard object inherits the seat resource's version — so a version-1, 2 or 3 client asks for a keyboard that
 * cannot receive `repeat_info` at all. Sending it anyway puts an event on the wire that the client never
 * negotiated: at best the client drops it, at worst a client that checks opcodes against its bound version
 * desynchronises.
 *
 * **SO THIS PROBE BINDS ONE VERSION AND RECORDS WHAT ARRIVES.** It is deliberately the smallest real client —
 * registry, seat, keyboard, nothing else — so that what it reports is the server's decision and not its own. It
 * prints one line per event and exits, and `run-seat-versions.sh` binds 1, 2, 3, 4 and 5 and asserts the boundary.
 *
 *   seat_version <version>        bind the seat at <version> and report the keyboard events received
 */

#define _GNU_SOURCE

#include <wayland-client.h>

#include <errno.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct probe {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	int bound_version;
	int have_keymap;
	int have_repeat_info;
	int repeat_rate;
	int repeat_delay;
	const char *protocol_error;
};

/* **THE KEYMAP IS A FILE DESCRIPTOR, AND THE PROBE MUST CLOSE IT.** Leaving it open would not fail this test, but a
 * client that leaks the keymap is a client with a bug, and the fd is the one thing here that is really the
 * client's to own. */
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
			    int32_t fd, uint32_t size)
{
	struct probe *probe = data;

	(void)keyboard;
	(void)format;
	(void)size;
	probe->have_keymap++;
	if (fd >= 0) {
		close(fd);
	}
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
			   struct wl_surface *surface, struct wl_array *keys)
{
	(void)data; (void)keyboard; (void)serial; (void)surface; (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
			   struct wl_surface *surface)
{
	(void)data; (void)keyboard; (void)serial; (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
			 uint32_t time, uint32_t key, uint32_t state)
{
	(void)data; (void)keyboard; (void)serial; (void)time; (void)key; (void)state;
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
			       uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
	(void)data; (void)keyboard; (void)serial;
	(void)depressed; (void)latched; (void)locked; (void)group;
}

/* **THE EVENT UNDER TEST.** Present in the listener struct at every version, because the struct is the client's
 * and not the protocol's — so writing the field is always legal, and what makes it reachable is the VERSION the
 * proxy was created at, which is the server's to respect. */
static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
	struct probe *probe = data;

	(void)keyboard;
	probe->have_repeat_info++;
	probe->repeat_rate = rate;
	probe->repeat_delay = delay;
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
	struct probe *probe = data;

	(void)seat;
	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && probe->keyboard == NULL) {
		/* **THE KEYBOARD INHERITS THE SEAT'S VERSION**, which is the whole mechanism under test: a client that
		 * bound the seat at 3 gets a keyboard resource at 3, and the server must not speak version 4 to it. */
		probe->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(probe->keyboard, &keyboard_listener, probe);
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

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
			    const char *interface, uint32_t version)
{
	struct probe *probe = data;

	if (strcmp(interface, wl_seat_interface.name) != 0 || probe->seat != NULL) {
		return;
	}

	/* **THE CLIENT'S CHOICE IS THE SMALLEST OF THE THREE NUMBERS**: what it wants, what the server advertises,
	 * and what its own libwayland knows. Asking for more than the server offers is a protocol error, so a client
	 * that hard-codes 5 cannot run against an older server — which is why the clamp is here rather than in the
	 * caller. */
	int wanted = probe->bound_version;
	int use = wanted < (int)version ? wanted : (int)version;

	probe->seat = wl_registry_bind(registry, name, &wl_seat_interface, (uint32_t)use);
	probe->bound_version = (int)wl_seat_get_version(probe->seat);
	wl_seat_add_listener(probe->seat, &seat_listener, probe);
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
	struct probe probe;

	if (argc != 2) {
		fprintf(stderr, "usage: seat_version <version>\n");
		return 2;
	}
	memset(&probe, 0, sizeof probe);
	probe.bound_version = atoi(argv[1]);
	if (probe.bound_version < 1 || probe.bound_version > 9) {
		fprintf(stderr, "seat_version: version out of range: %s\n", argv[1]);
		return 2;
	}

	probe.display = wl_display_connect(NULL);
	if (probe.display == NULL) {
		fprintf(stderr, "seat_version: no display\n");
		return 2;
	}
	probe.registry = wl_display_get_registry(probe.display);
	wl_registry_add_listener(probe.registry, &registry_listener, &probe);
	if (wl_display_roundtrip(probe.display) < 0) {
		fprintf(stderr, "seat_version: registry roundtrip failed: %s\n",
			strerror(errno));
		return 2;
	}
	/* ONE MORE ROUND TRIP: the capabilities event is a reply to the bind, so the keyboard object exists only
	 * after the server has spoken again. */
	if (wl_display_roundtrip(probe.display) < 0) {
		fprintf(stderr, "seat_version: seat roundtrip failed: %s\n", strerror(errno));
		return 2;
	}
	if (probe.seat == NULL) {
		fprintf(stderr, "seat_version: the server advertised no seat\n");
		return 2;
	}
	if (probe.keyboard == NULL) {
		fprintf(stderr, "seat_version: the seat advertised no keyboard\n");
		return 2;
	}
	/* THE KEYMAP AND THE REPEAT POLICY ARE BOTH SENT AT CREATION, so one more round trip collects them and there
	 * is nothing to wait for afterwards. */
	if (wl_display_roundtrip(probe.display) < 0) {
		fprintf(stderr, "seat_version: keyboard roundtrip failed: %s\n", strerror(errno));
		return 2;
	}

	printf("bound=%d keymap=%d repeat_info=%d rate=%d delay=%d\n",
	       probe.bound_version, probe.have_keymap, probe.have_repeat_info,
	       probe.repeat_rate, probe.repeat_delay);

	wl_keyboard_destroy(probe.keyboard);
	wl_seat_destroy(probe.seat);
	wl_registry_destroy(probe.registry);
	wl_display_disconnect(probe.display);
	return 0;
}
