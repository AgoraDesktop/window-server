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
#include "session-vt.h"

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


/* **A VT SWITCH ARRIVES AS A SIGNAL AND IS ANSWERED FROM THE LOOP.** The kernel asks before it takes the terminal
 * (`VT_PROCESS`), and this is where the compositor lets go: the backend drops DRM master and the CRTC, and only then is
 * the switch acknowledged — answering first would leave the screen being composited into by a process that has already
 * given it away. The same in reverse on the way back, with a repaint of every pixel. */
static struct aws_session_vt s_vt;

static int on_vt_release(int signal_number, void *data)
{
	struct aws_server *server = data;

	(void)signal_number;
	aws_server_suspend(server);
	aws_vt_ack_release(&s_vt);
	return 0;
}

static int on_vt_acquire(int signal_number, void *data)
{
	struct aws_server *server = data;

	(void)signal_number;
	aws_server_resume(server);
	aws_vt_ack_acquire(&s_vt);
	return 0;
}

/* **A TERMINAL WITH NOTHING ON IT MUST GIVE ITSELF BACK, AND THAT IS A DEADLINE RATHER THAN A POLICY.** `--frames N`
 * counts COMPOSITED frames, so a compositor with no client composites none and never reaches its own exit condition —
 * it sits on somebody's screen until it is killed. MEASURED, on this machine, by the person whose session it was: the
 * screen went to a login prompt, stayed there, and the terminal it had claimed could not be switched away from because
 * the VT negotiation it had armed was waiting for an answer from a process that had nothing to draw.
 *
 * So a claim has a deadline: `--vt-timeout` seconds to produce a frame, after which the terminal is given back and the
 * process exits with a message. It is disarmed by the first frame, so a working session never sees it. */
static struct wl_event_source *s_vt_timer;
static int s_vt_watching;

static int on_vt_timeout(void *data)
{
	struct aws_server *server = data;

	fprintf(stderr, "window-server: nothing was composited on terminal %d within the deadline — giving it "
			"back (a server with no client has nothing to draw; raise --vt-timeout if that is what "
			"happened)\n", s_vt.number);
	server->running = 0;
	return 0;
}

/* **AND THE NEGOTIATION IS ARMED ONLY ONCE THERE IS SOMETHING TO NEGOTIATE.** `VT_PROCESS` makes every terminal
 * switch a question the kernel asks THIS process, and an owner that cannot answer — because it is stuck, or because it
 * was killed and the mode outlived it — leaves the machine's terminals unswitchable. Arming it after the first frame
 * means a compositor that never gets that far never takes the keyboard's VT switches hostage. */
