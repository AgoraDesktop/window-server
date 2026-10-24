/*
 * main.c — the window server's entry point.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
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
#include <time.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A SIGNAL DOES NOT KILL THIS PROCESS; IT ASKS THE LOOP TO STOP. See where it is registered. */
/* **A COMPOSITE COSTS A FULL-SCREEN COPY, SO THE RATE IS THE OUTPUT'S, NOT THE CLIENTS'.** A client that asks for a
 * frame the moment it is given one — the frame callback contract — will happily run as fast as the compositor can
 * answer, and MEASURED, one such client (a test wallpaper stub) drove 233 composites a second and 86% of a core
 * with nobody touching the machine. So a render that is due but EARLY is deferred to the next frame's boundary by
 * the event loop's own timeout: the work still happens, at the pace a display can show it. */
#define AWS_FRAME_NS 16666666LL		/* 1/60 s */

static int64_t aws_monotonic_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int64_t s_last_render_ns;


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
		"          [--frames N] [--screenshot PATH.ppm] [--system-menu-icon auto|NAME]\n"
		"          [--list-system-menu-icons] [--dump-os-identity]\n"
		"  --backend NAME    where the pixels go: %s (default headless)\n"
		"  --renderer NAME   who draws them: %s (default auto: gl when a backend has a GPU present)\n"
		"  --input NAME      where input comes from: %s (default libinput)\n"
		"  --width/--height  the output's size, in pixels (default 1280x720)\n"
		"  --socket NAME     the Wayland socket's name; the default is chosen and printed\n"
		"  --frames N        composite N frames, then stop (0, the default, runs until killed)\n"
		"  --screenshot P    write the final output to a PPM file\n"
		"  --system-menu-icon NAME\n"
		"                    force the System menu mark: an icon id, alias, distro id or\n"
		"                    family (default auto: detect the OS)\n"
		"  --list-system-menu-icons\n"
		"                    print the available marks and exit\n"
		"  --dump-os-identity\n"
		"                    print the detected OS identity and chosen mark, then exit\n",
		program, aws_backend_names(), aws_renderer_names(), aws_input_names());
}

