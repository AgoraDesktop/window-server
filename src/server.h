/*
 * server.h — Agora's window server, the initial version.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
 *
 * WHAT THIS IS. Agora's window server is a Wayland COMPOSITOR, and this is the first one: it accepts clients,
 * gives them surfaces, and composites what they draw into an output. It exists so that `libagora-app` can be
 * tested against AGORA'S OWN SERVER instead of against a stock compositor it will not ship with — the
 * acceptance test is a real `NSWindow` rendering into this.
 *
 * WHAT IT DOES NOT DO YET, recorded rather than left to be discovered:
 *
 *   - TWO OUTPUT BACKENDS, AND NEITHER IS THE SHIPPING ONE. `headless` is a framebuffer in this process;
 *     `x11` is a NESTED window on an X server, which is how a compositor is developed and demonstrated without
 *     taking over a machine's display. A DRM/KMS backend is the real one and is not written yet. The
 *     compositing code does not care which backend it has, because it is only ever given a width, a height and
 *     a place to put pixels — which is what makes a new one a file rather than a change.
 *
 *     AN X11 BACKEND IS NOT AN X11 TARGET, and the distinction is the whole of the reasoning: Agora's display
 *     target is Wayland on real hardware, and Xwayland exists for X CLIENTS. This backend is a development
 *     one, the same thing weston's `--backend=x11` and mutter's `--nested` are. It is also OPTIONAL — the core
 *     does not link libX11 at all unless it is built in.
 *   - TWO INPUT SOURCES, AND THE OUTPUT DOES NOT DECIDE WHICH. `libinput` reads the DEVICES — the real thing,
 *     through udev and logind, no root and no `input` group — and `host` takes the events the nested backend's
 *     window is given. They are mutually exclusive and chosen with `--input`, because both feed the same seat
 *     and a device read twice is a pointer that moves twice as fast. The DEFAULT is libinput when it is built
 *     in and can start, and the nested window otherwise, which is the right answer in each case: a machine with
 *     devices wants them, and a machine with only a host window has only that.
 *
 *     NOTHING HERE GRABS A DEVICE. `libinput_device_grab` is never called, so the host session keeps working
 *     while this compositor reads the same events — which is what makes running it on a real desktop safe.
 *   - NO DRAWING OF ITS OWN. A Mac-like window server owns the menu bar, the dock and the window frames, and
 *     this owns none of them: it composites client buffers and paints a background. That is why it takes no
 *     drawing library yet — the row copy below is the whole of its drawing. A drawing library arrives with the
 *     first thing that has to be drawn.
 *   - NO POLICY. Every toplevel is placed by a cascade and sized by its client. Placement, stacking, the menu
 *     bar's reserve and the dock's are window-management POLICY, and policy is what this version deliberately
 *     has none of.
 *
 * THE ONE THING IT DOES INSIST ON is that the client's frame loop works: `wl_surface.frame` callbacks are sent
 * when a frame has been composited. Without them a client draws once and waits forever, which is exactly what
 * an AppKit window does — it paints on the frame callback and nowhere else.
 */

#ifndef AGORA_WINDOW_SERVER_H
#define AGORA_WINDOW_SERVER_H

#include <stdint.h>

#include <wayland-server.h>

struct aws_server;

/* A SURFACE, and the role it has been given. A surface is what a client draws into; the ROLE is what makes it
 * a window rather than a cursor or a popup, and this version knows exactly one role. */
struct aws_surface {
	struct wl_resource *resource;		/* wl_surface */
	struct aws_server *server;

	/* THE BUFFERS ARE KEPT AS THE CLIENT'S OWN RESOURCES, and deliberately not as a struct of ours: the user
	 * data of a wl_buffer made through wl_shm ALREADY holds libwayland's `wl_shm_buffer *`, so a compositor that
	 * stores its own struct there destroys the pointer libwayland needs — MEASURED, as a SIGSEGV inside
	 * `wl_shm_buffer_begin_access`. The fields are read from the shm buffer when they are wanted, which is once
	 * per composite. */
	struct wl_resource *pending_buffer;	/* attached since the last commit */
	struct wl_resource *current_buffer;	/* what the last commit presented */
	struct wl_listener buffer_listener;	/* fires when the CLIENT destroys the committed buffer */
	int has_buffer_listener;

	struct wl_list frame_callbacks;		/* struct aws_frame, one per wl_surface.frame request */

	/* THE ROLE. NULL until something gives this surface one — and a surface with no role is never composited,
	 * because compositing is what a ROLE is for. */
	struct wl_resource *xdg_surface;
	struct wl_resource *xdg_toplevel;
	char *title;
	char *app_id;
	int configure_sent;			/* the initial configure has gone; it is sent once */
	int configured;				/* the client has acked a configure and may attach a buffer */
	int mapped;

	int x;					/* where the compositor has put it, in output coordinates */
	int y;

	struct wl_list link;			/* the server's list of surfaces, in mapping order */
};

struct aws_frame {
	struct wl_resource *resource;		/* wl_callback */
	struct wl_list link;
	/* A CLIENT MAY DESTROY A CALLBACK IT ASKED FOR, and a list that kept it would be walked over freed
	 * memory on the next composite. This is what takes it out. */
	struct wl_listener destroy_listener;
};

/* THE OUTPUT BACKEND. Everything above it — clients, surfaces, xdg-shell, compositing — sees only `width`,
 * `height` and `pixels`, which is what makes an output device a file rather than a change. */
