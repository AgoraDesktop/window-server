/*
 * input.c — choosing where input comes from.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AN INPUT SOURCE IS INDEPENDENT OF AN OUTPUT BACKEND, and keeping them apart is the point: `libinput` reads
 * the machine's devices and `host` takes the events a nested window is given, and either can be paired with
 * either output. The common development pairing is the interesting one — the x11 output with `libinput` input —
 * because it puts Agora's compositor in a window with the REAL keyboard and mouse, which is the closest thing
 * to the shipping configuration a desktop can offer.
 *
 * THEY ARE MUTUALLY EXCLUSIVE, and that is not a preference: both feed the same seat, so a device read twice is
 * a pointer that moves at double speed and a keystroke delivered twice. So the nested backend only sends input
 * when it is the chosen source, which is what `input_from_host` means.
 */

#include "server.h"

#include <stdio.h>
#include <string.h>

const char *aws_input_names(void)
{
	return "libinput, host, none";
}

int aws_input_init(struct aws_server *server, const char *name, const char *backend_name)
{
	server->input_from_host = 0;

	/* THE DEFAULT IS THE DEVICES WHEN THEY CAN BE HAD, and the host window otherwise: a machine with its own
	 * keyboard and mouse should use them, and a nested run on a machine that has none is the case the host
	 * source exists for. */
	if (name == NULL) {
		name = "libinput";
	}
	if (strcmp(name, "none") == 0) {
		return 0;
	}
	if (strcmp(name, "host") == 0) {
		return aws_input_host_start(server);
	}
	if (strcmp(name, "libinput") == 0) {
		if (aws_input_libinput_start(server) == 0) {
			return 0;
		}
		/* FALLING BACK IS SAID OUT LOUD: a compositor that quietly has no input is one somebody debugs for an
		 * hour, and the reason libinput failed is the useful part. */
		fprintf(stderr, "window-server: libinput could not be started; falling back. The libinput lines above\n"
				"                say why; the usual cause is that /dev/input is not readable by this user —\n"
				"                the devices are root:input, and being in the `input` group is what grants it.\n");
		if (backend_name != NULL && strcmp(backend_name, "x11") == 0) {
			return aws_input_host_start(server);
		}
		return 0;
	}
	fprintf(stderr, "window-server: no input source named '%s' (have: %s)\n", name, aws_input_names());
	return -1;
}

int aws_input_host_start(struct aws_server *server)
{
	/* NOTHING TO SET UP: the backend whose window this is already has the events and checks this flag before
	 * sending them, so "starting" the host source is saying that it is the one. */
	server->input_from_host = 1;
	fprintf(stderr, "window-server: input from the nested window\n");
	return 0;
}

void aws_input_shutdown(struct aws_server *server)
{
	/* The libinput source removes its own event source and drops its references; see input-libinput.c. */
	extern void aws_input_libinput_stop(struct aws_server *server);

	aws_input_libinput_stop(server);
}
