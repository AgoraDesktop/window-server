/*
 * pointer_axis — what a scroll actually puts on the wire, at every client version that can receive it.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **A SCROLL HAS THREE VERSIONS OF ITSELF AND ONLY ONE OF THEM MAY BE SENT.** `wl_pointer.axis` is version 1 and
 * carries the precise distance. From version 5 there is also `axis_source` ("a wheel is not a trackpad") and a
 * DISCRETE count of wheel steps — and from version 8 those steps are spelled `axis_value120` instead, in
 * hundred-and-twentieths, because a high-resolution wheel does not move in whole clicks. Sending both discrete
 * spellings to one client is the double count the report names; sending a v8 event to a v4 client is a protocol
 * violation, and sending `axis_source` once per axis rather than once per frame is a lie about how many gestures
 * happened.
 *
 * **SO THE TEST IS A VERSION SWEEP, NOT A SINGLE GESTURE.** The seat is compiled in as it ships and the five
 * `wl_pointer_send_*` doors are interposed, so what is asserted is the exact sequence a client would hear at each
 * bound version — which is the only place the version guards can be observed. The resources are REAL `wl_resource`s
 * over a real `wl_client`, because `wl_resource_get_version` is what the guards read; nothing is written to the
 * socket, because the sends themselves are the recorders.
 *
 * **AND THE FRAME IS THE OTHER HALF OF THE CONTRACT.** `wl_pointer.frame` is what makes a client's scroll handler
 * run once per gesture: two axes in one gesture must be two axis events inside ONE frame, and a frame with nothing
 * in it must not be sent at all — a client that treats `frame` as "something arrived" would otherwise run its
 * scroll handler on every mouse motion.
 */
#include "server.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ============================================================================================ *
 * THE THREE QUESTIONS `seat.c` ASKS OF THE REST OF THE SERVER, AND WHY THEY ARE HERE.            *
 *                                                                                                *
 * `aws_surface_at` is the hit test over the stacking order — an input to this test rather than a  *
 * fact to discover. `aws_window_send_pointer_global` and `aws_shell_window_list_changed` are     *
 * calls the seat makes on paths this test does not drive, but the symbols have to exist for    *
 * `seat.c` to link. `aws_server_schedule_render` is the frame the seat asks for when the        *
 * COMPOSITOR draws the pointer (a DRM fact, `backend.owns_cursor`), and it is the same kind of   *
 * call: sets a flag on a server this file does not own.                                         *
 * ============================================================================================ */
static struct aws_surface *s_under_pointer;

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
}

void aws_shell_window_list_changed(struct aws_server *server)
{
	(void)server;
}

void aws_server_schedule_render(struct aws_server *server)
{
	(void)server;
}

/* **AND THE TWO THE SEAT ASKS WHEN IT DRAWS ITS OWN POINTER.** A motion damages the pointer's old and new boxes and asks
 * for a PARTIAL render (`aws_cursor_device_box`, `aws_server_schedule_render_partial`); this file is about axis events, so
 * neither is interesting here and both are stubbed. A test server has no cursor pixels to place. */
void aws_server_schedule_render_partial(struct aws_server *server)
{
	(void)server;
}

int aws_cursor_device_box(struct aws_server *server, int x, int y, struct aws_box *box)
{
	(void)server;
	(void)x;
	(void)y;
	(void)box;
	return 0;
}

/* ============================================================================================ *
 * THE WIRE, INTERPOSED AT THE ONE FUNCTION EVERY EVENT GOES THROUGH.                           *
 *                                                                                                *
 * The generated `wl_pointer_send_*` helpers are `static inline` wrappers around                *
 * `wl_resource_post_event`, so they cannot be replaced — but that one can, and it is the same  *
 * question asked one level lower: which opcode, with which arguments, on which resource.        *
 * seat.c is compiled into this executable, so its calls bind to these definitions and nothing   *
 * is ever written to the socket.                                                               *
 * ============================================================================================ */
enum call {
	CALL_AXIS,
	CALL_AXIS_SOURCE,
	CALL_AXIS_DISCRETE,
	CALL_AXIS_VALUE120,
	CALL_FRAME,
	CALL_AXIS_STOP,
	CALL_OTHER,
};

