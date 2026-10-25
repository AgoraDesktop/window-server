/*
 * seat.c — the seat: wl_pointer, wl_keyboard, focus, and the keymap.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
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
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

/* **THE TWO ELEMENT TYPES LIVE IN `server.h`**, beside the lists they belong to: a test that wants to put a pointer
 * on the seat without a client asking for one has to be able to build the element, and a second copy of the struct
 * in a test file is a copy that can silently disagree with this one. */

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
	/* **A CLIENT'S CURSOR SURFACE IS STILL NOT IMPLEMENTED, AND THE FALLBACK IS WHAT SHIPS.** A nested compositor
	 * draws inside a window whose pointer the HOST is already showing, so a second cursor drawn into that window
	 * would be a pointer the person did not ask for; a DRM output has no host pointer at all, and the compositor
	 * draws its own arrow (cursor.c) so that the shipping path has one. Honouring the client's surface is the
	 * protocol feature this request is for, and it is where the honouring goes. */
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
	/* REPEAT POLICY IS A VERSION-4 EVENT, and the seat is advertised at version 5 but BOUND at whatever the
	 * client asked for — so this resource may be a version 1, 2 or 3 keyboard, where `repeat_info` does not
	 * exist. Sending it anyway puts an event on the wire that the client's own dispatch cannot name: at best
	 * it is dropped, at worst it desynchronises a client that validates event opcodes. The seat's `name`
	 * event above is guarded the same way. */
	if (wl_resource_get_version(keyboard->resource) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION) {
		wl_keyboard_send_repeat_info(keyboard->resource, 25, 600);
	}
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
							  version < 8 ? version : 8, id);

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

/* **THE HIT TEST LIVES IN `surface.c` NOW.** It walks the surface list, the level rule and the buffer size — all of
 * which are the surface model's and none of which is the seat's — and the move is also what lets a test compile THIS
 * file and answer "what is under that point" for itself. `aws_surface_at` is declared in `server.h` and is the same
 * function it always was. */

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

/* **WHERE A POINTER EVENT GOES, AND WHY IT IS ONE FUNCTION.** Three answers in order, and the order is the whole
 * of how a drag survives leaving its own window:
 *
 *   1. A CAPTURE — an interactive grab, held by a window being dragged or resized — takes everything, whatever is
 *      under the pointer. The hand moves out from under the window it is resizing (the edge follows the pointer,
 *      and the compositor's copy of the surface is a frame behind), so "what is under the pointer" is the one
 *      question a drag cannot answer.
 *   2. A surface UNDER THE POINTER takes it, which is the ordinary case and also the one a menu bar depends on.
 *   3. Only over NO surface at all does the FALLBACK grab apply — see `set_pointer_grab`, which is not a real grab
 *      precisely so that a menu bar can be crossed back to.
 *
 * MEASURED, before the capture existed: a window resized out over its neighbour stopped following the hand the
 * moment the pointer crossed onto it, and never resumed — the press and the release had gone to the neighbour too,
 * so the client went on believing it was still resizing. Motion, buttons and the recheck all ask THIS, because
 * three copies of this precedence is three ways for a drag to starve. */
struct aws_surface *aws_seat_pointer_target(struct aws_server *server, struct aws_surface *under)
{
	if (server->pointer_capture != NULL) {
		return server->pointer_capture;
	}
	if (under != NULL) {
		return under;
	}
	return server->pointer_grab;
}

