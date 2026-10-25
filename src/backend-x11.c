/*
 * backend-x11.c — a nested output: the compositor's screen is a window on an X server.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * THIS IS A DEVELOPMENT BACKEND AND NOT A DISPLAY TARGET, and the distinction is the whole of the reasoning.
 * Agora's display target is Wayland on real hardware, with Xwayland for X CLIENTS. This backend exists because
 * a compositor has to be developed and demonstrated somewhere, and the alternatives are worse: a DRM/KMS
 * backend needs a free virtual terminal and can take over the machine's display, and a headless one cannot be
 * seen. Nested in a window is what weston's `--backend=x11` and mutter's `--nested` are, and it is why this
 * file is optional in the build — the core links no X11 at all unless it is asked for.
 *
 * IT CANNOT TAKE OVER THE MACHINE'S INPUT, WHICH IS THE THING THAT MAKES A NESTED COMPOSITOR DANGEROUS. This
 * server has no input devices at all yet (no libinput, see server.h), so this backend PRESENTS PIXELS AND
 * GRABS NOTHING: no XGrabPointer, no XGrabKeyboard, no fence around the pointer. A nested session that does
 * grab the host's input is exactly how a desktop is left with a dead mouse; a compositor that has nothing to
 * route has nothing to grab, and that stays true until input arrives on purpose.
 *
 * INPUT COMES FROM THE SAME WINDOW, AND WITHOUT GRABBING ANYTHING. A nested compositor's input IS its host
 * window's input: the host window manager gives this window the pointer when it is under it and the keyboard
 * when it is focused, and the events arrive through `XNextEvent` like any other client's. NOTHING HERE CALLS
 * XGrabPointer OR XGrabKeyboard — a window that takes input when it is given it cannot leave a desktop with a
 * dead mouse, which is what a grab is for and what this does not need. The X keycode is converted to evdev by
 * SUBTRACTING 8, the same offset a client adds back, so the code a client receives is the code its own
 * keymap expects.
 *
 * THE PRESENT IS AN XPutImage OF THE WHOLE FRAME. That is 3.5 MB over the X connection per frame at 1280x720,
 * which is why it is a development backend and not a fast one — XShm would remove the copy and is a change to
 * this file alone, because nothing above it knows how the pixels get there.
 */

#include "scale.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef AWS_HAVE_X11

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

#include <sys/ipc.h>
#include <sys/shm.h>

struct x11_state {
	Display *display;
	int screen;
	Window window;
	GC gc;
	XImage *image;
	Atom wm_delete;
	struct wl_event_source *source;
	/* THE SHARED-MEMORY IMAGE, when the X server has MIT-SHM: the frame buffer IS the segment, so a present is a
	 * pointer the server already has. `shm` is zero when it is not, and every use of it is guarded. */
	XShmSegmentInfo shm;
	int shared;
	/* NONZERO WHILE THE QUEUE IS BEING DRAINED, so a present reached from inside a drain cannot drain again
	 * underneath itself. An Expose schedules a frame rather than presenting, but a drain is also reachable from
	 * the descriptor callback and from startup, and one flag makes every path safe rather than one path argued. */
	int draining;
};

/* **A WALL-CLOCK MILLISECOND, SO A DIAGNOSTIC CAN BE LINED UP WITH ANOTHER PROCESS'S.** Two logs that each say
 * "the click happened" cannot be compared without a clock in both, and the question this answers — how long the
 * click waited before the view saw it — needs exactly that. */
static long long aws_diag_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

static void x11_present(struct aws_server *server)
{
	struct x11_state *x11 = server->backend.data;

	if (x11->shared) {
		/* **A PRESENT THAT PUTS NOTHING ON THE WIRE.** With MIT-SHM the frame buffer lives in a segment the X
		 * server has already attached, so this names the image and the server reads it — where `XPutImage` sent
		 * 5.8 MB of pixels over the connection for every frame at 1600x900, MEASURED at 7-9 ms a frame and the
		 * largest single cost left once the composite was made cheap. `XSync` is deliberate: the segment is drawn
		 * into again on the next frame, so the server must have FINISHED reading this one before the pointer is
		 * reused, and a round trip to the local server is a fraction of the copy it replaces. */
		XShmPutImage(x11->display, x11->window, x11->gc, x11->image, 0, 0, 0, 0,
			     (unsigned)server->backend.width, (unsigned)server->backend.height, False);
		XSync(x11->display, False);
		return;
	}
	XPutImage(x11->display, x11->window, x11->gc, x11->image, 0, 0, 0, 0,
		  (unsigned)server->backend.width, (unsigned)server->backend.height);
	XFlush(x11->display);
}

