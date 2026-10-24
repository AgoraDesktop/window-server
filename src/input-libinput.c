/*
 * input-libinput.c — input from the machine's own devices.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * THIS IS THE REAL INPUT PATH. libinput reads /dev/input and hands a compositor normalized events: a pointer
 * that is a pointer whatever the hardware is, a key that is already an EVDEV code (so there is no `- 8` here —
 * that conversion belongs to the X backend, whose keycodes are offset), and a seat assignment that comes from
 * udev.
 *
 * AND IT NEEDS NEITHER ROOT NOR THE `input` GROUP. `libinput_udev_assign_seat` asks logind for the devices of
 * the seat the session is on, and logind grants them to a process in that session — which is why this works
 * from an ordinary login and why reading /dev/input directly does not. If it fails, the message says so and the
 * caller falls back.
 *
 * IT DOES NOT GRAB. `libinput_device_grab` is never called, so the X server and every application on the
 * desktop keep receiving the same events while this compositor reads them too. That is what makes running
 * Agora's window server on a real desktop safe: two readers of one keyboard, and no exclusive claim on either.
 *
 * THE CURSOR IS THE COMPOSITOR'S NOW, WHICH IS A REAL CHANGE. The host source got absolute positions from a
 * window; a mouse reports DELTAS, so something has to keep the position, and that something is a compositor —
 * it is what the pointer means and where it is clamped to the output are window-server questions.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AWS_HAVE_LIBINPUT

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <stdarg.h>

#include <libinput.h>
#include <libudev.h>

/* LIINPUT TALKS, AND A COMPOSITOR THAT DOES NOT LISTEN IS ONE THAT FAILS SILENTLY. libinput reports a device
 * it could not open, a seat it could not take and a configuration it did not understand through this handler;
 * without one it says nothing at all, which is how a seat that was given no devices looked exactly like a seat
 * with nothing plugged into it. INFO by default, and DEBUG when AWS_INPUT_DEBUG is set. */
static void log_handler(struct libinput *libinput, enum libinput_log_priority priority, const char *format,
			va_list args)
{
	(void)libinput;
	(void)priority;
	fputs("libinput: ", stderr);
	vfprintf(stderr, format, args);
}

struct libinput_state {
	struct udev *udev;
	struct libinput *libinput;
	struct wl_event_source *source;

	/* THE CURSOR, IN OUTPUT COORDINATES, KEPT AS REAL NUMBERS: a mouse reports deltas smaller than a pixel,
	 * and rounding each one would lose every slow movement there is. */
	double x;
	double y;
};

static int open_restricted(const char *path, int flags, void *user_data)
{
	int fd;

	(void)user_data;
	fd = open(path, flags);
	return fd < 0 ? -errno : fd;
}

static void close_restricted(int fd, void *user_data)
{
	(void)user_data;
	close(fd);
}

static const struct libinput_interface libinput_interface = {
	open_restricted,
	close_restricted,
};

static void pointer_move(struct aws_server *server, struct libinput_state *in)
{
	int width = aws_output_width(server);
	int height = aws_output_height(server);

	/* CLAMPED TO THE OUTPUT, which is what a compositor's pointer is: there is nowhere else for it to be. */
	if (in->x < 0.0) {
		in->x = 0.0;
	}
	if (in->y < 0.0) {
		in->y = 0.0;
	}
	if (in->x > (double)(width - 1)) {
		in->x = (double)(width - 1);
	}
	if (in->y > (double)(height - 1)) {
		in->y = (double)(height - 1);
	}
	aws_seat_pointer_motion(server, (int)in->x, (int)in->y);
}

/* **ONE SCROLL EVENT, BOTH AXES, ONE FRAME.** libinput packs a two-axis gesture into a single event, and the
 * protocol wants the axes in one frame — so both are sent here and `aws_seat_pointer_frame` closes it once. The
 * position is refreshed first for the same reason a button does it: a scroll can arrive with no motion before it,
 * and where it goes is decided by where the pointer IS.
 *
 * THE TWO NUMBERS ARE ONE DISTANCE SAID TWICE, and which of them reaches the client is the client's version's
 * business (`aws_seat_pointer_axis` decides): `get_scroll_value` is the precise distance and
 * `get_scroll_value_v120` the same motion in 120ths of a wheel step. A finger gesture has no discrete steps at all,
 * and libinput answers zero there — which is the honest answer and the one that stops a trackpad from being
 * reported as clicking its way down a document. */