/* **THE POINTER'S FOCUS IS A FUNCTION OF WHERE THE POINTER IS AND WHAT IS UNDER IT, so it has to be recomputed
 * whenever WHAT IS UNDER IT changes.** Motion is the ordinary way that changes, and it used to be the ONLY way —
 * which is a bug with a person's name on it: a surface can APPEAR under a pointer that has not moved (the menu
 * bar mapped at startup, a menu opened under a resting hand, a window raised over one), and with no motion to
 * follow, the focus stayed NULL or on whatever used to be there. THE FIRST CLICK AIMED AT THE NEW SURFACE THEN
 * WENT NOWHERE, AND THE NEXT MOUSE MOVE MADE IT WORK — which is exactly "the click does nothing until I move
 * the mouse".
 *
 * THAT IS A PROTOCOL OBLIGATION, not a nicety: a compositor owes a surface that comes under the pointer a
 * `wl_pointer.enter` before it owes it any other event. Every door that changes the set of surfaces or their
 * geometry calls this, and it is idempotent — `pointer_set_focus` does nothing when the answer has not changed. */
void aws_seat_pointer_recheck(struct aws_server *server)
{
	if (!server->pointer_inside) {
		return;	/* the pointer is not in the output, so there is nothing under it to be over */
	}
	pointer_set_focus(server, aws_seat_pointer_target(server,
							 aws_surface_at(server, server->pointer_x, server->pointer_y)));
}