/* **THE HOST WINDOW'S PIXELS ARE NOT THE COMPOSITOR'S POINTS.** This backend's window is `output x scale` DEVICE
 * pixels, but everything above the backend — the hit test, a surface's origin, the coordinates a client receives —
 * is LOGICAL. Passing an X motion event through unconverted made every click land at `1/scale` of the distance from
 * the window's origin: at 1.332 a click in the middle of a control registered two-thirds of the way to it. The
 * division is the exact inverse of `cpu_device_edge`'s multiplication, and truncating is deliberate — the device
 * pixel belongs to the logical pixel it starts in, which is what makes adjacent windows tile at a shared edge. */
static int x11_logical(struct aws_server *server, int device)
{
	return aws_logical_from_device(device, server->backend.scale);
}

/* **THE ONE PLACE THE HOST'S QUEUE IS EMPTIED.** `XPending` does the two things this needs: it flushes what is
 * still buffered for the server, and it reads whatever has arrived off the connection — so the queue it then reports
 * is the real one, and the loop beside it collects even the events Xlib pulled in during a flush or a setup call
 * rather than only those that made the descriptor readable. It is FINITE: every iteration consumes one event, and a
 * flood from the host simply means this returns with the queue non-empty and the descriptor readable, which is a
 * callback the loop will make. */