static struct {
	enum call kind;
	uint32_t axis;
	int32_t n;		/* discrete, value120, or the source */
	wl_fixed_t value;
} s_calls[64];
static int s_call_count;

static void record(enum call kind, uint32_t axis, int32_t n, wl_fixed_t value)
{
	if (s_call_count < (int)(sizeof s_calls / sizeof s_calls[0])) {
		s_calls[s_call_count].kind = kind;
		s_calls[s_call_count].axis = axis;
		s_calls[s_call_count].n = n;
		s_calls[s_call_count].value = value;
	}
	s_call_count++;
}

/* **THE OPCODES ARE THE PROTOCOL'S OWN NUMBERS**, in the order `wl_pointer` declares its events. They are written
 * down rather than reached through the generated struct because what is being checked is exactly "which opcode went
 * out", and a macro that resolved to itself would make the check circular. */
#define OP_AXIS			4u
#define OP_FRAME		5u
#define OP_AXIS_SOURCE		6u
#define OP_AXIS_STOP		7u
#define OP_AXIS_DISCRETE	8u
#define OP_AXIS_VALUE120	9u

void wl_resource_post_event(struct wl_resource *resource, uint32_t opcode, ...)
{
	va_list ap;

	/* **EVERY POSTED EVENT IN THIS TEST IS A POINTER EVENT**, so the opcode alone identifies it. The interface
	 * cannot be asked for: `wl_resource`'s own interface field is private to libwayland, and the public door that
	 * could answer (`wl_resource_instance_of`) compares the IMPLEMENTATION, which `seat.c` keeps private. What
	 * keeps this honest is the fixture — the only calls made below are the seat's axis ones. */
	(void)resource;
	va_start(ap, opcode);
	switch (opcode) {
	case OP_AXIS: {
		/* (time, axis, value) */
		uint32_t time = va_arg(ap, uint32_t);
		uint32_t axis = va_arg(ap, uint32_t);
		wl_fixed_t value = (wl_fixed_t)va_arg(ap, int32_t);

		(void)time;
		record(CALL_AXIS, axis, 0, value);
		break;
	}
	case OP_AXIS_SOURCE:
		record(CALL_AXIS_SOURCE, 0, (int32_t)va_arg(ap, uint32_t), 0);
		break;
	case OP_AXIS_DISCRETE: {
		uint32_t axis = va_arg(ap, uint32_t);
		int32_t discrete = va_arg(ap, int32_t);

		record(CALL_AXIS_DISCRETE, axis, discrete, 0);
		break;
	}
	case OP_AXIS_VALUE120: {
		uint32_t axis = va_arg(ap, uint32_t);
		int32_t value120 = va_arg(ap, int32_t);

		record(CALL_AXIS_VALUE120, axis, value120, 0);
		break;
	}
	case OP_AXIS_STOP: {
		uint32_t time = va_arg(ap, uint32_t);
		uint32_t axis = va_arg(ap, uint32_t);

		(void)time;
		record(CALL_AXIS_STOP, axis, 0, 0);
		break;
	}
	case OP_FRAME:
		record(CALL_FRAME, 0, 0, 0);
		break;
	default:
		record(CALL_OTHER, opcode, 0, 0);
		break;
	}
	va_end(ap);
}

static const char *call_name(enum call kind)
{
	switch (kind) {
	case CALL_AXIS: return "axis";
	case CALL_AXIS_SOURCE: return "axis_source";
	case CALL_AXIS_DISCRETE: return "axis_discrete";
	case CALL_AXIS_VALUE120: return "axis_value120";
	case CALL_FRAME: return "frame";
	case CALL_AXIS_STOP: return "axis_stop";
	case CALL_OTHER: return "other";
	}
	return "?";
}

static int s_failed;

static void check(int ok, const char *what)
{
	printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		s_failed++;
	}
}

/* EVERY CALL RECORDED, one line each, so a failure says what was sent rather than only how many. */
static void dump(void)
{
	int i;

	for (i = 0; i < s_call_count && i < (int)(sizeof s_calls / sizeof s_calls[0]); i++) {
		printf("        %s axis=%u n=%d value=%g\n", call_name(s_calls[i].kind), s_calls[i].axis,
		       s_calls[i].n, wl_fixed_to_double(s_calls[i].value));
	}
}

