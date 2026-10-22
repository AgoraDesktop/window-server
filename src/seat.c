/*
 * seat.c — the seat: wl_pointer, wl_keyboard, focus, and the keymap.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
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
	aws_window_send_pointer_global(surface, x, y);
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
