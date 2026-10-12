/*
 * backend-x11.c — a nested output: the compositor's screen is a window on an X server.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
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
 * THE PRESENT IS AN XPutImage OF THE WHOLE FRAME. That is 3.5 MB over the X connection per frame at 1280x720,
 * which is why it is a development backend and not a fast one — XShm would remove the copy and is a change to
 * this file alone, because nothing above it knows how the pixels get there.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AWS_HAVE_X11

#include <X11/Xlib.h>
#include <X11/Xutil.h>

struct x11_state {
	Display *display;
	int screen;
	Window window;
	GC gc;
	XImage *image;
	Atom wm_delete;
	struct wl_event_source *source;
};

static void x11_present(struct aws_server *server)
{
	struct x11_state *x11 = server->backend.data;

	XPutImage(x11->display, x11->window, x11->gc, x11->image, 0, 0, 0, 0,
		  (unsigned)server->backend.width, (unsigned)server->backend.height);
	XFlush(x11->display);
}

/* THE X CONNECTION IS A DESCRIPTOR LIKE ANY OTHER, so it is handed to the WAYLAND event loop rather than
 * waited on separately: one loop, one place where the process blocks, and no second poll to keep in step. */
static int x11_dispatch(int fd, uint32_t mask, void *data)
{
	struct aws_server *server = data;
	struct x11_state *x11 = server->backend.data;

	(void)fd;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		fprintf(stderr, "window-server: the X connection went away\n");
		server->running = 0;
		return 0;
	}
	while (XPending(x11->display) > 0) {
		XEvent event;

		XNextEvent(x11->display, &event);
		switch (event.type) {
		case Expose:
			/* The server owns the pixels, so an Expose is "put them here again" and nothing else. */
			x11_present(server);
			break;
		case ClientMessage:
			if ((Atom)event.xclient.data.l[0] == x11->wm_delete) {
				server->running = 0;
			}
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
	return 0;
}

static void x11_stop(struct aws_server *server)
{
	struct x11_state *x11 = server->backend.data;

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
	server->backend.data = NULL;
}

int aws_backend_x11_start(struct aws_server *server)
{
	struct x11_state *x11;
	size_t count = (size_t)server->backend.width * (size_t)server->backend.height;

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
	XSelectInput(x11->display, x11->window, ExposureMask | StructureNotifyMask);
	/* THE CLOSE BOX ASKS RATHER THAN KILLS, which is what makes the close path this process's to run. */
	x11->wm_delete = XInternAtom(x11->display, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(x11->display, x11->window, &x11->wm_delete, 1);
	x11->gc = XCreateGC(x11->display, x11->window, 0, NULL);
	x11->image = XCreateImage(x11->display, DefaultVisual(x11->display, x11->screen),
				  (unsigned)DefaultDepth(x11->display, x11->screen), ZPixmap, 0,
				  (char *)server->backend.pixels, (unsigned)server->backend.width,
				  (unsigned)server->backend.height, 32, 0);
	if (x11->image == NULL) {
		fprintf(stderr, "window-server: could not make an X image over the frame buffer\n");
		x11_stop(server);
		return -1;
	}
	XMapWindow(x11->display, x11->window);
	XFlush(x11->display);

	x11->source = wl_event_loop_add_fd(server->loop, ConnectionNumber(x11->display), WL_EVENT_READABLE,
					   x11_dispatch, server);
	server->backend.name = "x11";
	server->backend.data = x11;
	server->backend.present = x11_present;
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
