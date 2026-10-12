/*
 * main.c — the window server's entry point.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
 *
 * THE EVENT LOOP, AND WHY IT RENDERS WHERE IT DOES. `wl_event_loop_dispatch` returns after the clients that had
 * something to say have said it — a commit, a configure ack, a title. Only THEN is there something new to put
 * on the screen, so the composite happens after the dispatch and never inside a request handler: a handler that
 * composited would do it once per request, and a client that attaches, damages and commits would be composited
 * three times for one frame.
 *
 * `--frames N` IS WHAT MAKES THIS CHECKABLE. A compositor runs forever; a test cannot. So the server can be told
 * to composite N frames and then write the output to a file and exit — which is how a screenshot of AGORA'S OWN
 * compositor becomes the acceptance test of the AppKit, rather than a screenshot of somebody else's.
 *
 *   window-server [--backend headless|x11] [--input libinput|host|none] [--width W] [--height H]
 *                 [--socket NAME]
 *                 [--frames N] [--screenshot PATH.ppm]
 *
 * The socket name is printed on STDOUT alone — a test captures it with a pipe, and everything else goes to
 * stderr so the two do not mix.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [--backend NAME] [--width W] [--height H] [--socket NAME] [--frames N]\n"
		"          [--screenshot PATH.ppm]\n"
		"  --backend NAME    where the pixels go: %s (default headless)\n"
		"  --input NAME      where input comes from: %s (default libinput)\n"
		"  --width/--height  the output's size, in pixels (default 1280x720)\n"
		"  --socket NAME     the Wayland socket's name; the default is chosen and printed\n"
		"  --frames N        composite N frames, then stop (0, the default, runs until killed)\n"
		"  --screenshot P    write the final output to a PPM file\n",
		program, aws_backend_names(), aws_input_names());
}

int main(int argc, char **argv)
{
	const char *backend = "headless";
	const char *input = NULL;
	int width = 1280;
	int height = 720;
	int frames = 0;
	const char *socket_name = NULL;
	const char *screenshot = NULL;
	struct aws_server *server;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
			backend = argv[++i];
		} else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
			input = argv[++i];
		} else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
			width = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
			height = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
			socket_name = argv[++i];
		} else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			frames = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
			screenshot = argv[++i];
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	server = aws_server_create(backend, input, width, height);
	if (server == NULL) {
		fprintf(stderr, "window-server: could not start (backend '%s')\n", backend);
		return 1;
	}
	if (socket_name != NULL) {
		if (wl_display_add_socket(server->display, socket_name) < 0) {
			fprintf(stderr, "window-server: could not listen on %s\n", socket_name);
			return 1;
		}
	} else {
		socket_name = wl_display_add_socket_auto(server->display);
		if (socket_name == NULL) {
			fprintf(stderr, "window-server: could not create a socket (is XDG_RUNTIME_DIR set?)\n");
			return 1;
		}
	}
	server->max_frames = frames;
	server->screenshot = screenshot;

	printf("%s\n", socket_name);
	fflush(stdout);
	fprintf(stderr, "window-server: listening on %s, %s output %dx%d\n", socket_name, server->backend.name,
		server->backend.width, server->backend.height);

	server->running = 1;
	while (server->running) {
		if (wl_event_loop_dispatch(server->loop, -1) < 0) {
			break;
		}
		wl_display_flush_clients(server->display);
		if (server->render_pending) {
			server->render_pending = 0;
			aws_output_render(server);
			/* AND THEN IT GOES ON THE SCREEN, if the backend has one. */
			aws_backend_present(server);
			/* **THE RENDER QUEUES EVENTS — the buffer release and the frame callbacks — SO THE FLUSH HAS TO
			 * FOLLOW IT.** The flush above happens before the render and therefore misses exactly the events
			 * that make the next frame possible: the client sits waiting for a callback that is still in this
			 * process's outgoing buffer, and the event loop blocks forever. MEASURED as a compositor that
			 * composited one frame and then stopped with a connected client. */
			wl_display_flush_clients(server->display);
			server->frames++;
			if (server->max_frames > 0 && server->frames >= server->max_frames) {
				break;
			}
		}
	}

	fprintf(stderr, "window-server: %d frame(s) composited\n", server->frames);
	if (server->screenshot != NULL) {
		if (aws_output_write_ppm(server, server->screenshot) == 0) {
			fprintf(stderr, "window-server: wrote %s\n", server->screenshot);
		} else {
			fprintf(stderr, "window-server: could NOT write %s\n", server->screenshot);
		}
	}
	aws_server_destroy(server);
	return 0;
}
