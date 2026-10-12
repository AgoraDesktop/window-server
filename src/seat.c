/*
 * seat.c — the seat: wl_pointer, wl_keyboard, focus, and the keymap.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
 *
 * A SEAT IS ONE PERSON'S INPUT, and the capabilities event is a PROMISE: a client creates a keyboard object
 * because the seat said there is one. Both are advertised now, and both are delivered.
 *
 * FOCUS IS THE COMPOSITOR'S DECISION AND NOBODY ELSE'S, which is what makes it the window server's job rather
 * than the client's. Two questions, two answers:
 *
 *   THE POINTER is over the TOPMOST mapped surface containing the point — a hit test, which is why this file
 *   asks `aws_surface_at` rather than looking at the list order.
 *   THE KEYBOARD goes to the CLICKED window. This version has one focus policy and it is click-to-focus, which
 *   is a POLICY: the Mac's is click-through-with-window-raising, and a window manager's is more than that.
 *   Written here so the policy has one place to change.
 *
 * THE FOCUS POLICY IS TWO HALVES, and both are needed: A NEW WINDOW GETS THE KEYBOARD when it is mapped, so
 * that a person can type into the first thing that appears, and A CLICK MOVES IT. Without the first half a
 * window that was mapped but never clicked receives nothing — MEASURED as key events arriving with the focus
 * nil and going nowhere.
 *
 * THE MODIFIERS ARE SENT BEFORE THE KEY, and the order is not cosmetic. A client translates a key with its own
 * xkb state, and that state is updated by `wl_keyboard.modifiers` — so a compositor that sent the key first
 * would have the client translate Shift with Shift not yet down. MEASURED against this project's own client:
 * `AGKeyboard` updates its state from the modifiers event and nowhere else.
 *
 * AND THE KEY MAP IS THE COMPOSITOR'S, SENT AS A FILE DESCRIPTOR, so a client's key translation is exercised
 * against a map it did not choose. It is built from the system's xkb data by xkbcommon, which is where a
 * desktop's layout lives when there is no configuration file to read.
 */

#include "server.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

struct aws_pointer {
	struct wl_resource *resource;
	struct wl_list link;
};

struct aws_keyboard {
	struct wl_resource *resource;
	struct wl_list link;
};

static uint32_t seat_now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u);
}

/* ---------------------------------------------------------------------------------------------------
 * the keymap
 * --------------------------------------------------------------------------------------------------- */

/* AN ANONYMOUS FILE, because the protocol hands a keymap over as a descriptor and not as bytes. It is unlinked
 * the moment it exists, so nothing is left in /dev/shm and no second process can open it by name. */