int main(int argc, char **argv)
{
	const char *backend = "headless";
	const char *input = NULL;
	const char *renderer = NULL;
	int width = 1280;
	/* **THE DEVICE PIXELS PER LOGICAL PIXEL, AND 0 MEANS "ASK THE BACKEND".** `--scale 2` is what makes a HiDPI
	 * panel usable today: clients keep drawing at the sizes they always did and the compositor presents them twice
	 * as large, which is the right SIZE and a soft picture. A client that renders at 2x itself (buffer_scale) is then
	 * 1:1 and sharp — so the two halves are independent, and neither has to wait for the other. */
	int scale = 0;
	int height = 720;
	int frames = 0;
	const char *socket_name = NULL;
	const char *screenshot = NULL;
	const char *system_menu_icon = NULL;
	int list_system_menu_icons = 0;
	int dump_os_identity = 0;
	struct aws_server *server;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
			backend = argv[++i];
		} else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
			input = argv[++i];
		} else if (strcmp(argv[i], "--renderer") == 0 && i + 1 < argc) {
			renderer = argv[++i];
		} else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
			width = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
			height = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
			scale = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
			socket_name = argv[++i];
		} else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			frames = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
			screenshot = argv[++i];
		} else if (strcmp(argv[i], "--system-menu-icon") == 0 && i + 1 < argc) {
			system_menu_icon = argv[++i];
		} else if (strcmp(argv[i], "--list-system-menu-icons") == 0) {
			list_system_menu_icons = 1;
		} else if (strcmp(argv[i], "--dump-os-identity") == 0) {
			dump_os_identity = 1;
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	/* **THE DIAGNOSTIC DOORS OPEN BEFORE THE DISPLAY DOES.** `--list-system-menu-icons` and `--dump-os-identity`
	 * answer without a Wayland socket, a backend or an X server, which is what makes them useful on a machine whose
	 * compositor will not start — and in a bug report that says "distro X was not detected". */
	if (list_system_menu_icons) {
		size_t index;

		for (index = 0; index < aws_system_icon_count(); index++) {
			const struct aws_system_icon *icon = aws_system_icon_at(index);

			if (icon != NULL) {
				printf("%s\n", icon->id);
			}
		}
		return 0;
	}
	if (dump_os_identity) {
		struct aws_os_identity identity;
		const struct aws_system_icon *chosen;

		aws_os_identity_read(&identity);
		chosen = aws_system_menu_icon_resolve(&identity);
		if (system_menu_icon != NULL && strcmp(system_menu_icon, "auto") != 0) {
			chosen = aws_system_icon_by_name(system_menu_icon);
			if (chosen == NULL) {
				fprintf(stderr, "window-server: unknown system menu icon '%s'\n", system_menu_icon);
				return 2;
			}
		}
		printf("sysname: %s\n", identity.sysname[0] != '\0' ? identity.sysname : "(none)");
		printf("source: %s\n", identity.source != NULL ? identity.source : "(none)");
		printf("ID: %s\n", identity.id[0] != '\0' ? identity.id : "(none)");
		printf("ID_LIKE: %s\n", identity.id_like[0] != '\0' ? identity.id_like : "(none)");
		printf("NAME: %s\n", identity.name[0] != '\0' ? identity.name : "(none)");
		printf("family: %s\n", aws_system_menu_icon_family(&identity));
		printf("system menu icon: %s\n",
		       chosen != NULL ? chosen->id : AWS_SYSTEM_MENU_ICON_FALLBACK);
		return 0;
	}

	server = aws_server_create(backend, input, renderer, width, height, scale);
	if (server == NULL) {
		fprintf(stderr, "window-server: could not start (backend '%s')\n", backend);
		return 1;
	}
	if (system_menu_icon != NULL && strcmp(system_menu_icon, "auto") != 0) {
		if (aws_server_set_system_menu_icon(server, system_menu_icon) != 0) {
			fprintf(stderr, "window-server: unknown system menu icon '%s'\n", system_menu_icon);
			aws_server_destroy(server);
			return 2;
		}
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
	/* BOTH NUMBERS, because which one a reader wants depends on the question: a person wants the screen they have,
	 * and a diagnosis of a blurry or half-size frame wants the framebuffer. */
	fprintf(stderr, "window-server: listening on %s, %s output %dx%d, %dx%d device pixels (scale %d)\n",
		socket_name, server->backend.name, aws_output_width(server), aws_output_height(server),
		server->backend.width, server->backend.height, aws_output_scale(server));
	fprintf(stderr, "window-server: system menu: %s (source %s, ID=%s)\n",
		server->system_menu_icon != NULL ? server->system_menu_icon->id : AWS_SYSTEM_MENU_ICON_FALLBACK,
		server->os_identity.source != NULL ? server->os_identity.source : "(none)",
		server->os_identity.id[0] != '\0' ? server->os_identity.id : "(none)");

	server->running = 1;
	/* **A SIGNAL MUST GO THROUGH THE LOOP, NOT AROUND IT.** `SIGINT` and `SIGTERM` end this process, and the
	 * DRM backend's teardown is what releases the display — so a compositor killed by Ctrl+C without running it
	 * leaves a screen showing its last frame, which is the failure every backend in this tree is written to
	 * avoid. Registering them ON THE EVENT LOOP means the loop stops and the ordinary exit path runs, in the
	 * ordinary order, with the display given back. */
	/* **AND THE HANDLES ARE KEPT, BECAUSE A SOURCE IS THE LOOP'S TO REMOVE AND NOBODY ELSE'S.** Discarding
	 * them left two 64-byte sources registered on the loop at exit; `wl_display_destroy` frees the loop without
	 * walking its source list, so they were leaked — MEASURED under LSan as "128 byte(s) in 2 object(s)". They
	 * are removed below, while the loop is alive, which is the only time Wayland allows it. */
	struct wl_event_source *sigint_source = wl_event_loop_add_signal(server->loop, SIGINT, on_signal, server);
	struct wl_event_source *sigterm_source = wl_event_loop_add_signal(server->loop, SIGTERM, on_signal, server);
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
			int timeout = -1;
			int rc;

			if (server->render_pending && !aws_backend_busy(server)) {
				int64_t now = aws_monotonic_ns();
				int64_t due = s_last_render_ns + AWS_FRAME_NS;

				timeout = (now >= due) ? 0 : (int)((due - now) / 1000000LL) + 1;
			}
			rc = wl_event_loop_dispatch(server->loop, timeout);

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
		/* **AND THE RENDER ITSELF WAITS FOR THE FRAME'S BOUNDARY, WHICH IS THE HALF THAT ACTUALLY PACES.** Waiting
		 * only in the event loop's timeout does nothing while clients keep committing — every commit wakes the
		 * loop, and the render ran on each wake. MEASURED: 270 composites a second after the timeout alone. The
		 * deadline belongs on the PAINT. */
		if (server->render_pending && !aws_backend_busy(server) &&
		    aws_monotonic_ns() >= s_last_render_ns + AWS_FRAME_NS) {
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
			s_last_render_ns = aws_monotonic_ns();
			if (server->max_frames > 0 && server->frames >= server->max_frames) {
				break;
			}
		}
	}

	/* **THE SIGNAL SOURCES GO BEFORE THE LOOP DOES**, for the same reason the backend's and input's do: the
	 * loop owns the list they are on, and `aws_server_destroy` destroys it. */
	if (sigint_source != NULL) {
		wl_event_source_remove(sigint_source);
	}
	if (sigterm_source != NULL) {
		wl_event_source_remove(sigterm_source);
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
