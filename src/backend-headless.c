/*
 * backend-headless.c — an output that is a buffer of pixels in this process.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
 *
 * IT PRESENTS NOTHING BECAUSE THERE IS NOTHING TO PRESENT TO: the frame buffer IS the output, and `present` is
 * left NULL rather than written as an empty function — a backend that has no screen should not have a place
 * in the code that pretends it does. What reads the pixels is `--screenshot`, which writes them to a file and
 * is the whole reason this backend is testable.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>

int aws_backend_headless_start(struct aws_server *server)
{
	size_t count = (size_t)server->backend.width * (size_t)server->backend.height;

	server->backend.pixels = calloc(count, sizeof(uint32_t));
	if (server->backend.pixels == NULL) {
		fprintf(stderr, "window-server: could not allocate a %dx%d output\n", server->backend.width,
			server->backend.height);
		return -1;
	}
	server->backend.name = "headless";
	server->backend.data = NULL;
	server->backend.present = NULL;
	server->backend.stop = NULL;
	return 0;
}