static void x11_drain_events(struct aws_server *server)
{
	struct x11_state *x11 = server->backend.data;

	if (x11->draining) {
		return;
	}
	x11->draining = 1;
	while (XPending(x11->display) > 0) {
		XEvent event;

		XNextEvent(x11->display, &event);
		/* **WHAT THE HOST ACTUALLY HANDED THIS PROCESS.** "A click does nothing" is answered here and nowhere
		 * else: a button the compositor never received was lost between X and this loop, and a button it received
		 * but routed nowhere is a different bug (see `aws_seat_pointer_button`'s own log, which says which surface).
		 */
		if (getenv("AWS_INPUT_DIAG") != NULL &&
		    (event.type == ButtonPress || event.type == ButtonRelease || event.type == EnterNotify ||
		     event.type == LeaveNotify)) {
			const char *what = (event.type == ButtonPress) ? "ButtonPress"
				: (event.type == ButtonRelease) ? "ButtonRelease"
				: (event.type == EnterNotify) ? "EnterNotify" : "LeaveNotify";

			fprintf(stderr, "  DIAG x11 event %lld: %s at (%d,%d) button %u\n",
				(long long)aws_diag_ms(), what, event.xbutton.x, event.xbutton.y,
				(unsigned)event.xbutton.button);
		}
		switch (event.type) {
		case Expose:
			/* **AN EXPOSE ASKS FOR A FRAME, AND NOT FOR A COPY.** The CPU renderer's pixels are still there to put
			 * again, but a GL renderer's frame lives in a swapchain whose back buffer is undefined after a swap — so
			 * one answer that is right for both is "draw it again", which the render loop already knows how to do. */
			aws_server_schedule_render(server);
			break;
		case ClientMessage:
			if ((Atom)event.xclient.data.l[0] == x11->wm_delete) {
				/* **AND SAID OUT LOUD, because it is the OTHER way this process ends and it is otherwise
				 * indistinguishable from a signal.** A window manager sends this when the close button is
				 * pressed — and, for a window it has decided is not responding, when a person is offered to
				 * close it. */
				fprintf(stderr, "window-server: the window manager asked to close (WM_DELETE_WINDOW)\n");
				server->running = 0;
			}
			break;
		case MotionNotify:
			aws_seat_pointer_motion(server, x11_logical(server, event.xmotion.x),
						x11_logical(server, event.xmotion.y));
			break;
		case EnterNotify:
			aws_seat_pointer_enter(server, x11_logical(server, event.xcrossing.x),
					       x11_logical(server, event.xcrossing.y));
			break;
		case LeaveNotify:
			/* THE POINTER LEFT THE WINDOW, so it left the output: the client under it is told, because a
			 * client that never hears `leave` keeps drawing a hover it no longer has. */
			aws_seat_pointer_leave(server);
			break;
		case ButtonPress:
		case ButtonRelease: {
			/* X BUTTON NUMBERS ARE NOT EVDEV CODES. 1, 2 and 3 are the left, middle and right buttons and
			 * the rest are "other"; the client is told what a device would have said. */
			uint32_t button;
			int pressed = event.type == ButtonPress;

			/* **BUTTONS 4 TO 7 ARE THE WHEEL AND NOT BUTTONS AT ALL.** A nested session's mouse scrolls by
			 * pressing them, and the mapping below used to fall through to the "other button" arm and report
			 * BTN_SIDE — so scrolling in the demo clicked things instead of scrolling. X has no scroll event: the
			 * wheel IS these four button numbers, and the conversion is the whole of what a nested backend owes.
			 *
			 * **A SEPARATE GAP FROM THE libinput PATH, deliberately.** This is the development backend's
			 * translation of an X button into the same axis the real input path produces; the units are the ones
			 * a wheel tick means to libinput (15.0 distance, 120 hundred-and-twentieths, one discrete step) so
			 * that a client cannot tell which backend fed it.
			 *
			 * AND THE SIGNS ARE X'S, NOT WAYLAND'S: button 4 is wheel UP, and the protocol's positive vertical
			 * distance is DOWN. Flipped once, here, so nothing above this line has to know. */
			if (event.xbutton.button >= Button4 && event.xbutton.button <= 7) {
				int up = (event.xbutton.button == Button4);
				int down = (event.xbutton.button == Button5);
				int left = (event.xbutton.button == 6);
				uint32_t axis = (left || event.xbutton.button == 7)
					? WL_POINTER_AXIS_HORIZONTAL_SCROLL
					: WL_POINTER_AXIS_VERTICAL_SCROLL;
				int32_t v120 = (up || left) ? -120 : 120;

				aws_seat_pointer_motion(server, x11_logical(server, event.xbutton.x),
							x11_logical(server, event.xbutton.y));
				aws_seat_pointer_axis(server, axis, wl_fixed_from_double(v120 / 8.0),
						      WL_POINTER_AXIS_SOURCE_WHEEL, v120 / 120, v120);
				aws_seat_pointer_frame(server);
				break;
			}
			switch (event.xbutton.button) {
			case Button1: button = 0x110; break;	/* BTN_LEFT */
			case Button2: button = 0x112; break;	/* BTN_MIDDLE */
			case Button3: button = 0x111; break;	/* BTN_RIGHT */
			default:      button = 0x113 + (event.xbutton.button - 4); break;
			}
			/* THE POSITION FIRST: a button can arrive without a motion, and focus is decided by where the
			 * pointer IS, not by where it last moved to. */
			aws_seat_pointer_motion(server, x11_logical(server, event.xbutton.x),
						x11_logical(server, event.xbutton.y));
			aws_seat_pointer_button(server, button, pressed);
			break;
		}
		case KeyPress:
		case KeyRelease:
			/* MINUS 8 IS THE WHOLE CONVERSION: an X keycode is an evdev code plus 8, and a Wayland client's
			 * keymap is indexed by the evdev code. */
			aws_seat_key(server, (uint32_t)(event.xkey.keycode - 8), event.type == KeyPress);
			break;
		case ConfigureNotify:
			/* THE HOST WINDOW MANAGER MAY RESIZE THIS WINDOW AND THE OUTPUT DOES NOT FOLLOW IT: the output's
			 * size is what clients were TOLD it is, and changing it under them is a configure protocol this
			 * version does not run. The window is simply presented at its own size, clipped or with a margin. */
			break;
		default:
			break;
		}
	}
	x11->draining = 0;
}

/* **THE DESCRIPTOR SAID SOMETHING ARRIVED, SO DRAIN.** The loop wakes this process only when the connection is
 * readable; what it then empties may be more than what woke it, which is why the pump is shared. */
static int x11_dispatch(int fd, uint32_t mask, void *data)
{
	struct aws_server *server = data;

	(void)fd;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		fprintf(stderr, "window-server: the X connection went away\n");
		server->running = 0;
		return 0;
	}
	x11_drain_events(server);
	return 0;
}

