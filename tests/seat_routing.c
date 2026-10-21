/*
 * seat_routing — where does a pointer event go, and does a drag still get it when the hand leaves the window?
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed. SPDX-License-Identifier: MIT
 *
 * **THE BUG THIS FILE EXISTS FOR WAS REPORTED AS "it loses track of the pointer when the cursor leaves the
 * window".** Resizing drags a window's edge, and the edge follows the hand — but the compositor's copy of the
 * surface is a frame behind, so the pointer really does end up over the desktop or over the window BEHIND the one
 * being resized. There were two grabs in the protocol and only one of them could help: `set_pointer_grab` is a
 * FALLBACK that gives way to any surface under the pointer (it has to be, or a menu bar could not be crossed back
 * to), so a pointer that landed on a neighbour starved the drag. `set_pointer_capture` is the real grab a drag
 * needs, and this file pins the precedence.
 *
 * IT IS A UNIT TEST AND NOT A SCREENSHOT, because the claim is one function's answer and nothing about pixels. The
 * compositor's own seat is compiled in as it ships — `../src/seat.c` — and the two questions it asks of the rest of
 * the server ("what is under this point", "tell this window where the pointer is") are the only things stubbed.
 * Everything else is the real code path: the real `pointer_set_focus`, the real button bookkeeping, the real
 * recheck. A Wayland display is created but no client ever connects to it — `wl_display_next_serial` is the one
 * thing a button event needs from it.
 */
#include "server.h"

#include <stdio.h>
#include <string.h>

#include <wayland-server.h>

/* ============================================================================================ *
 * THE TWO QUESTIONS THIS FILE STUBS, AND WHY THOSE TWO.                                        *
 *                                                                                                *
 * `aws_surface_at` is the compositor's hit test over its own stacking order — a whole surface   *
 * model away from the rule under test, and its answer is an INPUT here rather than a fact to be  *
 * discovered. `aws_window_send_pointer_global` writes to a client's socket, and a test with no  *
 * client has nothing to write to. Everything else in `seat.c` is linked as it ships.             *
 * ============================================================================================ */
static struct aws_surface *s_under_pointer;	/* what the stubbed hit test answers */
static int s_global_sends;			/* how many absolute positions were sent */

struct aws_surface *aws_surface_at(struct aws_server *server, int x, int y)
{
	(void)server;
	(void)x;
	(void)y;
	return s_under_pointer;
}

void aws_window_send_pointer_global(struct aws_surface *surface, int x, int y)
{
	(void)surface;
	(void)x;
	(void)y;
	s_global_sends++;
}

/* **AND THE THIRD, WHICH ARRIVED WITH THE WINDOW LIST.** A focus change tells a Dock what is in front, and a test
 * with no client to tell has nothing to do with it — but the call exists in `seat.c` as it ships, so the symbol has
 * to be here or the suite would not link. If the list ever grows a claim worth testing, this is where a counter
 * would go. */
void aws_shell_window_list_changed(struct aws_server *server)
{
	(void)server;
}

static int s_failed;

static void check(int ok, const char *what)
{
	printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		s_failed++;
	}
}

/* A SURFACE THE RULE CAN NAME. `aws_seat_pointer_target` compares pointers and reads nothing else, so a zeroed
 * struct is a surface as far as this file is concerned — which is the point: the rule is about identity, not about
 * geometry or buffers. */
static struct aws_surface s_a, s_b, s_grab;

static void reset(struct aws_server *server)
{
	memset(server, 0, sizeof *server);
	wl_list_init(&server->pointers);
	wl_list_init(&server->keyboards);
	wl_list_init(&server->surfaces);
	server->pointer_inside = 1;
	server->pointer_x = 100;
	server->pointer_y = 100;
	s_under_pointer = NULL;
}

