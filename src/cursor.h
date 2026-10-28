/*
 * cursor.h — the pointer the compositor draws itself.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **WHY THE COMPOSITOR OWNS A CURSOR AT ALL.** A nested or windowed backend has a host pointer already on the
 * glass; a DRM output has nothing but the pixels this process scans out, so a compositor with no cursor code has a
 * desktop with no visible pointer — which is working input and an invisible hand. This is the FALLBACK pointer: it
 * is shown wherever a client has not supplied one, and it is a compositor-owned image rather than a plane because
 * it has to be visible over every client surface, including an empty desktop, and only a composited quad is.
 *
 * **AND ITS PIXELS ARE A THEME'S, NOT A POLYGON'S.** The arrow used to be drawn here from its own points, which made
 * a session's pointer Agora's idea of an arrow instead of the one the desktop was designed with. It now comes from an
 * Xcursor theme (`cursor-theme.h`) — the vendored Vimix theme by default — selected by the output's own scale and
 * carrying the theme's hotspot, and the two display paths get their pixels from the same call:
 *
 *   - a DRM output composites the image every frame (`aws_cursor_draw_cpu`, or the GL renderer's textured quad);
 *   - a nested X11 output hands the same image to the HOST, which draws the pointer inside this process's window
 *     (`aws_cursor_theme_pointer` and `backend.apply_cursor`). The host still draws it — a second pointer composited
 *     into a window that already has one is a pointer the person did not ask for — but what it draws is the theme's.
 *
 * The drawn arrow is still here, as the fallback when no theme is installed or the theme has no `left_ptr`. It is
 * original to Agora, so a checkout without the GPL theme's artwork is still a desktop with a visible pointer.
 *
 * WHAT IT IS NOT: an implementation of `wl_pointer.set_cursor`. A client's cursor surface is still accepted and
 * ignored (see `pointer_set_cursor` in seat.c) — that is the protocol feature, and it carries the client's own PIXELS.
 * WHAT THIS FILE DOES IMPLEMENT IS THE OTHER ANSWER, `agora_window.set_cursor`: a NAME, resolved against the theme that
 * is already loaded here, kept per window and applied when the window under the pointer changes (see
 * `aws_cursor_follow_focus`). A compositor-owned pointer with a shape the window names is the whole of it; a client's
 * own bitmap is not, and nothing above this line pretends otherwise.
 */

#ifndef AGORA_WINDOW_SERVER_CURSOR_H
#define AGORA_WINDOW_SERVER_CURSOR_H

#include <stdint.h>

#include "cursor-theme.h"

struct aws_server;

/* THE SHAPE THE COMPOSITOR PUTS UNDER THE POINTER, and the size it is asked for. `left_ptr` is the Xcursor name every
 * theme gives its arrow — the name a client's cursor shape would carry too — and 24 points is the theme's own design
 * size: its 24-nominal artwork is 32 pixels, which is 24 points on a 96-dpi panel. */
#define AWS_CURSOR_ARROW_NAME "left_ptr"
#define AWS_CURSOR_DEFAULT_SIZE 24.0
/* **THE ONE SHAPE THAT IS NOT A THEME ENTRY.** `agora_window.set_cursor`'s `none` means the pointer is not drawn at all —
 * the answer for a video player, or a page that paints its own pointer — and no freedesktop theme carries a name for it,
 * because an invisible cursor is not artwork. The compositor makes one: a single transparent pixel, which blends to
 * nothing over every background and needs no second code path in either renderer. */
#define AWS_CURSOR_NONE_NAME "none"
/* THE LONGEST SHAPE NAME KEPT. Every name the Xcursor vocabulary has is far shorter, so a longer one is a client sending
 * something that cannot be a theme entry; it is treated as a name the theme does not have rather than truncated into a
 * DIFFERENT name that happens to exist. */
#define AWS_CURSOR_NAME_MAX 64
/* THE LARGEST SIZE A SESSION MAY ASK FOR. The vendored theme's biggest arrow is 64 pixels; a size beyond this one is a
 * number the artwork cannot honour (the largest variant would be used anyway), so it is refused where it is set rather
 * than silently capped. */
#define AWS_MAX_CURSOR_SIZE 256.0

/* THE IMAGE, KEPT FOR ONE SCALE AND ONE THEME SETTING. `pixels` is 0xAARRGGBB with straight (non-premultiplied)
 * alpha, which is what both renderers already blend. `hotspot` is the point of the IMAGE that sits on the pointer's
 * own position, in DEVICE pixels. `generation` numbers THIS IMAGE — a fresh number every time one is made, whether
 * because the scale or the theme changed — so a renderer that caches it as a texture can tell a re-made image from the
 * one it already has. The server's `cursor_generation` is the matching counter: equal while the image is current,
 * different once a setting has moved. */
struct aws_cursor {
	double scale;			/* the scale `pixels` was made for; 0 until there is an image */
	unsigned generation;		/* this image's own number; see the note above */
	int width;
	int height;
	int hotspot_x;
	int hotspot_y;
	uint32_t *pixels;
};

/* EVERY ANSWER BELOW IS THE SERVER'S, AND EVERY ONE OF THEM IS SAFE TO CALL WHEN THERE IS NOTHING TO DRAW. */

/* Whether a pointer belongs on the glass at all: the backend has no host cursor of its own AND the pointer is on
 * the output. Asked by both renderers, so the two agree about when a cursor exists. */
