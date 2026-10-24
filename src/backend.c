/*
 * backend.c — choosing an output backend.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A BACKEND IS A PLACE TO PUT PIXELS. That is the entire contract, and it is deliberately small: everything
 * above it — clients, surfaces, xdg-shell, the compositing loop — sees a width, a height and a buffer, and
 * never asks where the buffer is going. So a new output device is a file that fills in three function
 * pointers rather than a change to the compositor.
 *
 * THE HEADLESS ONE IS NOT A TEST FIXTURE; IT IS AN OUTPUT. It is what a machine with no display driver can
 * still run Agora on, and it is what the automated tests use because it is the only one that cannot disturb
 * the machine it is started on.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *aws_backend_names(void)
{
	/* NAMED AS BUILT, NOT AS IMAGINED: the backends are chosen by CMake options
	 * (AWS_WITH_DRM, AWS_WITH_X11), so a build without one must not advertise it in
	 * --help or in the "no backend named" message. */
	return "headless"
#ifdef AWS_HAVE_X11
	       ", x11"
#endif
#ifdef AWS_HAVE_DRM
	       ", drm"
#endif
	       ;
}

int aws_backend_init(struct aws_server *server, const char *name, int width, int height, double scale)
{
	/* **THE SCALE IS RESOLVED BEFORE THE FRAMEBUFFER IS ALLOCATED, AND IT IS A DIVISOR EITHER WAY.** A backend told
	 * the logical size (x11, headless) allocates `logical x scale`; one that learns the physical size from the
	 * display (drm) overwrites width/height with its mode and its logical size is that divided by the scale. So the
	 * framebuffer below is always `width x height`, and `aws_output_width()` is always the number clients are told.
	 *
	 * THE ONE FIELD IS WHY THIS IS ONE PLACE: `--scale` wins, a backend's own hint is used when nobody said, and 1
	 * is the answer for anything that cannot tell. */
	server->backend.user_scale = (scale > 0.0) ? scale : 0.0;
	if (scale <= 0.0) {
		scale = 1.0;	/* provisional: a backend may replace this from its display during start */
	}
	server->backend.scale = scale;
	/* THE SIZE IS SET HERE RATHER THAN BY EACH BACKEND, so that a backend can only be a PLACE for pixels and
	 * not a decider of how many — a backend that wanted different dimensions would be asking for a policy
	 * this does not have. The request is LOGICAL, so the pixels are the request times the scale. */
	server->backend.logical_width = width;
	server->backend.logical_height = height;
	/* **THE FRAMEBUFFER IS THE LOGICAL SIZE TIMES THE SCALE, ROUNDED — AND THE LOGICAL SIZE IS NOT DERIVED BACK FROM
	 * IT.** At 1.5 the division would not return the number that was asked for, and a session that answered a 1280x720
	 * request with 853x480 would be a screen whose size changes depending on how you ask about it. */
	server->backend.width = (int)((double)width * scale + 0.5);
	server->backend.height = (int)((double)height * scale + 0.5);

	if (name == NULL || strcmp(name, "headless") == 0) {
		return aws_backend_headless_start(server);
	}
	if (strcmp(name, "x11") == 0) {
		return aws_backend_x11_start(server);
	}
	if (strcmp(name, "drm") == 0) {
		return aws_backend_drm_start(server);
	}
	fprintf(stderr, "window-server: no output backend named '%s' (have: %s)\n", name, aws_backend_names());
	return -1;
}

void aws_server_suspend(struct aws_server *server)
{
	if (server == NULL || server->suspended) {
		return;
	}
	server->suspended = 1;
	/* **THE BACKEND IS TOLD FIRST AND THE FLAG SECOND**, because its pause is what actually gives the device up — and
	 * a frame composed between the two would be presented to a display this process has already released. */
	if (server->backend.pause != NULL) {
		server->backend.pause(server);
	}
}

void aws_server_resume(struct aws_server *server)
{
	if (server == NULL || !server->suspended) {
		return;
	}
	if (server->backend.resume != NULL) {
		server->backend.resume(server);
	}
	server->suspended = 0;
	/* **AND EVERYTHING IS REPAINTED**, because the terminal came back with whatever the kernel drew on it — a text
	 * console, another session's leftovers — and every pixel of this compositor's scene is stale. */
	server->render_pending = 1;
}

void aws_backend_present(struct aws_server *server)
{
	/* A PRESENT WHILE THE DISPLAY IS SOMEBODY ELSE'S IS NOT A MISSED FRAME, IT IS A FRAME THAT MUST NOT HAPPEN. */
	if (server->suspended) {
		return;
	}
	/* A BACKEND WITH NOTHING TO PRESENT LEAVES THIS NULL, which is what a headless output is: the frame
	 * buffer IS the screen. */
	if (server->backend.present != NULL) {
		server->backend.present(server);
	}
	/* **AND THEN THE EVENTS PRESENTING MAY HAVE SWALLOWED.** Presenting flushes, and a flush is where a native
	 * library reads the connection into its own queue — see `struct aws_backend`'s `drain`. Doing it here, after
	 * whatever presented, is what makes it a property of presenting rather than of one present implementation. */
	if (server->backend.drain != NULL) {
		server->backend.drain(server);
	}
}

/* **WHOEVER ALLOCATED THE PIXELS FREES THEM, AND THAT IS THE BACKEND.** An unconditional `free` here was
 * wrong for exactly one of the three and cost a SIGSEGV at exit: the DRM backend's pixels are an `mmap` of a
 * kernel dumb buffer, which its own `stop` unmaps — and `free` on an mmap'd pointer is not a leak, it is a
 * crash. MEASURED: 300 frames composited and the display restored correctly, then exit 139 in the cleanup.
 * Hoisting the ownership to each backend also means a backend that maps rather than allocates needs no special
 * case here, which is what a fourth one would have had to add. */
/* WHETHER A FRAME MAY BE DRAWN NOW. A backend with nothing to say is always ready; the ones that are not are
 * the ones holding a page the display is still reading. */
int aws_backend_busy(struct aws_server *server)
{
	return server->backend.busy != NULL ? server->backend.busy(server) : 0;
}

void aws_backend_shutdown(struct aws_server *server)
{
	if (server->backend.stop != NULL) {
		server->backend.stop(server);
	}
	server->backend.pixels = NULL;
}

double aws_output_scale(const struct aws_server *server)
{
	return (server != NULL && server->backend.scale > 0.0) ? server->backend.scale : 1.0;
}

int aws_output_scale_integer(const struct aws_server *server)
{
	double scale = aws_output_scale(server);
	int whole = (int)scale;

	/* THE FRACTION ROUNDS UP, which is the direction that gives a client MORE pixels than the screen has rather than
	 * fewer. A scale already whole is unchanged — the common case costs one comparison. */
	if ((double)whole < scale) {
		whole++;
	}
	return whole < 1 ? 1 : whole;
}

int aws_output_width(const struct aws_server *server)
{
	return (server != NULL) ? server->backend.logical_width : 0;
}

int aws_output_height(const struct aws_server *server)
{
	return (server != NULL) ? server->backend.logical_height : 0;
}