int main(void)
{
	struct aws_server server;
	struct wl_display *display = wl_display_create();

	if (display == NULL) {
		printf("FAIL: could not create a Wayland display, and a button event needs one for its serial\n");
		return 1;
	}
	memset(&s_a, 0, sizeof s_a);
	memset(&s_b, 0, sizeof s_b);
	memset(&s_grab, 0, sizeof s_grab);

	/* 1. **THE ORDINARY CASE: THE SURFACE UNDER THE POINTER GETS IT.** Nothing is held, so this is the answer every
	 * compositor gives and the one everything else is a departure from. */
	reset(&server);
	server.display = display;
	s_under_pointer = &s_b;
	aws_seat_pointer_motion(&server, 100, 100);
	check(server.pointer_focus == &s_b, "with nothing held, the surface UNDER THE POINTER gets the motion");

	/* 2. **THE FALLBACK GRAB IS A FALLBACK, AND THAT IS THE MENU'S CONTRACT.** A menu bar is a separate window from
	 * the menu it opened and the pointer has to be able to cross back to it, so a surface under the pointer wins. */
	reset(&server);
	server.display = display;
	server.pointer_grab = &s_grab;
	s_under_pointer = &s_b;
	aws_seat_pointer_motion(&server, 100, 100);
	check(server.pointer_focus == &s_b, "and a FALLBACK grab gives way to it — the menu bar is why");

	/* 3. **BUT A FALLBACK GRAB DOES TAKE THE EVENTS THAT WOULD GO NOWHERE** — a click on the desktop, which is how
	 * "click anywhere to dismiss" is said. */
	reset(&server);
	server.display = display;
	server.pointer_grab = &s_grab;
	s_under_pointer = NULL;
	aws_seat_pointer_motion(&server, 100, 100);
	check(server.pointer_focus == &s_grab, "while over NO surface it takes them — a menu's click-outside");

	/* 4. **A CAPTURE TAKES THEM WHEREVER THEY ARE, WHICH IS THE DRAG'S ENTIRE REASON FOR EXISTING.** This is the
	 * reported bug: the hand is over a NEIGHBOUR, and the window being resized still has it. */
	reset(&server);
	server.display = display;
	server.pointer_capture = &s_a;
	s_under_pointer = &s_b;
	aws_seat_pointer_motion(&server, 100, 100);
	check(server.pointer_focus == &s_a, "a CAPTURE takes the motion even when a NEIGHBOUR is under the pointer");

	/* 5. **AND OVER THE DESKTOP TOO**, where the fallback would have caught it anyway — the capture must not depend
	 * on that accident. */
	reset(&server);
	server.display = display;
	server.pointer_capture = &s_a;
	s_under_pointer = NULL;
	aws_seat_pointer_motion(&server, 100, 100);
	check(server.pointer_focus == &s_a, "and over no surface at all");

	/* 6. **THE RECHECK ASKS THE SAME QUESTION.** It runs whenever the set of surfaces or their geometry changes —
	 * which a resize does on every frame — so a recheck that re-decided focus from the pointer alone would drop the
	 * capture in the middle of the drag it was taken for. */
	reset(&server);
	server.display = display;
	server.pointer_capture = &s_a;
	s_under_pointer = &s_b;
	aws_seat_pointer_recheck(&server);
	check(server.pointer_focus == &s_a, "and a RECHECK — which a resize triggers on every frame — agrees");

	/* 7. **THE RELEASE REACHES THE CAPTURING WINDOW, AND THE CAPTURE ENDS WITH IT.** A press is what starts a drag;
	 * the release must go to the same window even over a neighbour, and the compositor then takes the pointer back
	 * so a lost release cannot hold the desktop for good. */
	reset(&server);
	server.display = display;
	server.pointer_capture = &s_a;
	s_under_pointer = &s_b;
	aws_seat_pointer_button(&server, 0x110, 1);
	check(server.pointer_focus == &s_a && server.pointer_buttons != 0,
	      "a PRESS goes to the capturing window and is remembered as down");
	aws_seat_pointer_button(&server, 0x110, 0);
	check(server.pointer_capture == NULL, "and the RELEASE gives the pointer back — the server does not trust the client to");

	/* 8. **AND THE FALLBACK IS UNTOUCHED BY ANY OF IT.** A menu's grab survives a press and a release with no capture
	 * in sight, which is what keeps the two doors from becoming one. */
	reset(&server);
	server.display = display;
	server.pointer_grab = &s_grab;
	s_under_pointer = NULL;
	aws_seat_pointer_button(&server, 0x110, 1);
	check(server.pointer_grab == &s_grab, "a press with only the FALLBACK held leaves that fallback alone");
	aws_seat_pointer_button(&server, 0x110, 0);
	check(server.pointer_grab == &s_grab, "and its release does not give it away");

	/* 9. **A DESTROYED WINDOW GIVES EVERYTHING BACK.** The window being dragged can be destroyed mid-drag; a capture
	 * that outlived it would leave the desktop with no pointer at all. */
	reset(&server);
	server.display = display;
	server.pointer_capture = &s_a;
	server.pointer_focus = &s_a;
	aws_seat_surface_gone(&server, &s_a);
	check(server.pointer_capture == NULL && server.pointer_focus == NULL,
	      "and a window DESTROYED mid-drag takes its capture with it");

	wl_display_destroy(display);
	printf("seat_routing: %d case(s) failed\n", s_failed);
	return s_failed == 0 ? 0 : 1;
}
