/*
 * seat_routing — where does a pointer event go, and does a drag still get it when the hand leaves the window?
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
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
 * client has nothing to write to. `aws_server_schedule_render` only sets a flag on a server this  *
 * file does not have — the seat asks for a frame whenever the COMPOSITOR draws the pointer, and a *
 * compositor-owned cursor is a DRM fact rather than a seat rule. Everything else in `seat.c` is    *
 * linked as it ships.                                                                            *
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

/* **AND THE FOURTH: A FRAME REQUEST.** The seat schedules a redraw when the pointer moves on a backend that draws
 * its own cursor (`backend.owns_cursor`), so the symbol is asked for even though a test server has no loop to draw in.
 * A counter here would be the place to assert that request if the seat's cursor rule ever becomes a claim. */
void aws_server_schedule_render(struct aws_server *server)
{
	(void)server;
}

/* **AND THE OTHER HALF OF THE POINTER'S FRAME: a render that may be partial, and the pointer's own box.** The seat is
 * the caller here: a motion adds where the pointer was and where it is to the server's damage ledger and asks for a
 * partial render. The real `aws_cursor_device_box` is `cursor.c`'s arithmetic (device placement and hotspot), which the
 * cursor suite checks on its own; this stub is a 16x16 box at the pointer, so what is measured here is the SEAT's
 * decision about which boxes a motion touches. */
static int s_partial_renders;

void aws_server_schedule_render_partial(struct aws_server *server)
{
	(void)server;
	s_partial_renders++;
}

int aws_cursor_device_box(struct aws_server *server, int x, int y, struct aws_box *box)
{
	if (!server->backend.owns_cursor) {
		return 0;
	}
	box->x = x;
	box->y = y;
	box->width = 16;
	box->height = 16;
	return 1;
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

	/* 10. **A MOVING POINTER IS A POINTER-SIZED FRAME, NOT A SCREEN-SIZED ONE.** Both boxes — where it was and where it
	 * is — are damaged, the frame is NOT marked full, and the render asked for is the partial one. The old box is the
	 * trap this exists for: those pixels are not a rectangle any client damaged. */
	reset(&server);
	server.display = display;
	server.backend.owns_cursor = 1;
	s_under_pointer = NULL;
	s_partial_renders = 0;
	aws_seat_pointer_motion(&server, 140, 120);
	check(server.damage.x == 100 && server.damage.y == 100 && server.damage.width == 56 &&
		      server.damage.height == 36,
	      "a motion damages BOTH the pointer's old box and its new one, as one union");
	check(server.damage_full == 0, "and it is NOT a full redraw");
	check(s_partial_renders == 1, "and it asks for a PARTIAL render, which is the only kind that can be clipped");

	/* 11. **AND LEAVING THE OUTPUT REPAINTS WHERE THE POINTER WAS, WITH NO NEW BOX.** The pointer is gone, so only the
	 * old rectangle is damaged — the frame after it leaves cannot be a no-op. */
	reset(&server);
	server.display = display;
	server.backend.owns_cursor = 1;
	s_under_pointer = NULL;
	s_partial_renders = 0;
	aws_seat_pointer_leave(&server);
	check(server.damage.x == 100 && server.damage.y == 100 && server.damage.width == 16 &&
		      server.damage.height == 16,
	      "leaving the output damages exactly where the pointer was");
	check(s_partial_renders == 1 && server.damage_full == 0, "as one partial frame, not a screen");

	/* 12. **A BACKEND WITH A HOST POINTER DOES NOTHING AT ALL.** The host draws that arrow and no pixel of this
	 * compositor's contains it, so a motion here must not damage anything. */
	reset(&server);
	server.display = display;
	server.backend.owns_cursor = 0;
	s_under_pointer = NULL;
	s_partial_renders = 0;
	aws_seat_pointer_motion(&server, 140, 120);
	check(server.damage.width == 0 && server.damage.height == 0 && s_partial_renders == 0,
	      "and a host-drawn pointer damages nothing, because none of its pixels are ours");

	wl_display_destroy(display);
	printf("seat_routing: %d case(s) failed\n", s_failed);
	return s_failed == 0 ? 0 : 1;
}
