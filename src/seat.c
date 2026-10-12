/*
 * seat.c — the seat, and the keymap this version can honestly supply.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
 *
 * WHAT A SEAT IS. Wayland does not have "a mouse" and "a keyboard"; it has a SEAT — one person's input on one
 * machine — and a client asks the seat for the devices it advertises. The capabilities event is therefore a
 * PROMISE: a client creates a keyboard object because the seat said it has one.
 *
 * THIS VERSION ADVERTISES A KEYBOARD AND DELIVERS ITS KEYMAP, AND NOTHING ELSE. The keymap is real: it is built
 * with xkbcommon and sent the way the protocol sends one, through a file descriptor, so a client's key
 * translation is exercised against a map a COMPOSITOR supplied rather than one it made up for itself. Keys
 * THEMSELVES need a device to read, and `libinput` is the dependency that would bring one — it is not taken
 * yet, so no key event is ever generated.
 *
 * THE POINTER IS NOT ADVERTISED AT ALL, which is the honest choice rather than the conservative one: a seat
 * that promises a pointer and never sends a motion is a client that creates an object and waits, and the
 * difference between "not yet" and "broken" is exactly the capability bit.
 */

#include "server.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

/* ---------------------------------------------------------------------------------------------------
 * the keymap, built once and copied to every keyboard
 * --------------------------------------------------------------------------------------------------- */

/* AN ANONYMOUS FILE, because the protocol hands a keymap over as a file descriptor and not as bytes. It is
 * unlinked the moment it exists, so no second process can open it by name and nothing is left in /dev/shm. */
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

/* THE KEYMAP IS COMPILED FROM THE SYSTEM'S OWN XKB DATA (`xkb_keymap_new_from_names`), which is where a
 * desktop's layout lives; a compositor with a configuration file would read it here instead. */
static const char *keymap_text(size_t *size)
{
	static char *text;
	static size_t length;

	if (text == NULL) {
		struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_keymap *keymap;

		if (context == NULL) {
			return NULL;
		}
		keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (keymap != NULL) {
			text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
			length = text != NULL ? strlen(text) + 1 : 0;
			xkb_keymap_unref(keymap);
		}
		xkb_context_unref(context);
	}
	if (size != NULL) {
		*size = length;
	}
	return text;
}

static void keyboard_release(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_keyboard_interface keyboard_implementation = {
	keyboard_release,
};

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct wl_resource *keyboard;
	size_t size = 0;
	const char *text = keymap_text(&size);
	int fd;
	void *mapped;

	keyboard = wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
	if (keyboard == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(keyboard, &keyboard_implementation, NULL, NULL);
	if (text == NULL || size == 0) {
		return;
	}
	fd = create_anonymous_file(size);
	if (fd < 0) {
		return;
	}
	mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapped == MAP_FAILED) {
		close(fd);
		return;
	}
	memcpy(mapped, text, size);
	munmap(mapped, size);
	/* XKB_V1 IS THE ONLY FORMAT ANYONE SENDS, and the `size` is the map's length INCLUDING its NUL — a client
	 * that reads to the NUL and one that reads `size` bytes must agree, and they do only if it is sent. */
	wl_keyboard_send_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
	/* The descriptor is duplicated into the message, so this one is finished with. */
	close(fd);
	wl_keyboard_send_repeat_info(keyboard, 25, 600);
}

static void seat_get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	/* THE OBJECT IS MADE BECAUSE THE REQUEST EXISTS, but the capability bit never advertises it, so a client
	 * that got here ignored the bit — and an object with no events is a better answer than an error. */
	wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(resource), id);
}

static void seat_get_touch(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	(void)client; (void)resource; (void)id;
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
	struct wl_resource *resource = wl_resource_create(client, &wl_seat_interface,
							  version < 5 ? version : 5, id);

	(void)data;
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &seat_implementation, data, NULL);
	wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_KEYBOARD);
	if (wl_resource_get_version(resource) >= 2) {
		wl_seat_send_name(resource, "seat0");
	}
}

void aws_seat_init(struct aws_server *server)
{
	wl_global_create(server->display, &wl_seat_interface, 5, server, bind_seat);
}