static void vt_watch_arm(struct aws_server *server)
{
	char why[256];

	if (s_vt.number < 0 || s_vt_watching) {
		return;
	}
	if (aws_vt_watch(&s_vt, "window-server", why, sizeof why) != 0) {
		fprintf(stderr, "window-server: on terminal %d but not negotiating switches: %s\n", s_vt.number, why);
		return;
	}
	wl_event_loop_add_signal(server->loop, SIGUSR1, on_vt_release, server);
	wl_event_loop_add_signal(server->loop, SIGUSR2, on_vt_acquire, server);
	s_vt_watching = 1;
	fprintf(stderr, "window-server: terminal switches are negotiated (SIGUSR1 release, SIGUSR2 acquire)\n");
}

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
		"  --launcher NAME   where the terminal comes from: existing (default — run it from a text\n"
		"                    console) or vt (switch to --vt N; needs root, and a display manager may\n"
		"                    own the terminal you name — see --vt-timeout)\n"
		"  --vt N            the terminal --launcher vt switches to. REQUIRED, and never chosen for\n"
		"                    you: a terminal the kernel calls free may be somebody's login prompt\n"
		"  --vt-timeout N    seconds a claimed terminal may go without a composited frame before it is\n"
		"                    given back and the server exits (default 10; 0 disables)\n"
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
	double scale = 0.0;
	/* **WHERE THE DISPLAY COMES FROM: THE TERMINAL IT WAS STARTED ON, OR A NEW ONE IT CLAIMS.**
	 * `existing` is the default and needs no privilege — run it from a text console and that console is the session.
	 * `vt` asks the kernel for a free terminal, switches to it, and gives it back on exit: the only way a terminal
	 * switch can be ANSWERED rather than survived, and the only way it can be started from anywhere. */
	const char *launcher = "existing";
	/* **THE TERMINAL IS NAMED, NEVER CHOSEN.** See `aws_vt_claim`: "the first free terminal" is a getty's on a machine
	 * with a display manager, and taking it moves a person off their session. */
	int vt_number = 0;
	int vt_timeout = 10;
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
			/* **A FRACTION IS ACCEPTED HERE BECAUSE 1.5 IS A REAL CONFIGURATION.** `--scale 1.5` is a 4K panel at a
			 * comfortable size or a laptop that ships at 150%, and `atof` is the whole difference from a session that
			 * could only be told whole numbers. */
			scale = atof(argv[++i]);
		} else if (strcmp(argv[i], "--vt") == 0 && i + 1 < argc) {
			vt_number = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--vt-timeout") == 0 && i + 1 < argc) {
			vt_timeout = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--launcher") == 0 && i + 1 < argc) {
			launcher = argv[++i];
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

	/* **THE TERMINAL IS CLAIMED BEFORE THE DEVICE, AND THE ORDER IS LOAD-BEARING.** A DRM master is the process the
	 * kernel's terminal bookkeeping names as the owner of the screen; taking the device first and the terminal second
	 * would claim a display for a session that is not yet the active one — which is exactly the state a compositor
	 * hangs in when it draws nothing and answers nothing. */
	aws_vt_init(&s_vt);
	if (strcmp(launcher, "vt") == 0) {
		char why[256];

		if (aws_vt_claim(&s_vt, vt_number, why, sizeof why) != 0) {
			fprintf(stderr, "window-server: --launcher vt could not take a terminal: %s\n", why);
			fprintf(stderr, "                (claiming one needs root or CAP_SYS_TTY_CONFIG; the default "
					"launcher, `existing`, runs on the terminal it was started from and needs neither)\n");
			return 1;
		}
		fprintf(stderr, "window-server: on terminal %d (started on %d)\n", s_vt.number, s_vt.previous);
	} else if (strcmp(launcher, "existing") != 0) {
		fprintf(stderr, "window-server: no launcher named '%s' (have: existing, vt)\n", launcher);
		return 2;
	}

	server = aws_server_create(backend, input, renderer, width, height, scale);
	if (server == NULL) {
		fprintf(stderr, "window-server: could not start (backend '%s')\n", backend);
		/* **A CLAIMED TERMINAL IS GIVEN BACK ON EVERY EXIT PATH, NOT ONLY THE HAPPY ONE.** Leaving it here would
		 * strand a person on a black screen because a compositor that failed to start still owns the display — the
		 * failure is loud and the terminal is not the place to report it. */
		aws_vt_release(&s_vt);
		return 1;
	}
	if (system_menu_icon != NULL && strcmp(system_menu_icon, "auto") != 0) {
		if (aws_server_set_system_menu_icon(server, system_menu_icon) != 0) {
			fprintf(stderr, "window-server: unknown system menu icon '%s'\n", system_menu_icon);
			aws_server_destroy(server);
			aws_vt_release(&s_vt);
			return 2;
		}
	}
	if (socket_name != NULL) {
		if (wl_display_add_socket(server->display, socket_name) < 0) {
			fprintf(stderr, "window-server: could not listen on %s\n", socket_name);
			aws_server_destroy(server);
			aws_vt_release(&s_vt);
			return 1;
		}
	} else {
		socket_name = wl_display_add_socket_auto(server->display);
		if (socket_name == NULL) {
			fprintf(stderr, "window-server: could not create a socket (is XDG_RUNTIME_DIR set?)\n");
			aws_server_destroy(server);
			aws_vt_release(&s_vt);
			return 1;
		}
	}
	server->max_frames = frames;
	server->screenshot = screenshot;

	printf("%s\n", socket_name);
	fflush(stdout);
	/* BOTH NUMBERS, because which one a reader wants depends on the question: a person wants the screen they have,
	 * and a diagnosis of a blurry or half-size frame wants the framebuffer. */
	fprintf(stderr, "window-server: listening on %s, %s output %dx%d, %dx%d device pixels (scale %g)\n",
		socket_name, server->backend.name, aws_output_width(server), aws_output_height(server),
		server->backend.width, server->backend.height, aws_output_scale(server));
	fprintf(stderr, "window-server: system menu: %s (source %s, ID=%s)\n",
		server->system_menu_icon != NULL ? server->system_menu_icon->id : AWS_SYSTEM_MENU_ICON_FALLBACK,
		server->os_identity.source != NULL ? server->os_identity.source : "(none)",
		server->os_identity.id[0] != '\0' ? server->os_identity.id : "(none)");

	if (s_vt.number >= 0) {
		/* **THE DEADLINE STARTS WITH THE CLAIM**, and it is on the event loop so it fires while the loop is blocked
		 * waiting for a client that may never come. */
		s_vt_timer = wl_event_loop_add_timer(server->loop, on_vt_timeout, server);
		if (s_vt_timer != NULL && vt_timeout > 0) {
			wl_event_source_timer_update(s_vt_timer, vt_timeout * 1000);
		}
	}
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
	/* **AND A HANGUP, WHICH IS WHAT A TERMINAL GOING AWAY LOOKS LIKE.** A compositor that ignored it would keep a
	 * claimed terminal after the session that started it had gone. */
	struct wl_event_source *sighup_source = wl_event_loop_add_signal(server->loop, SIGHUP, on_signal, server);
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
			/* THE FIRST FRAME IS THE PROOF THAT THERE IS A COMPOSITOR HERE, so the deadline goes and the terminal
			 * negotiation arrives. Both are one-way: a session that later goes quiet keeps its terminal, as a
			 * session should. */
			if (s_vt_timer != NULL) {
				wl_event_source_timer_update(s_vt_timer, 0);
			}
			if (server->frames == 1) {
				vt_watch_arm(server);
			}
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
	if (sighup_source != NULL) {
		wl_event_source_remove(sighup_source);
	}
	if (s_vt_timer != NULL) {
		wl_event_source_remove(s_vt_timer);
		s_vt_timer = NULL;
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
	/* **THE TERMINAL GOES BACK LAST**, after the display has been released: the compositor's own teardown drops DRM
	 * master and clears the CRTC, and switching the console away first would hand a person a black screen while this
	 * process was still believing it owned one. */
	aws_vt_release(&s_vt);
	return 0;
}