static void scroll_event(struct aws_server *server, struct libinput_event *event, uint32_t source)
{
	struct libinput_state *in = server->input;
	struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);
	int sent = 0;

	pointer_move(server, in);
	if (libinput_event_pointer_has_axis(pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)) {
		double value = libinput_event_pointer_get_scroll_value(pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
		int32_t value120 = libinput_event_pointer_get_scroll_value_v120(pointer,
									       LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);

		aws_seat_pointer_axis(server, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(value), source,
				      value120 / 120, value120);
		sent = 1;
	}
	if (libinput_event_pointer_has_axis(pointer, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL)) {
		double value = libinput_event_pointer_get_scroll_value(pointer, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
		int32_t value120 = libinput_event_pointer_get_scroll_value_v120(pointer,
									       LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);

		aws_seat_pointer_axis(server, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(value), source,
				      value120 / 120, value120);
		sent = 1;
	}
	if (sent) {
		aws_seat_pointer_frame(server);
	}
}

static void handle_event(struct aws_server *server, struct libinput_event *event)
{
	struct libinput_state *in = server->input;

	switch (libinput_event_get_type(event)) {
	case LIBINPUT_EVENT_DEVICE_ADDED: {
		struct libinput_device *device = libinput_event_get_device(event);

		fprintf(stderr, "window-server: input device: %s\n",
			libinput_device_get_name(device) != NULL ? libinput_device_get_name(device) : "(unnamed)");
		break;
	}
	case LIBINPUT_EVENT_DEVICE_REMOVED:
		break;
	case LIBINPUT_EVENT_POINTER_MOTION: {
		struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);

		in->x += libinput_event_pointer_get_dx(pointer);
		in->y += libinput_event_pointer_get_dy(pointer);
		pointer_move(server, in);
		break;
	}
	case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
		struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);

		/* AN ABSOLUTE DEVICE REPORTS IN ITS OWN SPACE, and the transformation to the output's is asked for
		 * rather than computed: libinput knows the tablet's range and this process does not. */
		/* **THE TRANSFORM'S RANGE IS THE LOGICAL SCREEN, NOT THE FRAMEBUFFER.** A tablet's absolute position is a
		 * pointer coordinate, and every pointer coordinate in this server is logical — so a 2x output that mapped
		 * the tablet across its device pixels would leave the right half of the screen unreachable and the left half
		 * covering twice the ink. */
		in->x = libinput_event_pointer_get_absolute_x_transformed(pointer, (uint32_t)aws_output_width(server));
		in->y = libinput_event_pointer_get_absolute_y_transformed(pointer, (uint32_t)aws_output_height(server));
		pointer_move(server, in);
		break;
	}
	case LIBINPUT_EVENT_POINTER_BUTTON: {
		struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);

		/* THE POSITION FIRST: a button can arrive with no motion before it, and the focus a click moves is
		 * decided by where the pointer IS. */
		pointer_move(server, in);
		aws_seat_pointer_button(server, libinput_event_pointer_get_button(pointer),
					libinput_event_pointer_get_button_state(pointer) == LIBINPUT_BUTTON_STATE_PRESSED);
		break;
	}
	case LIBINPUT_EVENT_KEYBOARD_KEY: {
		struct libinput_event_keyboard *keyboard = libinput_event_get_keyboard_event(event);

		/* AN EVDEV CODE ALREADY: libinput's key IS the code a Wayland client's keymap is indexed by. */
		aws_seat_key(server, libinput_event_keyboard_get_key(keyboard),
			     libinput_event_keyboard_get_key_state(keyboard) == LIBINPUT_KEY_STATE_PRESSED);
		break;
	}
	/* **THE WHEEL, AS THREE EVENT TYPES AND NOT ONE.** libinput 1.19 split the scroll event by SOURCE, because a
	 * wheel, a finger on a trackpad and a continuous device are three different gestures with three different
	 * units — and the old single `POINTER_AXIS` event is still sent **in addition**, which is exactly the double
	 * count the report names. Its own header says so: "Ignore this event if you are processing SCROLL_WHEEL,
	 * SCROLL_FINGER and SCROLL_CONTINUOUS." So the legacy type is ignored below, deliberately, and never both. */
	case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
		scroll_event(server, event, WL_POINTER_AXIS_SOURCE_WHEEL);
		break;
	case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
		scroll_event(server, event, WL_POINTER_AXIS_SOURCE_FINGER);
		break;
	case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
		scroll_event(server, event, WL_POINTER_AXIS_SOURCE_CONTINUOUS);
		break;
	case LIBINPUT_EVENT_POINTER_AXIS:
		/* **IGNORED, AND THAT IS THE FIX RATHER THAN AN OMISSION.** This is the deprecated event libinput still
		 * emits beside the three above; processing it as well would deliver every wheel tick twice, once through
		 * the modern path and once through this one. A build against a libinput older than the split would need
		 * this branch instead, and the build requires 1.19 or later for the three types to exist at all. */
		break;
	default:
		/* GESTURES, TABLETS AND SWITCHES ARE NOT DELIVERED YET, and they are dropped here rather than
		 * half-translated: a client sent an event nothing in this tree draws is a client that learned something
		 * untrue about its input. */
		break;
	}
}