struct aws_backend {
	const char *name;
	int width;
	int height;
	uint32_t *pixels;			/* XRGB8888, the format every client here sends */

	/* CALLED ONCE, AFTER THE WAYLAND DISPLAY EXISTS — an X11 backend needs the event loop to add its own
	 * descriptor to. Returns 0 on success. */
	int (*start)(struct aws_server *server);
	/* PUT THE FRAMEBUFFER ON THE DISPLAY. Called after every composite, so it must be cheap enough to call
	 * sixty times a second; a backend with nothing to do (headless) leaves it NULL. */
	void (*present)(struct aws_server *server);
	/* GIVE BACK WHATEVER `start` TOOK. */
	void (*stop)(struct aws_server *server);

	void *data;				/* the backend's own state */
};

/* THE OUTPUT GLOBAL'S OWN STATE, which is a wl_output resource and nothing else: the size belongs to the
 * backend, because the backend is what knows it. */
struct aws_output {
	struct wl_resource *resource;
};

struct aws_server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wl_event_source *idle;		/* the render tick */

	struct wl_list surfaces;		/* in mapping order: first mapped is furthest back */
	struct wl_list toplevels;
	unsigned int toplevel_count;		/* for the cascade */

	int requested_width;			/* what the caller asked for, before a backend adjusted it */
	int requested_height;

	int input_from_host;			/* the nested backend's window feeds the seat (--input host) */
	void *input;				/* the chosen input source's own state */

	struct aws_backend backend;
	struct aws_output output;

	/* ---- THE SEAT. Which surfaces have the pointer and the keyboard, where the pointer is, and the xkb state
	 * that turns a key into the MODIFIER STATE a client needs to translate it. ---- */
	struct wl_list pointers;		/* struct aws_pointer, one per client that asked */
	struct wl_list keyboards;		/* struct aws_keyboard */
	struct aws_surface *pointer_focus;	/* the surface the pointer is over, or NULL */
	struct aws_surface *keyboard_focus;	/* the surface the keyboard goes to, or NULL */
	int pointer_x;				/* the pointer, in OUTPUT coordinates */
	int pointer_y;
	int pointer_inside;			/* the pointer is inside the output at all */
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;

	int render_pending;
	int frames;
	int max_frames;
	const char *screenshot;
	int running;
};

/* server.c */
struct aws_server *aws_server_create(const char *backend_name, const char *input_name, int width, int height);
void aws_server_destroy(struct aws_server *server);
void aws_server_schedule_render(struct aws_server *server);

/* surface.c — the globals a client draws through */
void aws_compositor_init(struct aws_server *server);
void aws_xdg_shell_init(struct aws_server *server);

/* output.c — the wl_output global, the compositing, and the screenshot */
void aws_output_init(struct aws_server *server);
void aws_output_render(struct aws_server *server);
int aws_output_write_ppm(struct aws_server *server, const char *path);

/* backend.c — choosing one, and the two calls the event loop makes */
int aws_backend_init(struct aws_server *server, const char *name, int width, int height);
void aws_backend_present(struct aws_server *server);
void aws_backend_shutdown(struct aws_server *server);
const char *aws_backend_names(void);

/* EACH BACKEND FILLS IN `server->backend` AND ANSWERS 0, or answers non-zero and says why on stderr. */
int aws_backend_headless_start(struct aws_server *server);
int aws_backend_x11_start(struct aws_server *server);


/* seat.c — the seat: the devices, the focus, and the keymap. */
void aws_seat_init(struct aws_server *server);
void aws_seat_finish(struct aws_server *server);

/* THE FOUR THINGS A BACKEND CAN SAY ABOUT INPUT. Each is what an input device would have reported, and the
 * only caller is a backend — which is what keeps "where input comes from" out of everything above. */
void aws_seat_pointer_enter(struct aws_server *server, int x, int y);
void aws_seat_pointer_leave(struct aws_server *server);
void aws_seat_pointer_motion(struct aws_server *server, int x, int y);
void aws_seat_pointer_button(struct aws_server *server, uint32_t evdev_button, int pressed);
void aws_seat_key(struct aws_server *server, uint32_t evdev_keycode, int pressed);

/* input.c — choosing an input source, and shutting it down */
int aws_input_init(struct aws_server *server, const char *name, const char *backend_name);
void aws_input_shutdown(struct aws_server *server);
const char *aws_input_names(void);

/* EACH SOURCE FILLS IN `server->input` AND ANSWERS 0, or answers non-zero and says why on stderr. */
int aws_input_libinput_start(struct aws_server *server);
int aws_input_host_start(struct aws_server *server);

/* GIVE THE KEYBOARD TO A SURFACE (or to nothing, with NULL). The compositor's focus POLICY lives in one place,
 * and this is the door it uses — a new window takes the focus and a click moves it. */
void aws_seat_set_keyboard_focus(struct aws_server *server, struct aws_surface *surface);

/* THE SURFACE AT A POINT, topmost first — the compositor's own hit test, and the thing focus is decided by. */
struct aws_surface *aws_surface_at(struct aws_server *server, int x, int y);

/* SEND A SURFACE ITS FRAME CALLBACKS, which is how a client learns its frame was taken. */
void aws_surface_frame_done(struct aws_surface *surface, uint32_t time);

#endif /* AGORA_WINDOW_SERVER_H */
