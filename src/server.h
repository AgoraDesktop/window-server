/*
 * server.h — Agora's window server, the initial version.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
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
 *
 *     **AND THE POINTER, WHERE THERE IS NO HOST TO DRAW IT.** A nested backend's window already shows the host's
 *     cursor; a DRM output has only these pixels, so `cursor.c` composites a fallback arrow above every surface and
 *     `backend.owns_cursor` says which backends need it. A client's cursor surface is still not implemented.
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

#include "cursor.h"
#include "osdetect.h"
#include "systemicon.h"

struct aws_server;
struct gbm_device;
struct gbm_surface;
struct gbm_bo;

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
	char *badge;			/* the Dock tile's label this application set; empty when none */
	int configure_sent;			/* the initial configure has gone; it is sent once */
	int configured;				/* the client has acked a configure and may attach a buffer */
	int mapped;
	/* **THE APPLICATION'S FRAME AND LEVEL, HONOURED.** `x`/`y` are where it said to go, the frame pair is how
	 * big it says it is, and `level` is the integer the composition is ordered by. See `src/shell.c`. */
	int frame_width;
	int frame_height;
	/* **THE CLIENT'S BUFFER IS THIS MANY DEVICE PIXELS PER LOGICAL ONE, AND THE COMPOSITOR HAS TO KNOW.**
	 * `wl_surface.set_buffer_scale` is how a client says "my buffer is 2x the surface I am describing": a
	 * 640x400 window on a 2x output sends a 1280x800 buffer and asks for it to be placed as 640x400. Everything
	 * above this line — the frame, the level, the hit test, the shell's placement — is in LOGICAL pixels, so the
	 * scale is only ever a divisor at the two places that touch the BUFFER: compositing it and hit-testing it.
	 * Until this existed the request was an empty function, so a client that honoured an advertised output scale
	 * would have overflowed its own frame by the factor.
	 *
	 * DOUBLE-BUFFERED, as the protocol says: the request takes effect on the NEXT commit, which is why the
	 * pending copy is here. A client sets the scale and the size together and they must land together.
	 *
	 * **AND IT IS A DOUBLE BECAUSE THE PROTOCOL'S INTEGER IS NOT THE REAL NUMBER.** `agora_window.set_buffer_scale_fixed`
	 * declares the display's own fraction (1.333, 0.805), which is what lets a client rasterise on the final pixel
	 * grid and the compositor place it 1:1. `wl_surface.set_buffer_scale` still writes this field, with a whole
	 * number, for clients that do not speak the extension. */
	double buffer_scale;			/* device pixels per logical pixel; 1 until a client says otherwise */
	double pending_buffer_scale;		/* what the scale becomes on the next commit */
	int level;
	/* **AND THE ONE KIND OF WINDOW WHOSE PLACEMENT IS NOT THE APPLICATION'S.** `NSStatusItem` has a length and
	 * no position, so its width is the whole of what its client is asked for and the packing is `shell.c`'s.
	 * Zero means an ordinary window, placed by its own frame like every other one. */
	int status_length;
	/* **WHETHER THE APPLICATION WANTS THIS ON THE SCREEN.** A surface with a buffer is not necessarily a surface
	 * anyone should see: `-orderOut:` is a real thing, and without this the server painted every window a client had
	 * ever hidden — which is what "the menus never close" looks like from the outside. */
	int visible;
	/* **WHETHER THIS WINDOW IS TAKING THE POINTER WHEN IT IS OVER NO SURFACE.** A menu sets this while it tracks,
	 * so the click that lands outside it — on the desktop, where the compositor would otherwise have nobody to
	 * tell — still reaches the client, which is how "click anywhere to dismiss" is said. See the protocol's
	 * `set_pointer_grab`. */
	int pointer_grab;
	/* **WHETHER THIS WINDOW IS HOLDING THE POINTER OUTRIGHT.** A window being dragged or resized sets this, and
	 * while it is set every pointer event is delivered HERE whatever is under the pointer — the hand moves out
	 * from under the window it is resizing, so "what is under the pointer" is the one question nobody is asking.
	 * See the protocol's `set_pointer_capture`; it is an INTERACTIVE grab and not the fallback above. */
	int pointer_capture;
	/* **AND HOW MUCH OF THE SCREEN THIS WINDOW HAS TAKEN FOR ITSELF.** The client says an edge and a length
	 * (`set_reserved`), the work area is the output minus what every reserver asked for, and the Dock is the
	 * client that made this a request rather than a constant. `reserved_amount` 0 means nothing is reserved. */
	int reserved_edge;			/* an `agora_window.edge`; meaningless while reserved_amount is 0 */
	int reserved_amount;			/* pixels along that edge, summed with every other surface reserving it */
	/* **AND WHETHER A CLICK ON IT MAY GIVE ITS APPLICATION THE KEYBOARD.** A Dock is the window this exists for:
	 * clicking a tile must activate the APPLICATION THE TILE NAMES, and a Dock that took the keyboard would take
	 * the menu bar with it. Default 1; `set_activatable` writes it. */
	int activatable;
	unsigned int id;			/* the server's number for this window; 0 until it is given an agora role */
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
	/* **THE FRAMEBUFFER, IN DEVICE PIXELS — AND `scale` IS HOW MANY OF THEM MAKE A LOGICAL ONE.** These two are what
	 * the renderers write and what a backend allocates; every WINDOW, every level, every hit test and everything a
	 * client is told about the screen is in LOGICAL pixels, which is `aws_output_width()` and not these. Before the
	 * scale existed the two were the same number, which is why the distinction has to be made at every use rather
	 * than assumed: a 2x output is 2560 device pixels and 1280 logical ones, and a window at x=100 is at device
	 * pixel 200. */
	int width;
	int height;
	/* **THE SCREEN THE CLIENT IS TOLD ABOUT, KEPT RATHER THAN DIVIDED BACK OUT.** With a fractional scale
	 * `width / scale` is not the number the request named (1280 / 1.5 = 853.33, and the session is 853 or 1280
	 * depending on what it asked for) — so the logical size is its own fact and the framebuffer is derived from it. */
	int logical_width;
	int logical_height;
	/* **DEVICE PIXELS PER LOGICAL PIXEL, AND IT IS NOT NECESSARILY A WHOLE NUMBER.** A 1.5x session is a real
	 * configuration — a 4K panel at a comfortable size, a laptop that ships at 150% — and the compositor has to draw
	 * 1.5 device pixels per logical one. What a CLIENT is told is still an integer, because `wl_output.scale` is one:
	 * `aws_output_scale_integer()` rounds UP, so a client on a 1.5x output renders at 2x and the compositor downsamples
	 * 2 -> 1.5. That is what makes 1.5 sharp rather than a 1.5-times magnifying glass. */
	double scale;
	/* **WHAT THE CALLER ASKED FOR, WHICH IS NOT THE SAME AS WHAT THE OUTPUT ENDED UP AT.** 0 means nobody said, so
	 * a backend that can INFER a scale from its display may fill `scale` in during its own start; a non-zero value is
	 * an instruction and no backend may override it. */
	double user_scale;
	uint32_t *pixels;			/* XRGB8888, the format every client here sends */

	/* **AND WHERE A GPU COULD PUT THEM, WHEN THIS BACKEND HAS ONE.** A windowed backend that can hand an EGL
	 * context a native window says so here — the X11 backend fills these in, and the others leave them zero — and
	 * that is the whole of what the GL renderer needs from a backend. A backend that cannot leaves them zero and
	 * gets the CPU renderer, which is the answer rather than a failure: a place to put pixels is still a place. */
	void *native_display;			/* X11: Display * */
	unsigned long native_window;		/* X11: Window */
	unsigned long native_visual;		/* X11: the window's visual id, for choosing an EGL config */

	/* **AND WHERE A GPU DRAWS STRAIGHT TO A SCANNER: GBM.** A DRM backend has no window to hand EGL, so it hands
	 * the card's device instead. The renderer makes a GBM surface from it, renders into it, and on present asks
	 * the backend to put the locked front buffer on the plane (`gbm_present`). The surface is the RENDERER's to
	 * make and destroy; the backend keeps the pointer only because releasing a buffer back to a GBM surface needs
	 * it, and that release happens on a page-flip event the backend owns. */
	struct gbm_device *gbm_device;		/* DRM: the GBM device made from the card's fd */
	struct gbm_surface *gbm_surface;	/* set by the GL renderer once it has made one */
	/* Show one locked GBM buffer. Nonzero means it could not be shown and the RENDERER gives the buffer back. */
	int (*gbm_present)(struct aws_server *server, struct gbm_bo *bo);

	/* CALLED ONCE, AFTER THE WAYLAND DISPLAY EXISTS — an X11 backend needs the event loop to add its own
	 * descriptor to. Returns 0 on success. */
	int (*start)(struct aws_server *server);
	/* **THE SECOND HALF OF STARTING, ONCE A RENDERER HAS BEEN CHOSEN.** A GPU backend must not allocate its
	 * fallback pages before it knows whether the GL renderer took the present; a CPU backend has nothing to
	 * defer. Returns 0 on success. NULL for a backend whose `start` was the whole story. */
	int (*start_present)(struct aws_server *server);
	/* PUT THE FRAMEBUFFER ON THE DISPLAY. Called after every composite, so it must be cheap enough to call
	 * sixty times a second; a backend with nothing to do (headless) leaves it NULL. */
	void (*present)(struct aws_server *server);
	/* **AND THE OTHER PRESENT, WHICH A GPU OWNS.** A renderer that draws through EGL swaps the buffers itself, so
	 * it replaces `present` above — but the backend may still be holding the copy path's memory (the X11
	 * backend's MIT-SHM segment and XImage), and that is what this asks it to give back. NULL for a backend that
	 * never had a copy path to release. */
	void (*release_present)(struct aws_server *server);
	/* **CALLED AFTER EVERY PRESENT, WHATEVER DID THE PRESENTING, AND IT IS THE NATIVE LIBRARY'S QUEUE IT
	 * EMPTIES.** A windowed backend's presentation goes through a library that keeps its OWN buffered input: Xlib
	 * reads events off the connection into an internal queue during `XFlush`/`XSync` and during the setup calls
	 * that precede them, and this server only ever drained that queue from its descriptor-ready callback. Once the
	 * socket is emptied into Xlib's queue the descriptor is NOT readable, so the event loop has no reason to call
	 * the callback and a key release sits in a queue nobody looks at until the next mouse move — which is exactly
	 * the "a tap repeats until the pointer moves" a person reported, MEASURED from a nested session.
	 *
	 * IT BELONGS HERE rather than inside one backend's `present` because A RENDERER MAY REPLACE `present` (the GL
	 * one does, through its swap) — so a drain bolted onto the CPU path would be missing from the path that most
	 * needs it. Called after `present` has returned, so a drain may not present again recursively. NULL for a
	 * backend with no such queue. */
	void (*drain)(struct aws_server *server);

	/* **GIVE THE DISPLAY UP AND TAKE IT BACK, WHICH IS NOT THE SAME AS STOPPING.** A virtual-terminal switch asks a
	 * compositor to let go of the screen without forgetting anything: the device stays open, the framebuffers stay
	 * allocated, the connector and mode stay chosen — only the claim on the device and the drawing stop. `stop` is
	 * the end of the backend's life and frees all of that; this pair is a pause. NULL for a backend whose display is
	 * a window somebody else owns, which is every backend but DRM. */
	void (*pause)(struct aws_server *server);
	void (*resume)(struct aws_server *server);

	/* GIVE BACK WHATEVER `start` TOOK. */
	void (*stop)(struct aws_server *server);

	/* **WHETHER THE POINTER IS THE COMPOSITOR'S TO DRAW.** A backend that shows its output inside somebody else's
	 * window system — the nested X11 one — already has a host pointer on the glass, and a second arrow composited
	 * into the window would be a pointer the person did not ask for. A DRM output has nothing of the sort: the only
	 * pixels are the ones this process scans out, so there the compositor draws the cursor itself (see cursor.c).
	 * Headless leaves it zero, which is also why its screenshots stay cursor-free. */
	int owns_cursor;

	/* **NONZERO WHILE THE BACKEND CANNOT ACCEPT A FRAME** — the display has not taken the last one yet. A RENDER
	 * LOOP ASKS THIS BEFORE IT DRAWS, because drawing into a buffer the display may still be reading is the tear
	 * a double-buffered backend exists to avoid. NULL means always ready. */
	int (*busy)(struct aws_server *server);

	void *data;				/* the backend's own state */
};