int aws_cursor_visible(struct aws_server *server);

/* Make the image for the output's current scale and the server's current theme, if the cached one is for anything
 * else. The theme's variant is chosen for `cursor_size × scale` device pixels, and the drawn arrow is made when there
 * is no theme to ask. */
void aws_cursor_ensure(struct aws_server *server);

/* Draw the POINTER OVER THE FRAME, which is the caller's last act after the scene has been painted. Device-pixel
 * placement, fractional-scale rounding and clipping to the output are all here, so the CPU and GL paths cannot
 * disagree about where the pointer is. */
void aws_cursor_draw_cpu(struct aws_server *server);

/* **AND THE SAME DRAW, CLIPPED TO THE FRAME'S BOX.** `box` is in device pixels (top-down) or NULL for the whole output;
 * the pointer is not exempt from a partial frame, and the rows and columns outside the box are the caller's — not the
 * pointer's — to leave alone. `aws_cursor_draw_cpu` is this with NULL, so the whole-frame callers and the test that
 * asserts the placement arithmetic keep the same door. */
struct aws_box;
void aws_cursor_draw_cpu_box(struct aws_server *server, const struct aws_box *box);

/* **THE RECTANGLE THE POINTER OCCUPIES AT A GIVEN LOGICAL POSITION, IN DEVICE PIXELS.** Answers 0 when this backend
 * does not draw a pointer or has no image to draw, and 1 with `box` filled otherwise. The seat asks it for the OLD
 * position as well as the new one: the pixels where the pointer was are not a rectangle any client damaged, and a
 * frame that repaints both boxes is what makes a moving pointer cost a pointer instead of a screen. It is the same
 * placement arithmetic `aws_cursor_draw_cpu_box` uses, asked without drawing — two copies would be two answers. */
int aws_cursor_device_box(struct aws_server *server, int x, int y, struct aws_box *box);

/* The image, for a renderer that uploads it as a texture rather than blending it. NULL when there is nothing to
 * draw. */
const struct aws_cursor *aws_cursor_image(struct aws_server *server);

/* **THE THEME'S POINTER ITSELF, FOR A DISPLAY THAT DRAWS IT RATHER THAN HAVING IT BLENDED.** The nested X11 backend
 * makes a host cursor out of this: `pixels` is straight ARGB, `hotspot` is in the image's own pixels, and the variant
 * is the same one the DRM path would composite at this output's scale — so a nested session and a bare-metal one show
 * the same theme, at the same size, with the same tip. NULL when there is no theme: the host keeps its own cursor
 * rather than being handed one Agora drew. */
const struct aws_cursor_image *aws_cursor_theme_pointer(struct aws_server *server);

/* Give the raster back. Called from the server's own destruction. */
void aws_cursor_finish(struct aws_server *server);

/* ==================================================================================================
 * THE POINTER'S SHAPE, WHICH IS THE WINDOW'S TO NAME
 * ================================================================================================== */

/* **PUT THE POINTER'S SHAPE INTO FORCE, BY NAME.** `name` is an Xcursor name (`text`, `pointer`, `nwse-resize`) resolved
 * against the loaded theme at the output's own scale; NULL or the empty string means the default (`left_ptr`), and
 * `AWS_CURSOR_NONE_NAME` means the pointer is not drawn. A name the theme does not have is the DEFAULT — a theme that
 * carries `left_ptr` but not `help` still shows its own arrow — and `aws_cursor_report_shape` is where that is said out
 * loud, at the request that carried the name.
 *
 * A NAME THAT HAS NOT CHANGED DOES NOTHING AT ALL: the composited image is re-made only when the shape, the scale or the
 * theme actually moved, and a backend whose host draws the pointer is told to re-apply it only then. That is what makes
 * this a change-of-shape door rather than a per-motion one. */
void aws_cursor_set_shape(struct aws_server *server, const char *name);

/* **THE SHAPE THE WINDOW UNDER THE POINTER ASKS FOR.** `agora_window.set_cursor` stores a name per window
 * (`struct aws_surface`'s `cursor_name`) because a pointer's shape belongs to what it is over, so this is what both the
 * seat calls when the focus moves and the request handler calls when a window changes its own mind. A pointer over no
 * window — the desktop — takes the default, and so does a window that never asked. */
void aws_cursor_follow_focus(struct aws_server *server);

/* THE NAME IN FORCE, for a diagnostic that has to say what the pointer is rather than what a window asked for. Never
 * NULL: the default's name when nothing has been set. */
const char *aws_cursor_shape(const struct aws_server *server);

/* **WHAT A REQUESTED NAME RESOLVES TO, SAID OUT LOUD — ONCE, WHERE IT WAS ASKED FOR.** A nested session's pointer is drawn
 * by the HOST, so no frame this process writes contains it and no screenshot can show that `agora_window.set_cursor`
 * reached the theme; `AWS_CURSOR_DIAG` is the only instrument. Reporting it here — at the request, with the window's own
 * number — is also what lets the test run HEADLESS: the claim being checked is "the server resolved this window's name",
 * which needs no display at all, and the host's half is `backend-x11.c`'s own line. A name the theme does not have says so
 * in as many words, which is the difference between a fallback and a silent substitution. */
void aws_cursor_report_shape(struct aws_server *server, int window_id, const char *name);

#endif /* AGORA_WINDOW_SERVER_CURSOR_H */