static int count_of(enum call kind)
{
	int i, n = 0;

	for (i = 0; i < s_call_count; i++) {
		if (s_calls[i].kind == kind) {
			n++;
		}
	}
	return n;
}

/* ============================================================================================ *
 * THE FIXTURE: A REAL CLIENT, A REAL POINTER RESOURCE AT A CHOSEN VERSION.                     *
 * ============================================================================================ */
static struct wl_display *s_display;
static struct wl_client *s_client;
static struct aws_surface s_surface;
static struct aws_pointer s_pointer;

/* ONE BOUND VERSION, STARTING FROM NOTHING. The resource is created at `version` because that is the number the
 * guards read; the client is the one the socketpair made, so `same_client` answers yes for pointer and surface. */
static void bind_at(struct aws_server *server, int version)
{
	memset(server, 0, sizeof *server);
	wl_list_init(&server->pointers);
	wl_list_init(&server->keyboards);
	wl_list_init(&server->surfaces);
	server->display = s_display;
	server->pointer_inside = 1;
	server->pointer_x = 10;
	server->pointer_y = 10;

	memset(&s_surface, 0, sizeof s_surface);
	s_surface.resource = wl_resource_create(s_client, &wl_surface_interface, 4, 2);

	s_pointer.resource = wl_resource_create(s_client, &wl_pointer_interface, version, 3);
	wl_list_insert(server->pointers.prev, &s_pointer.link);

	/* THE FOCUS IS ALREADY THESE SURFACES, so `pointer_set_focus` is a no-op and the test measures the AXIS path
	 * rather than enter/leave traffic. */
	server->pointer_focus = &s_surface;
	s_under_pointer = &s_surface;

	s_call_count = 0;
}

/* THE GESTURE: one wheel tick down, one across, then the frame that closes them. */
static void two_axis_gesture(struct aws_server *server)
{
	aws_seat_pointer_axis(server, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(15.0),
			      WL_POINTER_AXIS_SOURCE_WHEEL, 1, 120);
	aws_seat_pointer_axis(server, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(-15.0),
			      WL_POINTER_AXIS_SOURCE_WHEEL, -1, -120);
	aws_seat_pointer_frame(server);
}