/* **THE DRM VOCABULARY A GPU BUFFER IS DESCRIBED IN, WRITTEN DOWN HERE RATHER THAN PULLED IN FROM libdrm.** A
 * format is a fourcc and a modifier is a 64-bit vendor code; `<drm_fourcc.h>` would give these names, but it would
 * also make a header-only dependency on libdrm's include path for a file that only spells them. */
#define AWS_FOURCC(a, b, c, d) \
	((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define AWS_DRM_FORMAT_XRGB8888 AWS_FOURCC('X', 'R', '2', '4')
#define AWS_DRM_FORMAT_ARGB8888 AWS_FOURCC('A', 'R', '2', '4')
#define AWS_DRM_FORMAT_XBGR8888 AWS_FOURCC('X', 'B', '2', '4')
#define AWS_DRM_FORMAT_ABGR8888 AWS_FOURCC('A', 'B', '2', '4')
#define AWS_DRM_FORMAT_RGBX8888 AWS_FOURCC('R', 'X', '2', '4')
#define AWS_DRM_FORMAT_RGBA8888 AWS_FOURCC('R', 'A', '2', '4')
#define AWS_DRM_FORMAT_BGRX8888 AWS_FOURCC('B', 'X', '2', '4')
#define AWS_DRM_FORMAT_BGRA8888 AWS_FOURCC('B', 'A', '2', '4')
#define AWS_DRM_FORMAT_RGB888   AWS_FOURCC('R', 'G', '2', '4')
#define AWS_DRM_FORMAT_BGR888   AWS_FOURCC('B', 'G', '2', '4')
#define AWS_DRM_FORMAT_MOD_LINEAR  0ULL
#define AWS_DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL

/* ONE PLANE OF A CLIENT'S GPU BUFFER. Almost every RGB buffer has exactly one; the array exists because the
 * protocol carries a list and a format this compositor accepts is required to fit in it. */
struct aws_dmabuf_plane {
	int fd;
	uint32_t offset;
	uint32_t stride;
	uint64_t modifier;
};

/* **A (FORMAT, MODIFIER) PAIR THE SERVER CAN ACTUALLY IMPORT.** This is what gets advertised, and it comes from the
 * renderer's EGL context rather than from a list written here: advertising a modifier the GPU cannot sample is a
 * client that allocates a buffer the compositor would then silently not draw. */
struct aws_dmabuf_format {
	uint32_t format;		/* an AWS_DRM_FORMAT_* fourcc */
	uint64_t modifier;
};

/* **A `wl_buffer` WHOSE PIXELS ARE THE CLIENT'S GPU MEMORY.** The protocol half (dmabuf.c) owns the fields the wire
 * carries; `render` is the renderer's own import handle — a texture, in the GL renderer — and the protocol code
 * never looks inside it. The fds are closed once the import has taken its own reference.
 *
 * `server` is here because the buffer's destroy handler has to tell the renderer to let go, and a `wl_buffer`
 * resource carries no server pointer of its own. */
struct aws_dmabuf_buffer {
	struct aws_server *server;
	struct wl_resource *resource;
	int32_t width;
	int32_t height;
	uint32_t format;
	int plane_count;
	struct aws_dmabuf_plane planes[4];
	void *render;
};

/* **A RENDERER TURNS THE SERVER'S STATE INTO PIXELS, AND THERE ARE TWO OF THEM.**
 *
 * The CPU one composites into `backend->pixels` — the code this server shipped with. The GL one draws the same
 * scene with EGL and GLES2 and never makes CPU pixels at all, which is why `draw` is the whole of the contract:
 * a renderer that owns the present (the GL one, through `backend.present`) has nothing left to hand up.
 *
 * THE COMPOSITE ORDER IS NOT A RENDERER'S. What is drawn, in what order, and which surfaces get a shadow is the
 * server's POLICY — levels, the menu bar, the release that keeps a client able to draw — so it lives once in
 * `aws_output_walk` and both renderers are walked by it. Two copies of a level walk is two places for a menu to
 * stop sitting above a window. */
struct aws_renderer {
	const char *name;
	/* EVERY FRAME, ONCE. Called from `aws_output_render`; the buffer release that follows is the server's. */
	void (*draw)(struct aws_server *server);
	void (*stop)(struct aws_server *server);
	/* **THE GPU BUFFERS THIS RENDERER CAN TAKE, AND HOW IT TAKES THEM.** A renderer that fills `formats` and answers a
	 * positive count is one that can sample a client's dma-buf, and only then is `zwp_linux_dmabuf_v1` advertised at
	 * all. The CPU renderer leaves all three NULL: it reads pixels, and there is no pixels to read in a GPU buffer —
	 * so a compositor running it promises nothing rather than promising a window it would never draw. */
	int (*dmabuf_formats)(struct aws_server *server, struct aws_dmabuf_format *formats, int max);
	int (*dmabuf_import)(struct aws_server *server, struct aws_dmabuf_buffer *buffer);
	void (*dmabuf_release)(struct aws_server *server, struct aws_dmabuf_buffer *buffer);
	void *data;				/* the renderer's own state */
};

/* THE OUTPUT GLOBAL'S OWN STATE, which is a wl_output resource and nothing else: the size belongs to the
 * backend, because the backend is what knows it. */
struct aws_output {
	struct wl_resource *resource;
};

/* **AN `agora_output` THE SERVER CAN SPEAK TO LATER.** The interface is event-only, so the resource has no
 * implementation struct to carry state — but the work area CHANGES now that clients reserve strips, and every
 * bound output has to hear it. This is the list's node. */
struct aws_agora_output {
	struct wl_resource *resource;
	struct wl_list link;
};

/* **AND A CLIENT THAT ASKED TO BE TOLD WHAT WINDOWS EXIST.** A Dock is the caller; the snapshot is re-sent on every
 * change, so this list is walked whenever anything about a window moves. Empty for every ordinary application,
 * which is what makes the broadcast free for them. */
struct aws_window_list {
	struct wl_resource *resource;
	struct wl_list link;
};

/* **AND ONE OUTSTANDING QUESTION ABOUT A MENU.** A Dock that right-clicks a tile asks the application behind it for
 * the items of its tile's menu, and the answer has to find its way back to the asker — so this is the asker's
 * `agora_dock_menu` resource, held under the WINDOW ID it named. The node lives exactly as long as the resource
 * does: `window` is set to zero once the answer has gone out, which is what makes a later answer to a stale
 * question a no-op rather than a second menu.
 *
 * **THE WINDOW ID IS THE KEY AND NOT A POINTER, WHICH IS THE WHOLE OF WHY THIS IS SAFE.** A surface may be
 * destroyed while a Dock is still waiting on it, and a node that held the surface would be walking freed memory
 * the next time it was looked at. An id is a number; a question about a window that has gone away is simply never
 * answered, which the asking client's timeout already covers. */
struct aws_dock_menu {
	struct wl_resource *resource;
	unsigned int window;		/* the id the ask named; 0 once it has been answered */
	struct wl_list link;
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
	/* **WHO TURNS THE SCENE INTO PIXELS.** Always set after a successful `aws_server_create`; the CPU renderer is
	 * what a server that has no GPU present gets, and a GL one is only ever chosen when a backend handed it a
	 * native window. */
	struct aws_renderer *renderer;
	/* **AND THE GPU BUFFERS IT SAID IT COULD TAKE.** The pair list is the renderer's answer, kept here because the
	 * protocol's bind handler has to send it to every client that asks. Empty on a server whose renderer cannot
	 * import, which is also the case in which no `zwp_linux_dmabuf_v1` global exists. */
	struct aws_dmabuf_format *dmabuf_formats;
	int dmabuf_format_count;
	struct aws_output output;
	/* Every bound `agora_output`, so a work-area change reaches all of them. */
	struct wl_list agora_outputs;
	/* Every bound `agora_window_list`, so a window change reaches the dock that asked. */
	struct wl_list window_lists;
	/* **EVERY BOUND `agora_shell`, BECAUSE THE SELECTION IS ANNOUNCED UNSOLICITED.** A window change has a client to
	 * tell; a selection change does not ask anybody first, so the list of bound shells is the only way to say it. */
	struct wl_list shell_bindings;
	/* **THE SELECTION ITSELF: ONE OWNER AND ITS TYPES.** The compositor keeps the claim and the vocabulary and never
	 * the data — a request for it turns into an fd between the two clients. See CLIPBOARD-PLAN.md. */
	struct wl_resource *selection_owner;
	char *selection_types[32];
	int selection_type_count;
	/* **AND THE MENUS A DOCK IS WAITING FOR.** One node per `get_dock_menu`, keyed by the window it asked about; see
	 * `struct aws_dock_menu`. Walked when an application answers, and by nothing else. */
	struct wl_list dock_menus;
	/* **THE NUMBER A WINDOW IS KNOWN BY OUTSIDE ITS OWN CLIENT.** A Dock holds no proxy for somebody else's
	 * window, so `activate_window` and `close_window` name one by this id — the server's, stable for the life of
	 * the window. Zero is never handed out, so zero can mean "no window". */
	unsigned int next_window_id;

	/* ---- THE SYSTEM MENU'S MARK. What the OS says it is, and the resolved icon object the bar draws. Detection
	 * is done once, in `server.c`; the override can replace the resolved pointer. ---- */
	struct aws_os_identity os_identity;
	struct aws_system_icon *system_menu_icon;

	/* ---- THE SEAT. Which surfaces have the pointer and the keyboard, where the pointer is, and the xkb state
	 * that turns a key into the MODIFIER STATE a client needs to translate it. ---- */
	/* **ONE NODE PER CLIENT THAT ASKED**, and the two element types are here rather than private to `seat.c`
	 * because the list member is here: a type declared on one side of that line and used on the other is how a
	 * definition and its user drift apart. */
	struct wl_list pointers;		/* struct aws_pointer, one per client that asked */
	struct wl_list keyboards;		/* struct aws_keyboard */
	struct aws_surface *pointer_focus;	/* the surface the pointer is over, or NULL */
	struct aws_surface *pointer_grab;	/* the surface that takes the pointer over NO surface, or NULL */
	struct aws_surface *pointer_capture;	/* the surface that takes the pointer WHEREVER it is, or NULL */
	uint32_t pointer_buttons;		/* the pointer's buttons that are down, as a bit per evdev code */
	struct aws_surface *keyboard_focus;	/* the surface the keyboard goes to, or NULL */
	int pointer_x;				/* the pointer, in OUTPUT coordinates */
	int pointer_y;
	int pointer_inside;			/* the pointer is inside the output at all */
	/* **THE FALLBACK CURSOR'S RASTER, KEPT FOR THE SCALE IT WAS MADE AT.** A DRM output has no host pointer, so the
	 * arrow in cursor.c is composited over every frame; the image is rasterized once per scale and reused, because
	 * the shape never changes and the raster is the only thing about a cursor that costs anything. */
	struct aws_cursor cursor;
	/* AN AXIS HAS BEEN SENT SINCE THE LAST FRAME, which is what makes `axis_source` once per frame rather than
	 * once per axis — the protocol asks for it in the frame the axis events belong to, and repeating it for the
	 * second axis of one gesture would be a lie about how many gestures there were. */
	int pointer_axis_frame_open;
	/* **THE SCROLL SEQUENCE'S OWN STATE, WHICH IS WHAT A `stop` IS ABOUT.** `axis_used` is a bit per axis that has
	 * moved since the last stop was sent; the timer is the thing that decides the sequence has ended, because an
	 * ending is an ABSENCE and no event announces it. Both belong to the seat and not to a backend: which axes are
	 * still moving is a fact about the seat's input, whoever fed it. */
	uint32_t axis_used;
	struct wl_event_source *axis_stop_timer;
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;

	int render_pending;
	int frames;
	int max_frames;
	const char *screenshot;
	int running;
	/* **WHILE THE DISPLAY IS SOMEBODY ELSE'S.** Set between a VT release and its acquire: the loop stops drawing and
	 * `present` refuses, so a frame composed during the switch cannot be scanned out onto a terminal this process no
	 * longer owns. */
	int suspended;
};

/* server.c */
/* `width` and `height` are LOGICAL pixels — the screen a client is told about — and `scale` is the device pixels per
 * logical pixel, where 0 means "whatever the backend can infer" (see `aws_backend_init`). */
struct aws_server *aws_server_create(const char *backend_name, const char *input_name, const char *renderer_name,
				     int width, int height, double scale);
void aws_server_destroy(struct aws_server *server);
void aws_server_schedule_render(struct aws_server *server);
/* `name` is an icon id, an alias, a distro id, a family, or "auto" to re-resolve from the identity. Returns 0 on
 * success and -1 for an unknown name. */
int aws_server_set_system_menu_icon(struct aws_server *server, const char *name);

/* surface.c — the globals a client draws through */
void aws_compositor_init(struct aws_server *server);
void aws_xdg_shell_init(struct aws_server *server);

/* output.c — the wl_output global, the composite ORDER, and the screenshot */
void aws_output_init(struct aws_server *server);
void aws_output_render(struct aws_server *server);
/* **THE COMPOSITE ORDER, IN ONE PLACE.** `emit` is called with a NULL surface when the menu bar is due, and once
 * for each surface that is actually on the screen, in the order they are painted — background clears are the
 * renderer's, and the buffer release after the walk is `aws_output_render`'s. A menu window's `frame_done` belongs
 * here because it is owed at composite time; the caller passes the frame's clock down through `time`. */
typedef void (*aws_output_emit_fn)(struct aws_server *server, struct aws_surface *surface, uint32_t time);
void aws_output_walk(struct aws_server *server, aws_output_emit_fn emit);
int aws_output_write_ppm(struct aws_server *server, const char *path);

/* renderer.c — choosing who draws, and the two calls the lifecycle makes */const char *aws_renderer_names(void);
int aws_renderer_init(struct aws_server *server, const char *name);
void aws_renderer_shutdown(struct aws_server *server);

/* EACH RENDERER ANSWERS 0 AND SETS `server->renderer`, or answers non-zero and says why on stderr. */
int aws_renderer_cpu_create(struct aws_server *server);
int aws_renderer_gl_create(struct aws_server *server);

/* dmabuf.c — `zwp_linux_dmabuf_v1`: the GPU buffers a client may hand over instead of pixels.
 *
 * **THE GLOBAL IS CREATED AFTER THE RENDERER, AND THAT ORDER IS THE CONTRACT.** What can be advertised is what the
 * active renderer can import, so a server with no GL context advertises nothing — and a client that binds the
 * global is therefore never told about a buffer that would be dropped on the floor. */
void aws_dmabuf_init(struct aws_server *server);
/* **THE BUFFER, IF IT IS ONE OF OURS.** NULL for a `wl_shm` buffer (whose user data belongs to libwayland) and for
 * anything else — which is what lets the two compositors ask "is this a dma-buf?" without knowing how the protocol
 * stores it. */
struct aws_dmabuf_buffer *aws_dmabuf_buffer_from_resource(struct wl_resource *buffer);
/* Whether a fourcc carries alpha, and how many planes this compositor accepts for it (0 = not accepted). */
int aws_dmabuf_format_has_alpha(uint32_t format);
int aws_dmabuf_format_planes(uint32_t format);

/* backend.c — choosing one, and the two calls the event loop makes */
int aws_backend_init(struct aws_server *server, const char *name, int width, int height, double scale);
void aws_backend_present(struct aws_server *server);

/* **THE VT SWITCH'S TWO HALVES, ROUTED TO THE BACKEND THAT HAS THEM.** Called from the compositor's own signal
 * handlers (registered on the event loop, never from async-signal context) so that a release is answered only once the
 * display has actually been given up. */
void aws_server_suspend(struct aws_server *server);
void aws_server_resume(struct aws_server *server);

/* **THE SCREEN THE CLIENT IS TOLD ABOUT, WHICH IS THE LOGICAL ONE.** Every coordinate this server handles above the
 * backends — a window's frame, a level, a hit test, the work area, the menu bar's height — is in logical pixels, and
 * these three are the only place the device framebuffer is divided down. Written as functions rather than as
 * `backend.width / backend.scale` at each use, because a missing division is invisible until something is drawn at
 * half size and there is no compile error for getting it wrong. */
int aws_output_width(const struct aws_server *server);
int aws_output_height(const struct aws_server *server);
/* The device pixels per logical pixel, as a fraction: what the renderers blit by. */
double aws_output_scale(const struct aws_server *server);
/* **AND THE SAME NUMBER AS A CLIENT MAY HEAR IT.** `wl_output.scale` is an integer, so a 1.5x output is advertised as
 * 2: a client that renders at 2x is composited 2 -> 1.5, which is sharp, while a client that rendered at 1.5 would not
 * be representable at all. Rounded UP, because rounding down would have the client render at fewer pixels than the
 * screen has. */
int aws_output_scale_integer(const struct aws_server *server);
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
/* **THE LEVEL THE BAR IS COMPOSITED AT, AND IT IS A LEVEL BECAUSE THE DESKTOP IS A WINDOW NOW.** The bar is
 * furniture the server owns, so it is drawn in the level walk before the surfaces at or above this level: the
 * DESKTOP (a very negative level) and ordinary windows (0) are painted first and the bar covers them, while the
 * application's own MENU window and the STATUS items — both at or above the main-menu level — are painted after
 * and sit IN the bar. Apple's `kCGMainMenuWindowLevel` is 24, the same number `NSWindow.h` states. */
#define AWS_MENU_BAR_LEVEL 24
/* **ARGENTUM'S PALETTE, AND THESE ARE ITS NUMBERS RATHER THAN ANYBODY'S TASTE IN THIS FILE.** Argentum is the
 * design language this desktop is drawn in, and every value below was MEASURED from its sample rather than chosen
 * here — the provenance is `ARGENTUM.md`, which records the mockup each one came from. A server that picked its own
 * greys is how a menu bar comes to differ from the controls drawn under it by four counts. */
#define AWS_MENU_BAR_COLOUR 0xFFEBEBEBu		/* Argentum: the bar's face, sampled 235,235,235 */
/* **AND THE DESKTOP'S FILL, WHICH BOTH RENDERERS CLEAR TO.** A distinct colour rather than black, because a black
 * background is indistinguishable from a client that painted black, and this has to be checkable from a
 * screenshot. It is aqua BY REQUEST: (168,222,217), `#A8DED9` — light and green-leaning, so it reads as water. */
#define AWS_BACKGROUND 0xFFA8DED9u

/* **THE SHADOW'S THREE NUMBERS, WHICH TWO RENDERERS NOW HAVE TO AGREE ON.** The DEPTH is how far it reaches from
 * the frame's edge; the ALPHA is what it starts at there; the RADIUS is the frame's own corner — the same corner
 * the window is clipped to, because a shadow and the shape that casts it are one shape. These are a COPY of the
 * client's `fn_BAR_RADIUS` in `libagora-app/NSWindow.m`, and the two have to be changed together; they are here
 * rather than in one renderer because the CPU compositor and the GL shader must fall off over the same pixels. */
#define AWS_SHADOW_DEPTH  10
#define AWS_SHADOW_ALPHA  120
#define AWS_SHADOW_RADIUS 6

/* **THE SYSTEM MENU, WHICH IS THE SERVER'S OWN.** A square at the left of the bar, and inside it the OS mark —
 * the hamburger on an unrecognised system, or one of the exact OS marks. The mark is vector data in a 16×16 box
 * (`systemicon.h`), rasterized once into coverage and blended here; its bars, curves and holes are no longer
 * arithmetic in this header. These numbers are the BOX and WHERE IT SITS, not the shape inside it. */
#define AWS_SYSTEM_MENU_WIDTH 28
#define AWS_SYSTEM_MENU_GLYPH_WIDTH 16
#define AWS_SYSTEM_MENU_GLYPH_HEIGHT 16
#define AWS_SYSTEM_MENU_GLYPH_TOP ((AWS_MENU_BAR_HEIGHT - AWS_SYSTEM_MENU_GLYPH_HEIGHT) / 2)
/* **THE MARK IS PLACED, NOT CENTRED.** The square is 28 wide and the mark 16, so CENTRING it leaves six pixels on
 * each side — and a person looking at the bar asked for more room ON THE LEFT, which is the side the bar begins at
 * and the side a hand comes from. Ten pixels puts the mark at 10..25, which leaves the SAME ten pixels between it
 * and an application's first menu (2 to the square's edge, then the 8-pixel gap below) — equal air on both sides,
 * which is what an eye is reading when it says one side looks cramped. IT IS A MARGIN AND NOT A SHIFT because the
 * number to change is the distance from the edge, not an offset from a centre nobody chose. */
#define AWS_SYSTEM_MENU_LEFT_MARGIN 10
#define AWS_SYSTEM_MENU_GLYPH_COLOUR 0xFF000000u	/* Argentum: the bar's ink, sampled pure black */

/* **AND THE AIR BETWEEN IT AND AN APPLICATION'S OWN MENUS, WHICH IS THE BAR'S AND NOT THE SQUARE'S.** The square is
 * the System menu's — it is what a click on it hits — and widening it to make room would move the GLYPH with it,
 * because the mark is centred in its square. The gap is a separate thing: the strip's own spacing, which the server
 * owns because the server owns the strip, and which a client cannot guess any more than it can guess the square's
 * width. It is part of the region `menu_bar_area` names, so the application's menus move right by exactly this much
 * and nothing on the client side changes. */
#define AWS_SYSTEM_MENU_GAP 8

/* shell.c — agora_shell: the frames, the levels and the work area the Mac's model needs. */
void aws_shell_init(struct aws_server *server);
/* **THE WORK AREA, AS OF NOW.** The output minus the menu bar the server owns and minus every strip a client
 * reserved — which is `NSScreen.visibleFrame`, and the answer `constrainFrameRect:toScreen:` is checked against. */
void aws_shell_reserved(struct aws_server *server, int *x, int *y, int *width, int *height);
/* **AND TELLING EVERY CLIENT IT MOVED.** `set_reserved` is the only thing that changes it at runtime, so this is
 * called from there and from the two places a reservation dies (the role destroyed, the surface gone). It sends
 * `agora_output.work_area` to every bound output and `agora_window.work_area_changed` to every window. */
void aws_shell_reserved_changed(struct aws_server *server);
/* **AND THE RELEASE, WHICH IS THE SAME NO MATTER HOW IT HAPPENED.** A surface that never reserved anything is left
 * alone, so nothing is announced for a window that never asked. */
void aws_shell_release_reserve(struct aws_surface *surface);
/* **AND TELLING THE DOCK WHAT IS ON THE SCREEN.** A snapshot — `begin`, one `window` per window, `done` — sent to
 * every client that asked for one. Called from every door that changes what the list would say: a window mapped or
 * gone, a title or an application id, a level, a visibility, and the keyboard moving. */
void aws_shell_window_list_changed(struct aws_server *server);
/* **WHERE AN APPLICATION'S OWN MENUS MAY BEGIN** — the strip minus the System menu's square. The one number about
 * the bar that a client cannot derive, because the System menu's width is the server's. See `agora-shell.xml`. */
void aws_shell_menu_bar_area(struct aws_server *server, int *x, int *y, int *width, int *height);

/* The packing of the status area, which a client adding or removing an item has just invalidated. */
void aws_shell_place_status_items(struct aws_server *server);

/* **ONE NODE PER BOUND `agora_shell`, AND IT OWNS NOTHING BUT ITS PLACE IN THE LIST.** The selection is announced
 * unsolicited, so the list is the only way to reach a client that never asked; the resource is kept beside it because a
 * broadcast has to name one. */
struct aws_shell_binding {
	struct aws_server *server;
	struct wl_resource *resource;
	struct wl_list link;
};

struct aws_pointer {
	struct wl_resource *resource;
	struct wl_list link;
};

struct aws_keyboard {
	struct wl_resource *resource;
	struct wl_list link;
};

/* seat.c — the seat: the devices, the focus, and the keymap. */
void aws_seat_init(struct aws_server *server);
void aws_seat_finish(struct aws_server *server);

/* THE FOUR THINGS A BACKEND CAN SAY ABOUT INPUT. Each is what an input device would have reported, and the
 * only caller is a backend — which is what keeps "where input comes from" out of everything above. */
void aws_seat_pointer_enter(struct aws_server *server, int x, int y);
void aws_seat_pointer_leave(struct aws_server *server);
void aws_seat_pointer_motion(struct aws_server *server, int x, int y);
void aws_seat_pointer_button(struct aws_server *server, uint32_t evdev_button, int pressed);
/* **THE WHEEL, WHICH IS A MOTION THAT CARRIES A DISTANCE.** A scroll is one axis of one frame: `axis` is
 * `WL_POINTER_AXIS_*`, `value` the precise distance in that axis's own units, `source` where the motion came from
 * (a wheel is not a trackpad is not a continuous device), and the two discrete numbers are the SAME distance said
 * in whole steps — `discrete` for `wl_pointer.axis_discrete` (v5-v7) and `value120` for `wl_pointer.axis_value120`
 * (v8+). Exactly one of those two goes on the wire, chosen by the client's bound version, which is what keeps a
 * wheel tick from being counted twice.
 *
 * `aws_seat_pointer_frame` closes the batch: `wl_pointer.frame` is the protocol's "these belong together", and a
 * backend that sends two axes in one gesture calls it once. Clients below version 5 have no frame at all and are
 * sent the axis events alone, exactly as the protocol says. */
void aws_seat_pointer_axis(struct aws_server *server, uint32_t axis, wl_fixed_t value, uint32_t source,
			   int32_t discrete, int32_t value120);
void aws_seat_pointer_frame(struct aws_server *server);
/* **"THE SCROLL HAS ENDED", WHICH THE PROTOCOL CANNOT SAY WITH AN EVENT.** `axis_stop` is how a client learns that a
 * continuous or finger gesture is over — the thing a kinetic scroll animation waits for. It is sent by the seat's own
 * timer once the axes have been quiet for `AWS_AXIS_STOP_MS`, and this door is that timer's body, public so a test can
 * drive it without waiting: `tests/pointer_axis.c` calls it directly. A wheel does not use it (a notch is discrete and
 * has no sequence to end). */
void aws_seat_pointer_axis_stop(struct aws_server *server);
/* **RE-DECIDE WHAT THE POINTER IS OVER, WITHOUT WAITING FOR IT TO MOVE.** What is under the pointer changes when
 * a surface is mapped, unmapped, moved, shown, hidden or restacked — none of which is a motion — so every one of
 * those doors calls this. Without it a surface that appears under a stationary pointer never gets the
 * `wl_pointer.enter` the protocol owes it, and the first click aimed at it goes to nobody: a click that does
 * nothing until the mouse is moved. */
void aws_seat_pointer_recheck(struct aws_server *server);
/* **WHERE A POINTER EVENT GOES, GIVEN WHAT IS UNDER THE POINTER — one answer for motion, buttons and the
 * recheck, because three copies of this precedence is three ways for a drag to starve. Exposed so a test can ask
 * it without a Wayland connection: `tests/seat_routing.c` is that test. */
struct aws_surface *aws_seat_pointer_target(struct aws_server *server, struct aws_surface *under);
/* **A SURFACE IS GOING AWAY, SO FORGET IT BEFORE IT IS FREED.** It is called from the surface's own destroy
 * handler, where sending a `leave` is not an option: the `wl_resource` it would name is the one being
 * destroyed. Clearing the focus here is what keeps `pointer_focus`/`keyboard_focus` from pointing at freed
 * memory — and a recheck afterwards lets whatever is now under the pointer take the focus. */
void aws_seat_surface_gone(struct aws_server *server, struct aws_surface *surface);
/* **THERE IS ALWAYS A KEY APPLICATION, WHICH IS TO SAY THE KEYBOARD IS NEVER LEFT ON NOBODY WHILE A WINDOW EXISTS.**
 * A desktop with no key window can offer no menu bar, no key equivalents and nowhere for a keystroke to go — so
 * this re-decides the focus whenever the surface that had it WENT AWAY or was HIDDEN, picking the topmost window
 * that is left. It is the server's policy, not a client's: a client cannot see the other clients' windows. */
void aws_seat_reassign_keyboard_focus(struct aws_server *server);
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

/* **THE FRACTIONAL BUFFER SCALE, FROM `agora_window.set_buffer_scale_fixed`.** Double-buffered exactly like
 * `wl_surface.set_buffer_scale`: it lands on the next commit, together with the buffer it describes. See
 * `struct aws_surface`'s `buffer_scale` and `aws_scale_quantized` for why the value is a double and where it came
 * from. */
void aws_surface_set_buffer_scale_fixed(struct aws_surface *surface, double scale);

/* **THE POINTER'S ABSOLUTE POSITION, TO THE WINDOW'S OWN CLIENT.** `wl_pointer.motion` reports the pointer RELATIVE
 * to the surface, which a window being MOVED turns into a moving target — so the absolute position is sent
 * alongside it (the protocol's `pointer_global`), and a client-side drag can hold a grabbed point under the
 * pointer without depending on where the window is. A no-op for a surface with no agora window. */
void aws_window_send_pointer_global(struct aws_surface *surface, int x, int y);

#endif /* AGORA_WINDOW_SERVER_H */