/* **AND THE COPY PATH'S MEMORY, GIVEN BACK WHEN A GPU TAKES THE PRESENT.** The MIT-SHM segment and the XImage
 * exist to put a CPU frame on the screen; a renderer that swaps its own buffers has no use for either, and
 * leaving them attached would be a present pointer that paints the OLD pixels over the new ones. The frame buffer
 * is freed here and set to NULL: whoever allocates the next one (the GL renderer, for a screenshot's readback) is
 * its owner, which is the same rule `backend.c` states for `stop`. */
static void x11_release_present(struct aws_server *server)
{
	struct x11_state *x11 = server->backend.data;

	if (x11 == NULL) {
		return;
	}
	if (x11->image != NULL) {
		/* THE PIXELS ARE NOT THE IMAGE'S TO FREE — clearing `data` keeps X from freeing a segment or a buffer
		 * that this process owns. */
		x11->image->data = NULL;
		XDestroyImage(x11->image);
		x11->image = NULL;
	}
	if (x11->shared) {
		XShmDetach(x11->display, &x11->shm);
		shmdt(x11->shm.shmaddr);
		shmctl(x11->shm.shmid, IPC_RMID, NULL);
		x11->shared = 0;
	} else {
		free(server->backend.pixels);
	}
	server->backend.pixels = NULL;
	server->backend.present = NULL;
}

static void x11_stop(struct aws_server *server)
{
	struct x11_state *x11 = server->backend.data;
	int shared = (x11 != NULL) ? x11->shared : 0;

	if (x11 == NULL) {
		return;
	}
	if (x11->source != NULL) {
		wl_event_source_remove(x11->source);
	}
	if (x11->image != NULL) {
		/* THE PIXELS ARE OURS: clearing `data` keeps X from freeing the frame buffer on the way out. */
		x11->image->data = NULL;
		XDestroyImage(x11->image);
	}
	if (x11->shared) {
		XShmDetach(x11->display, &x11->shm);
		shmdt(x11->shm.shmaddr);
		shmctl(x11->shm.shmid, IPC_RMID, NULL);
		x11->shared = 0;
	}
	if (x11->gc != NULL) {
		XFreeGC(x11->display, x11->gc);
	}
	if (x11->window != 0) {
		XDestroyWindow(x11->display, x11->window);
	}
	if (x11->display != NULL) {
		XCloseDisplay(x11->display);
	}
	free(x11);
	/* **THE FRAME BUFFER IS THE CALLOC'D ONE ONLY WHEN IT IS NOT THE SEGMENT.** A shared buffer belongs to the
	 * shm segment and is released by the detach above; `free` on it would be a free of an address the kernel owns. */
	if (!shared) {
		free(server->backend.pixels);
	}
	server->backend.pixels = NULL;
	server->backend.data = NULL;
}

