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

int aws_backend_init(struct aws_server *server, const char *name, int width, int height)
{
	/* THE SIZE IS SET HERE RATHER THAN BY EACH BACKEND, so that a backend can only be a PLACE for pixels and
	 * not a decider of how many — a backend that wanted different dimensions would be asking for a policy
	 * this does not have. */
	server->backend.width = width;
	server->backend.height = height;

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

void aws_backend_present(struct aws_server *server)
{
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
