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
 *   - IT DRAWS ITS OWN FURNITURE AND NOTHING ELSE. A Mac-like window server owns the menu bar, the dock and the
 *     window frames. This one fills the menu bar's STRIP — the first pixels here that are not a client's — and
 *     owns no more than that: the windows are the applications'. It still takes no drawing library, because a
 *     filled strip is a row loop. **ONLY THE STRIP IS FURNITURE**: everything IN it is a client's, and a server
 *     that drew an item would be making the mistake the dock rule exists to prevent. See DESIGN.md.
 *   - NO POLICY ABOUT WINDOWS, AND EXACTLY ONE ABOUT THE SCREEN. Placement, stacking and sizing are the
 *     APPLICATION's: no cascade, no tiling, and a frame is acted on as given. What the server does decide is
 *     what the SCREEN reserves for the furniture it owns — a different kind of question, because an application
 *     cannot know how tall the menu bar is.
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
	/* **WHETHER AN ATTACH HAPPENED, WHICH IS NOT THE SAME AS WHETHER THERE IS A BUFFER.** Wayland's attach
	 * SETS the buffer; not calling it leaves the previous one alone. Without this the two were one thing and a
	 * commit that simply did not re-attach took the window's buffer away — see `surface_commit`. */
	int pending_attached;
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
	/* **THE APPLICATION'S FRAME AND LEVEL, HONOURED.** `x`/`y` are where it said to go, the frame pair is how
	 * big it says it is, and `level` is the integer the composition is ordered by. See `src/shell.c`. */
	int frame_width;
	int frame_height;
	int level;
	/* **AND THE ONE KIND OF WINDOW WHOSE PLACEMENT IS NOT THE APPLICATION'S.** `NSStatusItem` has a length and
	 * no position, so its width is the whole of what its client is asked for and the packing is `shell.c`'s.
	 * Zero means an ordinary window, placed by its own frame like every other one. */
	int status_length;
	struct wl_resource *agora_window;	/* the role, when this surface's client is an Agora one */

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

	/* **NONZERO WHILE THE BACKEND CANNOT ACCEPT A FRAME** — the display has not taken the last one yet. A RENDER
	 * LOOP ASKS THIS BEFORE IT DRAWS, because drawing into a buffer the display may still be reading is the tear
	 * a double-buffered backend exists to avoid. NULL means always ready. */
	int (*busy)(struct aws_server *server);

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
int aws_backend_busy(struct aws_server *server);
void aws_backend_shutdown(struct aws_server *server);
const char *aws_backend_names(void);

/* EACH BACKEND FILLS IN `server->backend` AND ANSWERS 0, or answers non-zero and says why on stderr. */
int aws_backend_headless_start(struct aws_server *server);
int aws_backend_x11_start(struct aws_server *server);
int aws_backend_drm_start(struct aws_server *server);


/* **THE MENU BAR'S HEIGHT, IN ONE PLACE.** `aws_shell_reserved` subtracts it to answer the work area and
 * `aws_output_render` fills it to DRAW the strip, and a reserve and a strip that disagreed would be a window
 * placed under the bar or a bar over a window. It is the server's own furniture, so it is the server's constant;
 * a client asking `[[NSStatusBar systemStatusBar] thickness]` gets the same number through AppKit. */
#define AWS_MENU_BAR_HEIGHT 24
#define AWS_MENU_BAR_COLOUR 0xFFD8D8D8u

/* **THE SYSTEM MENU, WHICH IS THE SERVER'S OWN.** A square at the left of the bar, and inside it the hamburger —
 * `☰`, U+2630 — which IS three horizontal bars, so it is drawn rather than typeset and needs no font. The three
 * numbers below are the whole of the character: its bars, the gaps between them, and how wide the mark is. */
#define AWS_SYSTEM_MENU_WIDTH 28
#define AWS_SYSTEM_MENU_GLYPH_WIDTH 16
#define AWS_SYSTEM_MENU_BAR_THICKNESS 3
#define AWS_SYSTEM_MENU_BAR_GAP 3
#define AWS_SYSTEM_MENU_GLYPH_COLOUR 0xFF303030u

/* **AND THE AIR BETWEEN IT AND AN APPLICATION'S OWN MENUS, WHICH IS THE BAR'S AND NOT THE SQUARE'S.** The square is
 * the System menu's — it is what a click on it hits — and widening it to make room would move the GLYPH with it,
 * because the mark is centred in its square. The gap is a separate thing: the strip's own spacing, which the server
 * owns because the server owns the strip, and which a client cannot guess any more than it can guess the square's
 * width. It is part of the region `menu_bar_area` names, so the application's menus move right by exactly this much
 * and nothing on the client side changes. */
#define AWS_SYSTEM_MENU_GAP 8

/* shell.c — agora_shell: the frames, the levels and the work area the Mac's model needs. */
void aws_shell_init(struct aws_server *server);
void aws_shell_reserved(struct aws_server *server, int *x, int *y, int *width, int *height);
/* **WHERE AN APPLICATION'S OWN MENUS MAY BEGIN** — the strip minus the System menu's square. The one number about
 * the bar that a client cannot derive, because the System menu's width is the server's. See `agora-shell.xml`. */
void aws_shell_menu_bar_area(struct aws_server *server, int *x, int *y, int *width, int *height);

/* The packing of the status area, which a client adding or removing an item has just invalidated. */
void aws_shell_place_status_items(struct aws_server *server);

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