/* TAKE EVERY EVENT libinput HAS QUEUED. This is separate from the fd callback because `libinput_dispatch` can
 * ALSO be called when the descriptor is quiet — at startup, where it is what enumerates the devices that are
 * already plugged in — and the events it queues then would otherwise sit unread against a descriptor that has
 * nothing more to say. MEASURED: the first version dispatched once at startup and read nothing, so a seat that
 * had been given its devices reported none. */
static void input_drain(struct aws_server *server)
{
	struct libinput_state *in = server->input;
	struct libinput_event *event;

	while ((event = libinput_get_event(in->libinput)) != NULL) {
		handle_event(server, event);
		libinput_event_destroy(event);
	}
}

static int input_dispatch(int fd, uint32_t mask, void *data)
{
	struct aws_server *server = data;
	struct libinput_state *in = server->input;

	(void)fd;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		server->running = 0;
		return 0;
	}
	/* DISPATCH FIRST, THEN READ: libinput reads the devices during the dispatch, so a loop that read first
	 * would find nothing and the events would sit until the next wakeup. */
	if (libinput_dispatch(in->libinput) != 0) {
		return 0;
	}
	input_drain(server);
	/* The events just became Wayland events; they have to leave. */
	wl_display_flush_clients(server->display);
	return 0;
}

int aws_input_libinput_start(struct aws_server *server)
{
	struct libinput_state *in = calloc(1, sizeof *in);

	if (in == NULL) {
		return -1;
	}
	in->udev = udev_new();
	if (in->udev == NULL) {
		fprintf(stderr, "window-server: could not create a udev context\n");
		free(in);
		return -1;
	}
	in->libinput = libinput_udev_create_context(&libinput_interface, server, in->udev);
	if (in->libinput != NULL) {
		libinput_log_set_handler(in->libinput, log_handler);
		libinput_log_set_priority(in->libinput, getenv("AWS_INPUT_DEBUG") != NULL
							  ? LIBINPUT_LOG_PRIORITY_DEBUG
							  : LIBINPUT_LOG_PRIORITY_INFO);
	}
	if (in->libinput == NULL) {
		fprintf(stderr, "window-server: could not create a libinput context\n");
		udev_unref(in->udev);
		free(in);
		return -1;
	}
	/* THE SEAT, WHICH IS WHERE THE PERMISSION COMES FROM: logind hands a process in the active session the
	 * devices of that seat, so this needs neither root nor the `input` group. */
	if (libinput_udev_assign_seat(in->libinput, "seat0") != 0) {
		fprintf(stderr, "window-server: logind would not give up seat0's devices\n");
		libinput_unref(in->libinput);
		udev_unref(in->udev);
		free(in);
		return -1;
	}
	in->source = wl_event_loop_add_fd(server->loop, libinput_get_fd(in->libinput), WL_EVENT_READABLE,
					  input_dispatch, server);
	server->input = in;
	/* THE POINTER STARTS IN THE MIDDLE OF THE OUTPUT, because there is no device that can say where it was
	 * before this compositor started, and the centre is the one place that is inside every output. */
	in->x = (double)aws_output_width(server) / 2.0;
	in->y = (double)aws_output_height(server) / 2.0;
	server->pointer_inside = 1;
	aws_seat_pointer_enter(server, (int)in->x, (int)in->y);
	/* AND THE DEVICES THAT ARE ALREADY PLUGGED IN ARE PICKED UP HERE, which needs the drain as well as the
	 * dispatch — see `input_drain`. */
	libinput_dispatch(in->libinput);
	input_drain(server);
	fprintf(stderr, "window-server: input from libinput on seat0\n");
	return 0;
}

void aws_input_libinput_stop(struct aws_server *server)
{
	struct libinput_state *in = server->input;

	if (in == NULL) {
		return;
	}
	if (in->source != NULL) {
		wl_event_source_remove(in->source);
	}
	libinput_unref(in->libinput);
	udev_unref(in->udev);
	free(in);
	server->input = NULL;
}

#else /* !AWS_HAVE_LIBINPUT */

int aws_input_libinput_start(struct aws_server *server)
{
	(void)server;
	fprintf(stderr, "window-server: this build has no libinput (configured without it)\n");
	return -1;
}

void aws_input_libinput_stop(struct aws_server *server)
{
	(void)server;
}

#endif