int main(void)
{
	struct aws_server server;
	int fds[2];

	s_display = wl_display_create();
	if (s_display == NULL) {
		printf("FAIL: could not create a Wayland display\n");
		return 1;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		printf("FAIL: could not make a socketpair\n");
		return 1;
	}
	s_client = wl_client_create(s_display, fds[0]);
	if (s_client == NULL) {
		printf("FAIL: could not create a Wayland client\n");
		return 1;
	}

	/* ============================================================================================
	 * 1. A VERSION-1 CLIENT HEARS THE AXIS AND NOTHING ELSE.
	 * ============================================================================================ */
	bind_at(&server, 1);
	two_axis_gesture(&server);
	printf("  version 1: %d call(s)\n", s_call_count);
	dump();
	check(count_of(CALL_AXIS) == 2, "a v1 client gets both axes");
	check(count_of(CALL_AXIS_SOURCE) == 0, "and NO axis_source, which does not exist before v5");
	check(count_of(CALL_AXIS_DISCRETE) == 0, "and no axis_discrete");
	check(count_of(CALL_AXIS_VALUE120) == 0, "and no axis_value120");
	check(count_of(CALL_FRAME) == 0, "and no frame — v1 clients have no frames at all");
	check(s_calls[0].value == wl_fixed_from_double(15.0), "the first axis carries its precise distance");
	check(s_calls[0].axis == WL_POINTER_AXIS_VERTICAL_SCROLL, "and says which axis it is");
	check(s_calls[1].axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL, "as does the second");
	check(s_calls[1].value == wl_fixed_from_double(-15.0), "with its own sign, unconverted");

	/* ============================================================================================
	 * 2. A VERSION-5 CLIENT GETS THE SOURCE ONCE, THE STEPS AS `axis_discrete`, AND ONE FRAME.
	 * ============================================================================================ */
	bind_at(&server, 5);
	two_axis_gesture(&server);
	printf("  version 5: %d call(s)\n", s_call_count);
	dump();
	check(count_of(CALL_AXIS) == 2, "a v5 client gets both axes");
	check(count_of(CALL_AXIS_SOURCE) == 1,
	      "and axis_source ONCE for the gesture, not once per axis");
	check(s_calls[0].kind == CALL_AXIS_SOURCE && s_calls[0].n == WL_POINTER_AXIS_SOURCE_WHEEL,
	      "saying it came from a WHEEL, before the axis it describes");
	check(count_of(CALL_AXIS_DISCRETE) == 2, "and both steps as axis_discrete");
	/* THE SHAPE IS source, discrete, axis, discrete, axis, frame — one step beside the distance it names. */
	check(s_calls[1].kind == CALL_AXIS_DISCRETE && s_calls[1].n == 1, "the vertical step being +1");
	check(s_calls[3].kind == CALL_AXIS_DISCRETE && s_calls[3].n == -1, "and the horizontal one -1");
	check(s_calls[2].kind == CALL_AXIS && s_calls[2].axis == WL_POINTER_AXIS_VERTICAL_SCROLL,
	      "each step beside the axis it belongs to");
	check(count_of(CALL_AXIS_VALUE120) == 0, "and NO axis_value120 — the same motion said twice is a double count");
	check(count_of(CALL_FRAME) == 1, "and exactly one frame closes it");
	check(s_calls[s_call_count - 1].kind == CALL_FRAME, "the frame being LAST");

	/* ============================================================================================
	 * 3. A VERSION-8 CLIENT GETS 120THS INSTEAD, AND NO `axis_discrete`.
	 * ============================================================================================ */
	bind_at(&server, 8);
	two_axis_gesture(&server);
	printf("  version 8: %d call(s)\n", s_call_count);
	dump();
	check(count_of(CALL_AXIS) == 2, "a v8 client gets both axes");
	check(count_of(CALL_AXIS_VALUE120) == 2, "and both steps in 120ths");
	check(s_calls[1].kind == CALL_AXIS_VALUE120 && s_calls[1].n == 120, "the vertical one being +120");
	check(s_calls[3].kind == CALL_AXIS_VALUE120 && s_calls[3].n == -120, "and the horizontal one -120");
	check(count_of(CALL_AXIS_DISCRETE) == 0,
	      "and NO axis_discrete — a v8 client must not be told the same step twice");
	check(count_of(CALL_AXIS_SOURCE) == 1, "one axis_source");
	check(count_of(CALL_FRAME) == 1, "and one frame");

	/* ============================================================================================
	 * 4. A FINGER GESTURE HAS NO STEPS, AND SAYS SO BY SENDING NONE.
	 * ============================================================================================ */
	bind_at(&server, 8);
	aws_seat_pointer_axis(&server, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(3.5),
			      WL_POINTER_AXIS_SOURCE_FINGER, 0, 0);
	aws_seat_pointer_frame(&server);
	printf("  finger: %d call(s)\n", s_call_count);
	dump();
	check(count_of(CALL_AXIS) == 1, "a trackpad gesture sends its axis");
	check(count_of(CALL_AXIS_VALUE120) == 0, "with NO 120ths, because a finger has no wheel steps");
	check(s_calls[0].kind == CALL_AXIS_SOURCE && s_calls[0].n == WL_POINTER_AXIS_SOURCE_FINGER,
	      "and the source says FINGER, which is how a client knows the units are not clicks");
	check(count_of(CALL_FRAME) == 1, "and the frame still closes it");

	/* ============================================================================================
	 * 5. A FRAME WITH NOTHING IN IT IS NOT SENT.
	 * ============================================================================================ */
	bind_at(&server, 8);
	aws_seat_pointer_frame(&server);
	check(s_call_count == 0, "a frame with no axis in it sends NOTHING — a client's scroll handler must not run "
				 "on a gesture that did not happen");

	/* AND A SECOND GESTURE OPENS A NEW FRAME, so the flag is not a one-shot. */
	two_axis_gesture(&server);
	check(count_of(CALL_FRAME) == 1, "a second gesture sends its own frame");
	check(count_of(CALL_AXIS_SOURCE) == 1, "and its own axis_source, once");

	/* ============================================================================================
	 * 6. A SCROLL OVER NO SURFACE GOES NOWHERE — but the frame still closes, so the next gesture is not
	 *    reported as part of this one.
	 * ============================================================================================ */
	bind_at(&server, 8);
	server.pointer_focus = NULL;
	s_under_pointer = NULL;
	two_axis_gesture(&server);
	check(count_of(CALL_AXIS) == 0, "a scroll over nothing reaches no client");
	check(count_of(CALL_FRAME) == 0, "and puts no frame on the wire");

	/* ============================================================================================
	 * 7. **THE END OF A SEQUENCE, WHICH NO EVENT ANNOUNCES.** A finger or continuous gesture is over when its axes
	 *    go quiet, and `axis_stop` is how a client hears that — the thing a kinetic scroll waits for. The seat's
	 *    timer calls this; here it is called directly so the wire can be read without waiting.
	 * ============================================================================================ */
	bind_at(&server, 8);
	aws_seat_pointer_axis(&server, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(3.5),
			      WL_POINTER_AXIS_SOURCE_FINGER, 0, 0);
	aws_seat_pointer_frame(&server);
	s_call_count = 0;	/* measure the stop alone */
	aws_seat_pointer_axis_stop(&server);
	printf("  finger stop: %d call(s)\n", s_call_count);
	dump();
	check(count_of(CALL_AXIS_STOP) == 1, "a finger scroll that goes quiet sends ONE axis_stop");
	check(s_calls[0].axis == WL_POINTER_AXIS_VERTICAL_SCROLL, "naming the axis that was moving");
	check(count_of(CALL_FRAME) == 1, "and closes it with a frame of its own");

	/* ONLY THE AXES THAT MOVED. A stop for an axis nobody touched would end a gesture that never started. */
	bind_at(&server, 8);
	aws_seat_pointer_axis(&server, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(1.0),
			      WL_POINTER_AXIS_SOURCE_FINGER, 0, 0);
	aws_seat_pointer_frame(&server);
	s_call_count = 0;
	aws_seat_pointer_axis_stop(&server);
	check(count_of(CALL_AXIS_STOP) == 1 && s_calls[0].axis == WL_POINTER_AXIS_VERTICAL_SCROLL,
	      "and never for the horizontal axis, which nobody moved");

	/* AND ONCE, NOT ON EVERY CALL: the sequence is closed when the stop goes out. */
	s_call_count = 0;
	aws_seat_pointer_axis_stop(&server);
	check(s_call_count == 0, "a second stop with nothing moving sends nothing");

	/* BOTH AXES MOVING IS TWO STOPS IN ONE FRAME. */
	bind_at(&server, 8);
	aws_seat_pointer_axis(&server, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(1.0),
			      WL_POINTER_AXIS_SOURCE_CONTINUOUS, 0, 0);
	aws_seat_pointer_axis(&server, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(1.0),
			      WL_POINTER_AXIS_SOURCE_CONTINUOUS, 0, 0);
	aws_seat_pointer_frame(&server);
	s_call_count = 0;
	aws_seat_pointer_axis_stop(&server);
	check(count_of(CALL_AXIS_STOP) == 2, "a two-axis gesture ends with a stop for EACH axis");
	check(count_of(CALL_FRAME) == 1, "both in one frame, so a client closes its gesture once");

	/* **AND A WHEEL OWES NO STOP AT ALL.** A notch is complete when it arrives; there is no sequence to end, so a
	 * stop after one would tell a client a gesture had finished that it never began. */
	bind_at(&server, 8);
	two_axis_gesture(&server);	/* wheel source */
	s_call_count = 0;
	aws_seat_pointer_axis_stop(&server);
	check(s_call_count == 0, "a WHEEL notch owes no axis_stop — a click is not a gesture");

	/* AND A CLIENT BELOW v5 HEARS NONE EITHER, because the event does not exist there. */
	bind_at(&server, 4);
	aws_seat_pointer_axis(&server, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(1.0),
			      WL_POINTER_AXIS_SOURCE_FINGER, 0, 0);
	aws_seat_pointer_frame(&server);
	s_call_count = 0;
	aws_seat_pointer_axis_stop(&server);
	check(s_call_count == 0, "and a v4 client is sent nothing, having no axis_stop to receive");

	printf("pointer_axis: %d case(s) failed\n", s_failed);
	close(fds[1]);
	return s_failed == 0 ? 0 : 1;
}