void aws_seat_surface_gone(struct aws_server *server, struct aws_surface *surface)
{
	/* **NO LEAVE IS SENT, WHICH IS THE WHOLE POINT.** This runs from the surface's own destroy handler, where
	 * the `wl_resource` a `leave` would name is the one being torn down — MEASURED, sending it there took the
	 * SERVER down. The client destroyed the surface, so it already knows; there is nothing to tell it. */
	if (server->pointer_focus == surface) {
		server->pointer_focus = NULL;
	}
	if (server->keyboard_focus == surface) {
		server->keyboard_focus = NULL;
	}
	/* AND THE POINTER FALLBACK TOO, or a destroyed menu would go on being the surface a desktop click is
	 * delivered to. */
	if (server->pointer_grab == surface) {
		server->pointer_grab = NULL;
	}
	/* AND AN INTERACTIVE CAPTURE, or a window destroyed mid-drag would keep the whole desktop's pointer. */
	if (server->pointer_capture == surface) {
		server->pointer_capture = NULL;
	}
	/* **AND THE KEYBOARD GOES SOMEWHERE ELSE RATHER THAN NOWHERE.** The window that had it is gone; a desktop with
	 * no key window can offer no menu bar, so the focus moves to the topmost window that is left. */
	if (server->keyboard_focus == NULL) {
		aws_seat_reassign_keyboard_focus(server);
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

	/* **A WINDOW THAT MAY NOT ACTIVATE NEVER TAKES THE KEYBOARD, WHICHEVER DOOR ASKED.** The click path, the
	 * initial-focus rule on map and the reassignment after a window goes away all come through here, so the check is
	 * here once rather than in three callers that could disagree — the same reason the pointer's routing is one
	 * function. The focus is left EXACTLY where it was: a click on the Dock must not deactivate the application a
	 * person is working in. */
	if (surface != NULL && !surface->activatable) {
		return;
	}
	if (server->keyboard_focus == surface) {
		return;
	}
	wl_array_init(&keys);
	if (server->keyboard_focus != NULL) {
		struct aws_keyboard *k;

		/* **AND A CLIENT MOVING FOCUS BETWEEN ITS OWN WINDOWS IS NOT DEACTIVATED.** The leave is what tells an
		 * application it has LOST the keyboard, and a client owns the keyboard as long as ANY of its windows has
		 * it — so a leave sent before the enter for a second window of the SAME client would say "you are
		 * inactive" and then "you are active" in one breath, and an application that shows its menu bar only
		 * while active would blink it. The leave is for the client actually giving the keyboard up. */
		if (surface == NULL || !same_client(server->keyboard_focus->resource, surface->resource)) {
			wl_list_for_each(k, &server->keyboards, link) {
				if (same_client(k->resource, server->keyboard_focus->resource)) {
					wl_keyboard_send_leave(k->resource, serial, server->keyboard_focus->resource);
				}
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
	/* **AND A DOCK IS TOLD WHO IS IN FRONT**, because `active` is one of the fields it reads and this is the only
	 * notion of "frontmost" the tree has. It comes after the array is released, so nothing that walks the list can
	 * see half a keyboard. */
	aws_shell_window_list_changed(server);
}

/* **THE TOPMOST WINDOW, BY THE SAME RULE THE COMPOSITOR STACKS BY: the highest LEVEL, and within a level the
 * LAST-mapped surface.** A surface the application has HIDDEN is not a candidate — it is not on the screen, so it
 * cannot be the window a keystroke goes to — and neither is one with no buffer, which is not a window yet. */
static struct aws_surface *topmost_surface(struct aws_server *server)
{
	struct aws_surface *surface;
	struct aws_surface *best = NULL;
	int best_level = 0;

	wl_list_for_each(surface, &server->surfaces, link) {
		if (!surface->mapped || !surface->visible || surface->current_buffer == NULL) {
			continue;
		}
		/* **AND A WINDOW THAT MAY NOT ACTIVATE IS NOT A CANDIDATE**, because the answer here is used when the
		 * keyboard has to be GIVEN somewhere and a Dock is not a place to give it. */
		if (!surface->activatable) {
			continue;
		}
		/* `>=` AND NOT `>`: the list runs back-to-front, so the LATER surface at the same level is the one on top
		 * of the other, which is exactly the tie the compositor's own inner loop breaks the same way. */
		if (best == NULL || surface->level >= best_level) {
			best = surface;
			best_level = surface->level;
		}
	}
	return best;
}

void aws_seat_reassign_keyboard_focus(struct aws_server *server)
{
	struct aws_surface *focus = server->keyboard_focus;

	/* STILL VALID, SO NOTHING TO DO. This is what makes it safe to call after any change: it only acts when the
	 * window that had the keyboard is gone or hidden — or was just made non-activatable under it. */
	if (focus != NULL && focus->mapped && focus->visible && focus->current_buffer != NULL && focus->activatable) {
		return;
	}
	aws_seat_set_keyboard_focus(server, topmost_surface(server));
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
	/* **AND THE FRAME THAT TAKES THE CURSOR AWAY.** A compositor-owned pointer is part of the picture, so an output
	 * with no host pointer keeps its last frame — arrow and all — until something composites again. Leaving the
	 * output is exactly such a thing. */
	if (server->backend.owns_cursor) {
		aws_server_schedule_render(server);
	}
}

/* **A CLIENT THAT IS NOT READING MUST NOT BE GIVEN MORE EVENTS.** libwayland keeps a 4 KiB outgoing buffer per
 * connection and, when the socket cannot take it, the next event makes it fail with "Data too big for buffer" and the
 * CLIENT IS DROPPED. That is what a wheel spun while a client is busy drawing produced: a burst of pointer motion and
 * axis events filled the buffer, the client vanished mid-scroll, and a debugger (which changed the timing) did not
 * reproduce it. `-flush` moves whatever is already buffered to the socket; `-poll` then says whether the socket can
 * take more. A pointer event is COALESCABLE — the next motion carries the latest position, the next notch carries the
 * distance — so skipping it is the right answer, and the client stays connected. */
static int fn_resource_can_take_event(struct wl_resource *resource)
{
	struct wl_client *client;
	struct pollfd pfd;
	int fd;

	if (resource == NULL) {
		return 0;
	}
	client = wl_resource_get_client(resource);
	if (client == NULL) {
		return 0;
	}
	fd = wl_client_get_fd(client);
	if (fd < 0) {
		return 0;
	}
	pfd.fd = fd;
	pfd.events = POLLOUT;
	pfd.revents = 0;
	return poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLOUT) != 0;
}

void aws_seat_pointer_motion(struct aws_server *server, int x, int y)
{
	struct aws_surface *surface;
	struct aws_pointer *pointer;
	wl_fixed_t sx, sy;

	server->pointer_x = x;
	server->pointer_y = y;
	/* **A CURSOR THE COMPOSITOR DRAWS ONLY MOVES WHEN A FRAME COMPOSITES.** On a backend with a host pointer the
	 * person's own cursor moves with the device and no frame is needed; on DRM the arrow is pixels, and a motion with
	 * no redraw is a pointer that appears stuck until a client happens to repaint. Scheduled BEFORE the focus test
	 * below, because motion over an empty desktop has no surface to tell and is exactly the case that needed this. */
	if (server->backend.owns_cursor) {
		aws_server_schedule_render(server);
	}
	if (!server->pointer_inside) {
		return;
	}
	/* **AND WHERE IT GOES IS `aws_seat_pointer_target`'s ANSWER, NOT THIS FUNCTION'S.** A capture takes it wherever
	 * the pointer is; otherwise the surface under the pointer; otherwise, over the desktop, the menu's fallback. */
	surface = aws_seat_pointer_target(server, aws_surface_at(server, x, y));
	pointer_set_focus(server, surface);
	if (surface == NULL) {
		return;
	}
	/* **AND WHERE THE POINTER REALLY IS, FOR A CLIENT-SIDE DRAG.** The relative `wl_pointer.motion` below is
	 * relative to THIS surface; the absolute one is what lets a window being moved follow the pointer without
	 * chasing its own tail. Sent FIRST, so a client that pairs them sees the absolute before the relative. */
	if (fn_resource_can_take_event(surface->resource)) {
		aws_window_send_pointer_global(surface, x, y);
	}
	sx = wl_fixed_from_int(x - surface->x);
	sy = wl_fixed_from_int(y - surface->y);
	wl_list_for_each(pointer, &server->pointers, link) {
		if (same_client(pointer->resource, surface->resource) &&
		    fn_resource_can_take_event(pointer->resource)) {
			wl_pointer_send_motion(pointer->resource, seat_now_ms(), sx, sy);
		}
	}
}

void aws_seat_pointer_button(struct aws_server *server, uint32_t evdev_button, int pressed)
{
	struct aws_surface *surface;
	struct aws_pointer *pointer;
	uint32_t serial = wl_display_next_serial(server->display);

	/* **A CLICK GOES TO THE SURFACE UNDER THE POINTER, AND THAT IS ESTABLISHED HERE RATHER THAN ASSUMED.** Focus
	 * is normally set by motion and a click is not a motion, so a surface that appeared under a stationary
	 * pointer would otherwise have this press sent to whatever held the focus, or to nobody. `-recheck` is
	 * idempotent when the focus is already right, and when it is not it sends the `enter` the client is owed
	 * BEFORE the button. */
	surface = aws_surface_at(server, server->pointer_x, server->pointer_y);
	/* **WHICH SURFACE A PRESS ACTUALLY WENT TO, ON THE COMPOSITOR'S SIDE.** A client that reports "I got no
	 * click" is reporting the end of a chain whose first link is this lookup, and the two facts that decide it —
	 * the level and the frame of what was found — are only visible here. */
	if (getenv("AWS_INPUT_DIAG") != NULL) {
		if (surface != NULL) {
			struct wl_shm_buffer *shm = (surface->current_buffer != NULL)
				? wl_shm_buffer_get(surface->current_buffer) : NULL;

			fprintf(stderr,
				"  DIAG press (%d,%d) -> surface level %d at (%d,%d) %dx%d visible %d\n",
				server->pointer_x, server->pointer_y, surface->level, surface->x, surface->y,
				shm != NULL ? wl_shm_buffer_get_width(shm) : -1,
				shm != NULL ? wl_shm_buffer_get_height(shm) : -1, surface->visible);
		} else {
			fprintf(stderr, "  DIAG press (%d,%d) -> NO SURFACE\n", server->pointer_x,
				server->pointer_y);
		}
	}
	/* AND THE SAME ROUTING A MOTION USES — see `aws_seat_pointer_target`. A press on the desktop belongs to the
	 * popup that is tracking, which is how a click outside a menu dismisses it; a press anywhere at all belongs to
	 * a window that is holding the pointer for a drag, which is how a drag hears its own release. */
	surface = aws_seat_pointer_target(server, surface);
	pointer_set_focus(server, surface);
	/* CLICK-TO-FOCUS, AND IT HAPPENS ON THE WAY DOWN: the window being clicked is the window the keyboard
	 * should be talking to, and a client that heard the button before the focus would be drawing a focused
	 * control in an unfocused window.
	 *
	 * **AND A CLICK OVER NO WINDOW AT ALL DOES NOT CLEAR IT.** A press on the DESKTOP has no surface to give the
	 * keyboard to, and taking it away would leave the desktop with NO key application — no menu bar, no key
	 * equivalents, nowhere for a keystroke to go. The Mac's answer is that the click activates whoever IS in front
	 * (its file manager, which this tier has not got); the honest version of it here is to leave the focus exactly where
	 * it was. */
	if (pressed && surface != NULL) {
		aws_seat_set_keyboard_focus(server, surface);
	}
	if (surface == NULL) {
		return;
	}
	/* **AND THE SAME ABSOLUTE POSITION WITH A BUTTON, BECAUSE A DRAG'S GRAB POINT IS A PRESS.** The client needs to
	 * know where the pointer was when the button went down, not only where it is on the way. */
	aws_window_send_pointer_global(surface, server->pointer_x, server->pointer_y);
	wl_list_for_each(pointer, &server->pointers, link) {
		if (same_client(pointer->resource, surface->resource)) {
			wl_pointer_send_button(pointer->resource, serial, seat_now_ms(), evdev_button,
					       pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
		}
	}
	/* **AND THE CAPTURE IS GIVEN BACK ON THE LAST RELEASE, AFTER THE RELEASE HAS BEEN DELIVERED.** A capture is
	 * only ever meaningful mid-press, so the server does not rely on the client to let go of it: a client that
	 * lost the release — the very thing a capture exists to prevent — would otherwise hold the desktop's pointer
	 * for good. The order matters: the release goes to the CAPTURED surface first, or the drag would end without
	 * hearing that it had. */
	if (pressed) {
		server->pointer_buttons |= 1u << (evdev_button & 31u);
	} else {
		server->pointer_buttons &= ~(1u << (evdev_button & 31u));
		if (server->pointer_buttons == 0 && server->pointer_capture != NULL) {
			/* BOTH COPIES, so the surface's own flag does not say it still holds a pointer it has lost. */
			server->pointer_capture->pointer_capture = 0;
			server->pointer_capture = NULL;
			aws_seat_pointer_recheck(server);
		}
	}
}

/* **HOW LONG AN AXIS HAS TO BE QUIET BEFORE ITS SCROLL IS OVER.** The protocol names the event and not the number:
 * 150 ms is the number libweston's own scroll handling settled on, and it is long enough that a finger's pauses
 * between motions do not read as an ending and short enough that a kinetic animation waiting on it does not stall. */
#define AWS_AXIS_STOP_MS 150

/* **ONE AXIS OF ONE FRAME, ROUTED LIKE A MOTION AND DELIVERED LIKE A BUTTON.** Three things decide what a client
 * sees here, and each has its own reason for being asked in this order:
 *
 *   WHERE IT GOES is `aws_seat_pointer_target`'s answer, the same one motion and buttons use — a capture takes it
 *   wherever the pointer is, a surface under the pointer otherwise, a tracking menu's fallback over the desktop.
 *   The focus is re-established first, because a window that appeared under a stationary pointer would otherwise
 *   scroll its neighbour; the recheck sends the `enter` the client is owed BEFORE the axis, which is the order a
 *   client needs to accept the scroll at all.
 *
 *   WHAT IT IS is split by version, and the split is the whole of "no double counting": a wheel's distance can be
 *   said precisely (`axis`) and in steps (`axis_discrete` from v5, `axis_value120` from v8 — never both), and a
 *   client that bound below v5 hears only the precise value. `axis_source` is a v5 event and belongs to the frame
 *   the axis events are in, so it is sent once per frame.
 *
 *   AND THE FRAME IS THE CALLER'S TO CLOSE. libinput delivers one event carrying both axes of a trackpad gesture, so
 *   the backend calls this once per axis and then `aws_seat_pointer_frame` once — which is what makes a client's
 *   single scroll handler run once per gesture instead of once per axis.
 */
void aws_seat_pointer_axis(struct aws_server *server, uint32_t axis, wl_fixed_t value, uint32_t source,
			   int32_t discrete, int32_t value120)
{
	struct aws_surface *surface;
	struct aws_pointer *pointer;
	uint32_t time = seat_now_ms();

	surface = aws_seat_pointer_target(server, aws_surface_at(server, server->pointer_x, server->pointer_y));
	pointer_set_focus(server, surface);
	if (surface == NULL) {
		return;
	}
	wl_list_for_each(pointer, &server->pointers, link) {
		uint32_t version;

		if (!same_client(pointer->resource, surface->resource) ||
		    !fn_resource_can_take_event(pointer->resource)) {
			continue;
		}
		version = wl_resource_get_version(pointer->resource);
		if (version >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION && !server->pointer_axis_frame_open) {
			wl_pointer_send_axis_source(pointer->resource, source);
		}
		if (version >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION) {
			/* **THE 120THS ARE THE WHEEL'S OWN UNIT AND THE PRECISE VALUE IS THE DISTANCE.** A v8 client that was
			 * also sent `axis_discrete` would be told the same motion twice, which is the double count the
			 * protocol's own note warns about — so the older event is not sent at all here. */
			if (value120 != 0) {
				wl_pointer_send_axis_value120(pointer->resource, axis, value120);
			}
		} else if (version >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION) {
			if (discrete != 0) {
				wl_pointer_send_axis_discrete(pointer->resource, axis, discrete);
			}
		}
		wl_pointer_send_axis(pointer->resource, time, axis, value);
	}
	server->pointer_axis_frame_open = 1;
	/* **AND THE SEQUENCE RE-ARMS ITS OWN ENDING.** A stop is owed only for the axes a sequence actually moved, and
	 * only for the sources that have a sequence at all: a wheel's notch is complete when it arrives, so arming a
	 * timer for one would send a stop after every click of the wheel — a gesture end that never happened. */
	if (source == WL_POINTER_AXIS_SOURCE_FINGER || source == WL_POINTER_AXIS_SOURCE_CONTINUOUS) {
		server->axis_used |= 1u << (axis & 31u);
		if (server->axis_stop_timer != NULL) {
			wl_event_source_timer_update(server->axis_stop_timer, AWS_AXIS_STOP_MS);
		}
	}
}

void aws_seat_pointer_frame(struct aws_server *server)
{
	struct aws_pointer *pointer;

	/* NOTHING WAS SENT, SO THERE IS NO FRAME TO CLOSE — and a `frame` with no events in it is a client's scroll
	 * handler running on nothing, once per motion, which is exactly the kind of quiet wrongness a frame exists to
	 * avoid. */
	if (!server->pointer_axis_frame_open) {
		return;
	}
	wl_list_for_each(pointer, &server->pointers, link) {
		if (wl_resource_get_version(pointer->resource) >= WL_POINTER_FRAME_SINCE_VERSION &&
		    fn_resource_can_take_event(pointer->resource)) {
			wl_pointer_send_frame(pointer->resource);
		}
	}
	server->pointer_axis_frame_open = 0;
}

/* **AN ENDING HAS TO BE MANUFACTURED, BECAUSE NOTHING ANNOUNCES IT.** `axis_stop` tells a client that a scroll
 * sequence is over — it is what a kinetic animation on a trackpad waits for, and without it a client that scrolls
 * "with momentum" would run forever. Finger and continuous sources are the ones with a sequence: they emit values for
 * as long as the fingers move, and then simply stop. A WHEEL has no such thing, because a notch is discrete and its
 * end is the next notch not arriving.
 *
 * THE TIMEOUT IS THIS COMPOSITOR'S NUMBER. The protocol does not define one; what it does say is that the stop
 * belongs to the axes that were moving, so the set is remembered per sequence rather than assumed to be both. */
void aws_seat_pointer_axis_stop(struct aws_server *server)
{
	struct aws_pointer *pointer;
	uint32_t time = seat_now_ms();
	uint32_t used = server->axis_used;

	server->axis_used = 0;
	if (used == 0) {
		return;		/* nothing was moving, so no sequence ended */
	}
	wl_list_for_each(pointer, &server->pointers, link) {
		uint32_t version = wl_resource_get_version(pointer->resource);

		if (version < WL_POINTER_AXIS_STOP_SINCE_VERSION) {
			continue;	/* a client that never heard `axis` in a frame has nothing to end */
		}
		if (used & (1u << WL_POINTER_AXIS_VERTICAL_SCROLL)) {
			wl_pointer_send_axis_stop(pointer->resource, time, WL_POINTER_AXIS_VERTICAL_SCROLL);
		}
		if (used & (1u << WL_POINTER_AXIS_HORIZONTAL_SCROLL)) {
			wl_pointer_send_axis_stop(pointer->resource, time, WL_POINTER_AXIS_HORIZONTAL_SCROLL);
		}
		if (version >= WL_POINTER_FRAME_SINCE_VERSION) {
			/* AND IT GOES IN ITS OWN FRAME, so a client that closes its gesture on `frame` sees the stop as one
			 * delivery rather than as a stray event inside somebody else's. */
			wl_pointer_send_frame(pointer->resource);
		}
	}
}

/* THE TIMER'S BODY. Returning zero leaves the source armed-but-not-repeating: `wl_event_source_timer_update` is what
 * arms it again, and the next axis event does that. */
static int axis_stop_fired(void *data)
{
	struct aws_server *server = data;

	aws_seat_pointer_axis_stop(server);
	return 0;
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
	/* **THE SEAT'S ONE TIMER, AND IT IS CREATED HERE BECAUSE THE LOOP IS THE SERVER'S.** It is removed in
	 * `aws_seat_finish`, which runs while the loop is still alive — the same ordering `aws_server_destroy` had to
	 * learn for the backend and input sources. */
	server->axis_stop_timer = wl_event_loop_add_timer(server->loop, axis_stop_fired, server);
	/* **THE SEAT IS ADVERTISED AT 8, WHICH IS WHERE A POINTER LEARNS `axis_value120`.** The seat's own events stop
	 * at version 2 (`name`); every number above it exists to let a POINTER or a KEYBOARD be bound higher, and a
	 * child object inherits its parent's bound version — so a seat capped at 5 caps every pointer at 5, and the
	 * v8 spelling of a wheel's steps can never be reached no matter how carefully it is implemented. Measured: a
	 * client asking for 8 was answered with 5, which made the whole `axis_value120` path dead code until this
	 * number moved.
	 *
	 * 8 AND NOT THE LIBRARY'S MAXIMUM: version 9 adds `axis_relative_direction`, which this server does not send
	 * (a natural-scroll flag the compositor would have to know from the device), and advertising a version whose
	 * new events never arrive is a promise a client may wait on. The clamp keeps a build against an older
	 * libwayland honest as well. */
	wl_global_create(server->display, &wl_seat_interface,
			 wl_seat_interface.version < 8u ? wl_seat_interface.version : 8u, server, bind_seat);
}

void aws_seat_finish(struct aws_server *server)
{
	/* **AND THE TIMER GOES BEFORE THE LOOP DOES.** `wl_display_destroy` destroys the event loop, and a source
	 * removed afterwards is the invalid list operation `aws_server_destroy` used to perform. */
	if (server->axis_stop_timer != NULL) {
		wl_event_source_remove(server->axis_stop_timer);
		server->axis_stop_timer = NULL;
	}
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
