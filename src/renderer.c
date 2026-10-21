/*
 * renderer.c — choosing who draws the scene.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
 *
 * TWO ANSWERS AND ONE OF THEM IS ALWAYS AVAILABLE. `cpu` composites into the backend's frame buffer and works
 * everywhere, including a machine with no GPU and a headless test run. `gl` draws through EGL and GLES2 and only
 * exists when a backend handed it a native window — which, today, is the nested X11 backend.
 *
 * **`auto` IS THE DEFAULT, AND IT IS THE ONE THAT MAKES THE NESTED OUTPUT ACCELERATED BY ACCIDENT RATHER THAN BY
 * REQUEST.** A backend that cannot offer a native window (headless, DRM/KMS without GBM) simply falls to the CPU
 * renderer and says nothing, because that is not a failure. But a renderer ASKED FOR BY NAME and impossible is a
 * failure and says so: the same rule the output backends follow — a requested thing whose library is missing stops
 * the configure rather than silently becoming something else.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *aws_renderer_names(void)
{
	return "auto, cpu"
#ifdef AWS_HAVE_GL
	       ", gl"
#endif
	       ;
}

int aws_renderer_init(struct aws_server *server, const char *name)
{
	int explicit_gl = 0;
	int wants_gl = 1;

	if (name == NULL || strcmp(name, "auto") == 0) {
		wants_gl = 1;
	} else if (strcmp(name, "cpu") == 0) {
		wants_gl = 0;
	} else if (strcmp(name, "gl") == 0) {
		wants_gl = 1;
		explicit_gl = 1;
	} else {
		fprintf(stderr, "window-server: no renderer named '%s' (have: %s)\n", name,
			aws_renderer_names());
		return -1;
	}

	if (wants_gl && aws_renderer_gl_create(server) == 0) {
		fprintf(stderr, "window-server: renderer: %s\n", server->renderer->name);
		return 0;
	}
	if (explicit_gl) {
		/* **ASKED FOR BY NAME AND IMPOSSIBLE IS SAID OUT LOUD**, because a person who wrote `--renderer gl`
		 * wants to know the GPU is not being used — a silent fallback to the CPU would look like acceleration
		 * that is merely slow. */
		fprintf(stderr, "window-server: the gl renderer could not start and it was asked for by name\n");
		return -1;
	}
	if (aws_renderer_cpu_create(server) != 0) {
		return -1;
	}
	fprintf(stderr, "window-server: renderer: %s\n", server->renderer->name);
	return 0;
}

void aws_renderer_shutdown(struct aws_server *server)
{
	if (server->renderer == NULL) {
		return;
	}
	if (server->renderer->stop != NULL) {
		server->renderer->stop(server);
	}
	free(server->renderer);
	server->renderer = NULL;
}