static int create_anonymous_file(size_t size)
{
	char name[64];
	int fd;
	static int counter;

	snprintf(name, sizeof name, "/agora-window-server-%d-%d", (int)getpid(), counter++);
	fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		return -1;
	}
	shm_unlink(name);
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void seat_build_keymap(struct aws_server *server)
{
	server->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (server->xkb_context == NULL) {
		return;
	}
	server->xkb_keymap = xkb_keymap_new_from_names(server->xkb_context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (server->xkb_keymap == NULL) {
		return;
	}
	/* THE STATE IS THE COMPOSITOR'S OWN, AND IT EXISTS FOR THE MODIFIERS: a key becomes `depressed`,
	 * `latched`, `locked` and `group` here, and those four numbers are what a client is told. */
	server->xkb_state = xkb_state_new(server->xkb_keymap);
}

static void keyboard_send_keymap(struct aws_server *server, struct wl_resource *keyboard)
{
	char *text;
	size_t size;
	int fd;
	void *mapped;

	if (server->xkb_keymap == NULL) {
		return;
	}
	text = xkb_keymap_get_as_string(server->xkb_keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	if (text == NULL) {
		return;
	}
	size = strlen(text) + 1;	/* the NUL is part of the map's length and must be sent */
	fd = create_anonymous_file(size);
	if (fd < 0) {
		free(text);
		return;
	}
	mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapped != MAP_FAILED) {
		memcpy(mapped, text, size);
		munmap(mapped, size);
	}
	wl_keyboard_send_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
	close(fd);
	free(text);
}

/* ---------------------------------------------------------------------------------------------------
 * wl_pointer
 * --------------------------------------------------------------------------------------------------- */

static void pointer_set_cursor(struct wl_client *client, struct wl_resource *resource, uint32_t serial,
			       struct wl_resource *surface, int32_t hotspot_x, int32_t hotspot_y)
{
	(void)client; (void)resource; (void)serial; (void)surface; (void)hotspot_x; (void)hotspot_y;
	/* THE CURSOR IS THE HOST'S HERE: a nested compositor draws inside a window whose pointer the HOST is
	 * already showing, and a second cursor drawn into that window is a pointer the person did not ask for. A
	 * real output would honour this, and it is where the honouring goes. */
}

static void pointer_release(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_pointer_interface pointer_implementation = {
	pointer_set_cursor,
	pointer_release,
};

static void pointer_destroy_resource(struct wl_resource *resource)
{
	struct aws_pointer *pointer = wl_resource_get_user_data(resource);

	if (pointer != NULL) {
		wl_list_remove(&pointer->link);
		free(pointer);
	}
}

static void seat_get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct aws_server *server = wl_resource_get_user_data(resource);
	struct aws_pointer *pointer = calloc(1, sizeof *pointer);

	if (pointer == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	pointer->resource = wl_resource_create(client, &wl_pointer_interface,
					       wl_resource_get_version(resource), id);
	if (pointer->resource == NULL) {
		free(pointer);
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_insert(server->pointers.prev, &pointer->link);
	wl_resource_set_implementation(pointer->resource, &pointer_implementation, pointer,
				       pointer_destroy_resource);
}

/* ---------------------------------------------------------------------------------------------------
 * wl_keyboard
 * --------------------------------------------------------------------------------------------------- */

static void keyboard_release(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_keyboard_interface keyboard_implementation = {
	keyboard_release,
};

static void keyboard_destroy_resource(struct wl_resource *resource)
{
	struct aws_keyboard *keyboard = wl_resource_get_user_data(resource);

	if (keyboard != NULL) {
		wl_list_remove(&keyboard->link);
		free(keyboard);
	}
}

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct aws_server *server = wl_resource_get_user_data(resource);
	struct aws_keyboard *keyboard = calloc(1, sizeof *keyboard);

	if (keyboard == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	keyboard->resource = wl_resource_create(client, &wl_keyboard_interface,
						wl_resource_get_version(resource), id);
	if (keyboard->resource == NULL) {
		free(keyboard);
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_insert(server->keyboards.prev, &keyboard->link);
	wl_resource_set_implementation(keyboard->resource, &keyboard_implementation, keyboard,
				       keyboard_destroy_resource);
	keyboard_send_keymap(server, keyboard->resource);
	wl_keyboard_send_repeat_info(keyboard->resource, 25, 600);
}

static void seat_get_touch(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	(void)client; (void)resource; (void)id;
	/* NO TOUCH, AND THE CAPABILITY SAYS SO — see the seat's capabilities below. A client that asks anyway is
	 * not given an object that will never speak. */
}

static void seat_release(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_seat_interface seat_implementation = {
	seat_get_pointer,
	seat_get_keyboard,
	seat_get_touch,
	seat_release,
};

static void bind_seat(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct aws_server *server = data;
	struct wl_resource *resource = wl_resource_create(client, &wl_seat_interface,
							  version < 5 ? version : 5, id);

	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &seat_implementation, server, NULL);
	/* BOTH CAPABILITIES, AND THEY ARE A PROMISE THIS BACKEND KEEPS — a headless server without a host makes
	 * the same promise, and its seat is simply never pressed. The capability says what the SEAT HAS, not what
	 * the backend can feed it, and this seat has a keyboard and a pointer. */
	wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
	if (wl_resource_get_version(resource) >= 2) {
		wl_seat_send_name(resource, "seat0");
	}
}

/* ---------------------------------------------------------------------------------------------------
 * focus, and the four things a backend can say
 * --------------------------------------------------------------------------------------------------- */

/* THE SURFACE AT A POINT: the TOPMOST one containing it, and the list is back-to-front, so the walk is
 * reverse. A surface with no buffer is not there, and a surface is as big as the buffer it committed. */
struct aws_surface *aws_surface_at(struct aws_server *server, int x, int y)
{
	struct aws_surface *surface;

	wl_list_for_each_reverse(surface, &server->surfaces, link) {
		struct wl_shm_buffer *shm;
		int width, height;

		if (!surface->mapped || surface->current_buffer == NULL) {
			continue;
		}
		shm = wl_shm_buffer_get(surface->current_buffer);
		if (shm == NULL) {
			continue;
		}
		width = wl_shm_buffer_get_width(shm);
		height = wl_shm_buffer_get_height(shm);
		if (x >= surface->x && x < surface->x + width && y >= surface->y && y < surface->y + height) {
			return surface;
		}
	}
	return NULL;
}

static int same_client(struct wl_resource *a, struct wl_resource *b)
{
	return a != NULL && b != NULL && wl_resource_get_client(a) == wl_resource_get_client(b);
}

static void pointer_send_leave(struct aws_server *server, struct aws_surface *surface)
{
	struct aws_pointer *pointer;
	uint32_t serial = wl_display_next_serial(server->display);

	wl_list_for_each(pointer, &server->pointers, link) {
		if (same_client(pointer->resource, surface->resource)) {
			wl_pointer_send_leave(pointer->resource, serial, surface->resource);
		}
	}
}

static void pointer_send_enter(struct aws_server *server, struct aws_surface *surface)
{
	struct aws_pointer *pointer;
	uint32_t serial = wl_display_next_serial(server->display);
	wl_fixed_t sx = wl_fixed_from_int(server->pointer_x - surface->x);
	wl_fixed_t sy = wl_fixed_from_int(server->pointer_y - surface->y);

	wl_list_for_each(pointer, &server->pointers, link) {
		if (same_client(pointer->resource, surface->resource)) {
			wl_pointer_send_enter(pointer->resource, serial, surface->resource, sx, sy);
		}
	}
}

static void pointer_set_focus(struct aws_server *server, struct aws_surface *surface)
{
	if (server->pointer_focus == surface) {
		return;
	}
	if (server->pointer_focus != NULL) {
		pointer_send_leave(server, server->pointer_focus);
	}
	server->pointer_focus = surface;
	if (surface != NULL) {
		pointer_send_enter(server, surface);
	}
}

static void keyboard_send_modifiers(struct aws_server *server)
{
	struct aws_keyboard *keyboard;
	xkb_mod_mask_t depressed, latched, locked;
	uint32_t group;

	if (server->xkb_state == NULL) {
		return;
	}
	depressed = xkb_state_serialize_mods(server->xkb_state, XKB_STATE_MODS_DEPRESSED);
	latched = xkb_state_serialize_mods(server->xkb_state, XKB_STATE_MODS_LATCHED);
	locked = xkb_state_serialize_mods(server->xkb_state, XKB_STATE_MODS_LOCKED);
	group = xkb_state_serialize_layout(server->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
	wl_list_for_each(keyboard, &server->keyboards, link) {
		wl_keyboard_send_modifiers(keyboard->resource, wl_display_next_serial(server->display),
					   depressed, latched, locked, group);
	}
}

void aws_seat_set_keyboard_focus(struct aws_server *server, struct aws_surface *surface)
{
	struct aws_keyboard *keyboard;
	uint32_t serial = wl_display_next_serial(server->display);
	struct wl_array keys;

	if (server->keyboard_focus == surface) {
		return;
	}
	wl_array_init(&keys);
	if (server->keyboard_focus != NULL) {
		struct aws_keyboard *k;

		wl_list_for_each(k, &server->keyboards, link) {
			if (same_client(k->resource, server->keyboard_focus->resource)) {
				wl_keyboard_send_leave(k->resource, serial, server->keyboard_focus->resource);
			}
		}
	}
	server->keyboard_focus = surface;
	if (surface != NULL) {
		wl_list_for_each(keyboard, &server->keyboards, link) {
			if (same_client(keyboard->resource, surface->resource)) {
				wl_keyboard_send_enter(keyboard->resource, serial, surface->resource, &keys);
			}
		}
	}
	wl_array_release(&keys);
}

void aws_seat_pointer_enter(struct aws_server *server, int x, int y)
{
	server->pointer_inside = 1;
	aws_seat_pointer_motion(server, x, y);
}

void aws_seat_pointer_leave(struct aws_server *server)
{
	server->pointer_inside = 0;
	pointer_set_focus(server, NULL);
}

void aws_seat_pointer_motion(struct aws_server *server, int x, int y)
{
	struct aws_surface *surface;
	struct aws_pointer *pointer;
	wl_fixed_t sx, sy;

	server->pointer_x = x;
	server->pointer_y = y;
	if (!server->pointer_inside) {
		return;
	}
	surface = aws_surface_at(server, x, y);
	pointer_set_focus(server, surface);
	if (surface == NULL) {
		return;
	}
	sx = wl_fixed_from_int(x - surface->x);
	sy = wl_fixed_from_int(y - surface->y);
	wl_list_for_each(pointer, &server->pointers, link) {
		if (same_client(pointer->resource, surface->resource)) {
			wl_pointer_send_motion(pointer->resource, seat_now_ms(), sx, sy);
		}
	}
}

void aws_seat_pointer_button(struct aws_server *server, uint32_t evdev_button, int pressed)
{
	struct aws_surface *surface;
	struct aws_pointer *pointer;
	uint32_t serial = wl_display_next_serial(server->display);

	/* CLICK-TO-FOCUS, AND IT HAPPENS ON THE WAY DOWN: the window being clicked is the window the keyboard
	 * should be talking to, and a client that heard the button before the focus would be drawing a focused
	 * control in an unfocused window. */
	if (pressed) {
		surface = aws_surface_at(server, server->pointer_x, server->pointer_y);
		aws_seat_set_keyboard_focus(server, surface);
	}
	surface = server->pointer_focus;
	if (surface == NULL) {
		return;
	}
	wl_list_for_each(pointer, &server->pointers, link) {
		if (same_client(pointer->resource, surface->resource)) {
			wl_pointer_send_button(pointer->resource, serial, seat_now_ms(), evdev_button,
					       pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
		}
	}
}

void aws_seat_key(struct aws_server *server, uint32_t evdev_keycode, int pressed)
{
	struct aws_keyboard *keyboard;
	uint32_t time = seat_now_ms();

	if (server->xkb_state != NULL) {
		xkb_state_update_key(server->xkb_state, (xkb_keycode_t)(evdev_keycode + 8),
				     pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
	}
	/* THE MODIFIERS FIRST — see this file's head for why the order matters. */
	keyboard_send_modifiers(server);
	if (server->keyboard_focus == NULL) {
		return;
	}
	wl_list_for_each(keyboard, &server->keyboards, link) {
		if (same_client(keyboard->resource, server->keyboard_focus->resource)) {
			wl_keyboard_send_key(keyboard->resource, wl_display_next_serial(server->display), time,
					     evdev_keycode,
					     pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
		}
	}
}

/* ---------------------------------------------------------------------------------------------------
 * the global
 * --------------------------------------------------------------------------------------------------- */

void aws_seat_init(struct aws_server *server)
{
	wl_list_init(&server->pointers);
	wl_list_init(&server->keyboards);
	seat_build_keymap(server);
	wl_global_create(server->display, &wl_seat_interface, 5, server, bind_seat);
}

void aws_seat_finish(struct aws_server *server)
{
	if (server->xkb_state != NULL) {
		xkb_state_unref(server->xkb_state);
	}
	if (server->xkb_keymap != NULL) {
		xkb_keymap_unref(server->xkb_keymap);
	}
	if (server->xkb_context != NULL) {
		xkb_context_unref(server->xkb_context);
	}
}