int aws_backend_x11_start(struct aws_server *server)
{
	struct x11_state *x11;
	size_t count;

	if (getenv("DISPLAY") == NULL) {
		fprintf(stderr, "window-server: the x11 backend needs DISPLAY set\n");
		return -1;
	}
	x11 = calloc(1, sizeof *x11);
	if (x11 == NULL) {
		return -1;
	}
	x11->display = XOpenDisplay(NULL);
	if (x11->display == NULL) {
		fprintf(stderr, "window-server: could not open the X display\n");
		free(x11);
		return -1;
	}
	x11->screen = DefaultScreen(x11->display);
	/* **THE HOST'S OWN NUMBERS ARE THE HINT A NESTED SESSION NEEDS, WITH ONE POLICY ON TOP.** This backend's
	 * window sits on somebody else's desktop, so "how big is a logical point here" is a question about THAT
	 * screen. The answer is `aws_scale_for_display`: the host screen's pixel width and millimetres are measured the
	 * same way the DRM path reads a connector, and 96 dpi answers only when they say nothing usable. The mode's
	 * HEIGHT is not consulted — a resolution is not a physical size, and a host 1080p or lower is measured (or
	 * given the same conventional fallback) exactly like a larger one. `--dpi` and `--scale` still win.
	 *
	 * IT HAS TO HAPPEN HERE AND NOT IN `aws_backend_init`, because the framebuffer of this backend is allocated from
	 * the request — so the hint has to be known before that. The request is LOGICAL, which is why it is multiplied
	 * rather than divided. */
	if (server->backend.user_scale == 0) {
		int mm = DisplayWidthMM(x11->display, x11->screen);
		int px = DisplayWidth(x11->display, x11->screen);
		int py = DisplayHeight(x11->display, x11->screen);
		double derived = aws_scale_for_display(px, mm);

		if (derived > 0.0) {
			int fallback = aws_scale_from_physical_size(px, mm) <= 0.0;
			/* **THE DISPLAY'S ANSWER FIRST, THEN THE USER'S COMFORT FACTOR**, because the two are different
			 * statements: the host screen says how many device pixels make a point, and the person in front of
			 * it may say that points are too small. `base` is what the screen said (and what the message below
			 * quotes); `derived` is the number the framebuffer and the wire get, quantised once at 1/256. */
			double base;

			base = aws_scale_quantized(derived);
			derived = aws_scale_with_ui(base, server->backend.ui_scale);
			server->backend.base_scale = base;
			server->backend.scale = derived;
			server->backend.width = (int)((double)server->requested_width * derived + 0.5);
			server->backend.height = (int)((double)server->requested_height * derived + 0.5);
			if (fallback) {
				fprintf(stderr, "window-server: the host display is %dx%d and did not report a usable size, so "
						"its scale is %g (%.0f dpi; override with --dpi or --scale)\n",
					px, py, base, AWS_FALLBACK_DPI);
			} else {
				fprintf(stderr, "window-server: the host display is %d px on %d mm (%.0f dpi), so its "
						"scale is %g (override with --dpi or --scale)\n",
					px, mm, (double)px * 25.4 / (double)mm, base);
			}
		}
	}
	count = (size_t)server->backend.width * (size_t)server->backend.height;
	server->backend.pixels = calloc(count, sizeof(uint32_t));
	if (server->backend.pixels == NULL) {
		fprintf(stderr, "window-server: could not allocate a %dx%d output\n", server->backend.width,
			server->backend.height);
		XCloseDisplay(x11->display);
		free(x11);
		return -1;
	}
	x11->window = XCreateSimpleWindow(x11->display, RootWindow(x11->display, x11->screen), 0, 0,
					  (unsigned)server->backend.width, (unsigned)server->backend.height, 0,
					  BlackPixel(x11->display, x11->screen),
					  WhitePixel(x11->display, x11->screen));
	XStoreName(x11->display, x11->window, "Agora Window Server");
	/* **AND WHAT AN EGL CONTEXT NEEDS FROM THIS BACKEND, WHICH IS THE WHOLE OF IT.** A display, a window and the
	 * visual that window was made with — the last because an EGL config has to match it, and `eglCreateWindowSurface`
	 * fails with `EGL_BAD_MATCH` on a config whose visual is somebody else's. The GL renderer asks for nothing else. */
	server->backend.native_display = x11->display;
	server->backend.native_window = (unsigned long)x11->window;
	server->backend.native_visual =
		(unsigned long)XVisualIDFromVisual(DefaultVisual(x11->display, x11->screen));
	/* THE INPUT MASKS ARE THE HOST'S PERMISSION SLIP: these are the events this window is given, and there is
	 * no grab among them. PointerMotionMask delivers a motion only while the pointer is inside the window,
	 * which is exactly the reach of a nested output. */
	XSelectInput(x11->display, x11->window,
		     ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
		     ButtonReleaseMask | PointerMotionMask | EnterWindowMask | LeaveWindowMask);
	/* THE CLOSE BOX ASKS RATHER THAN KILLS, which is what makes the close path this process's to run. */
	x11->wm_delete = XInternAtom(x11->display, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(x11->display, x11->window, &x11->wm_delete, 1);
	x11->gc = XCreateGC(x11->display, x11->window, 0, NULL);
	/* **THE SHARED-MEMORY FRAME BUFFER, WHICH IS THE ONLY PART OF THIS BACKEND THAT IS A COPY NOT A COMPROMISE.**
	 * `XPutImage` sends the pixels; `XShmPutImage` names a segment both processes have mapped, so the compositor
	 * writes into the server's memory and the present is a pointer. It is asked for and then FALLEN BACK FROM:
	 * an X server without MIT-SHM (a remote display, an unusual server) is still a display, and a compositor that
	 * refused to run on one would be worse than one that copies. */
	if (XShmQueryExtension(x11->display)) {
		x11->image = XShmCreateImage(x11->display, DefaultVisual(x11->display, x11->screen),
					     (unsigned)DefaultDepth(x11->display, x11->screen), ZPixmap, NULL,
					     &x11->shm, (unsigned)server->backend.width,
					     (unsigned)server->backend.height);
		if (x11->image != NULL) {
			x11->shm.shmid = shmget(IPC_PRIVATE,
						(size_t)x11->image->bytes_per_line * (size_t)x11->image->height,
						IPC_CREAT | 0600);
			if (x11->shm.shmid >= 0) {
				x11->shm.shmaddr = x11->image->data = shmat(x11->shm.shmid, NULL, 0);
				x11->shm.readOnly = False;
				if (x11->shm.shmaddr != (char *)-1 &&
				    XShmAttach(x11->display, &x11->shm)) {
					/* THE SEGMENT IS OURS TO DESTROY once the server has attached it; the
					 * attach is the handover and this is the receipt. */
					shmctl(x11->shm.shmid, IPC_RMID, NULL);
					x11->shared = 1;
					/* THE CALLOC'D BUFFER IS THE FALLBACK'S, AND IT WAS ALREADY MADE; the segment replaces
					 * it, so it goes here rather than leaking for the life of the session. */
					free(server->backend.pixels);
					server->backend.pixels = (uint32_t *)(void *)x11->shm.shmaddr;
					fprintf(stderr, "window-server: x11 output is shared with the server (MIT-SHM)\n");
				} else if (x11->shm.shmaddr != (char *)-1) {
					shmdt(x11->shm.shmaddr);
				}
			}
			if (!x11->shared) {
				/* THE DATA IS NOT THE IMAGE'S TO FREE — it is a segment this process has just detached —
				 * so it is cleared first, exactly as `x11_stop` does. */
				x11->image->data = NULL;
				XDestroyImage(x11->image);
				x11->image = NULL;
				x11->shm.shmid = 0;
			}
		}
	}
	if (x11->image == NULL) {
		x11->image = XCreateImage(x11->display, DefaultVisual(x11->display, x11->screen),
					  (unsigned)DefaultDepth(x11->display, x11->screen), ZPixmap, 0,
					  (char *)server->backend.pixels, (unsigned)server->backend.width,
					  (unsigned)server->backend.height, 32, 0);
	}
	if (x11->image == NULL) {
		fprintf(stderr, "window-server: could not make an X image over the frame buffer\n");
		x11_stop(server);
		return -1;
	}
	XMapWindow(x11->display, x11->window);
	/* **AND IT ASKS TO BE ON TOP, WHICH A NESTED OUTPUT DOES BECAUSE IT IS SOMEBODY'S SCREEN.** Without this
	 * the window is mapped underneath whatever the desktop already had, and a compositor whose screen is
	 * buried receives no pointer input at all — MEASURED: a synthetic click aimed at its own geometry landed on
	 * another application, because that application was above it. This is a raise and NOT a grab: the window
	 * manager is still free to put something else in front. */
	XRaiseWindow(x11->display, x11->window);
	XFlush(x11->display);

	x11->source = wl_event_loop_add_fd(server->loop, ConnectionNumber(x11->display), WL_EVENT_READABLE,
					   x11_dispatch, server);
	server->backend.name = "x11";
	server->backend.data = x11;
	server->backend.present = x11_present;
	server->backend.release_present = x11_release_present;
	/* **AND THE DRAIN, WHICH EVERY PRESENT GOES THROUGH.** It is the backend's and not the present's on purpose:
	 * the GL renderer replaces `present` with its own swap, and the queue a swap leaves behind is the same one. */
	server->backend.drain = x11_drain_events;
	server->backend.stop = x11_stop;
	fprintf(stderr, "window-server: nested in an X window on %s\n", DisplayString(x11->display));
	return 0;
}

#else /* !AWS_HAVE_X11 */

int aws_backend_x11_start(struct aws_server *server)
{
	(void)server;
	/* BUILT OUT RATHER THAN FAILING MYSTERIOUSLY: the core takes no X11 dependency, so a build without it has
	 * to say so where a caller would look for the reason. */
	fprintf(stderr, "window-server: this build has no x11 backend (it was configured without libX11)\n");
	return -1;
}

#endif
