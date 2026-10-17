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

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A SIGNAL DOES NOT KILL THIS PROCESS; IT ASKS THE LOOP TO STOP. See where it is registered. */
static int on_signal(int signal_number, void *data)
{
	struct aws_server *server = data;

	(void)signal_number;
	/* **SAYING WHICH SIGNAL, BECAUSE "IT JUST STOPPED" IS NOT A DIAGNOSIS.** A compositor that ends silently
	 * leaves a frozen frame and a client that can only say "the window server has gone" — MEASURED, exactly that:
	 * a person's click made a menu appear and then the whole session died with nothing in either log to say why. */
	fprintf(stderr, "window-server: signal %d — stopping\n", signal_number);
	server->running = 0;
	return 0;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [--backend NAME] [--input NAME] [--width W] [--height H] [--socket NAME]\n"
		"          [--frames N] [--screenshot PATH.ppm]\n"
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
	/* **A SIGNAL MUST GO THROUGH THE LOOP, NOT AROUND IT.** `SIGINT` and `SIGTERM` end this process, and the
	 * DRM backend's teardown is what releases the display — so a compositor killed by Ctrl+C without running it
	 * leaves a screen showing its last frame, which is the failure every backend in this tree is written to
	 * avoid. Registering them ON THE EVENT LOOP means the loop stops and the ordinary exit path runs, in the
	 * ordinary order, with the display given back. */
	wl_event_loop_add_signal(server->loop, SIGINT, on_signal, server);
	wl_event_loop_add_signal(server->loop, SIGTERM, on_signal, server);
	while (server->running) {
		/* **A RENDER THAT IS DUE DOES NOT WAIT FOR AN EVENT, AND THAT IS THE WHOLE OF A BUG A PERSON SPOTTED.**
		 * A timeout of -1 means BLOCK UNTIL SOMETHING HAPPENS — and the only things that happen are a client's
		 * commit and, on the X11 backend, AN X EVENT, WHICH A MOUSE MOTION IS. MEASURED, from a session driven by
		 * hand: a menu opened by a click did not appear until the mouse moved, its rows did not highlight on the
		 * way over, and a drag made it show up — because THE SERVER WAS ONLY EVER RENDERING WHEN THE POINTER
		 * MOVED. A window created at startup (the menu bar's) never showed it, which is the asymmetry that gave it
		 * away. When a render is pending and the backend can take it, the loop polls instead: the work is DUE, and
		 * waiting for the next input to do work that is already waiting is how a screen comes to lag behind a hand
		 * by one gesture. */
		/* **AND A WAIT INTERRUPTED BY A SIGNAL IS NOT A FAILURE.** `wl_event_loop_dispatch` answers -1 with
		 * `errno` when its `poll`/`epoll_wait` is interrupted, and this loop used to treat that identically to a
		 * dead connection: `break` — with NOTHING printed, so the compositor simply vanished and left a frozen
		 * frame. MEASURED, from a live session: `window-server: 4763 frame(s) composited` and no other word, while
		 * the client reported "the window server has gone", twice, at moments nothing else was happening. A
		 * signal that arrives while the loop is blocked is ordinary on a desktop, so it now retries; anything else
		 * shouts. */
		{
			int rc = wl_event_loop_dispatch(server->loop,
							(server->render_pending && !aws_backend_busy(server)) ? 0 : -1);

			if (rc < 0) {
				if (errno == EINTR) {
					continue;
				}
				fprintf(stderr, "window-server: the event loop failed: %s — stopping\n",
					strerror(errno));
				break;
			}
		}
		wl_display_flush_clients(server->display);
		/* **AND NOT WHILE THE BACKEND IS HOLDING A PAGE.** `render_pending` is left SET when the backend is
		 * busy, so the frame is drawn as soon as the display has taken the last one — the flip event arrives on
		 * the DRM descriptor, which is on this loop, and the loop comes back round to here. That is the whole
		 * difference from waiting inside `present`: the wait is spent in the event loop, where the rest of the
		 * compositor keeps working, instead of inside a function that can do nothing else. */
		/* **AND THE SERVER SAYS WHAT IT IS DOING, ONCE A SECOND, IN ITS OWN WORDS.** Every attempt so far has read
		 * the CLIENT's side of this and none has asked the compositor what it thinks: how often it renders, and
		 * what it sees for each surface it holds. The headless backend shows the menu opening on one click every
		 * time and a live X11 session does not, and those two differ in the backend AND in the input source — so
		 * the next question has to be answered by the half of the system nobody has measured. */
		if (getenv("AWS_RENDER_DEBUG") != NULL) {
			static int s_tick;

			if (server->render_pending || ++s_tick >= 60) {
				struct aws_surface *s;

				s_tick = 0;
				fprintf(stderr, "  DIAG server: %s\n",
					server->render_pending ? "PAINTED (a render was due)"
							       : "IDLE (nothing was due)");
				wl_list_for_each(s, &server->surfaces, link) {
					fprintf(stderr, "    surface: mapped=%d visible=%d level=%d buffer=%d\n",
						s->mapped, s->visible, s->level, s->current_buffer != NULL);
				}
			}
		}
		if (server->render_pending && !aws_backend_busy(server)) {
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
