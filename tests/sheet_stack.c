/*
 * sheet_stack — is a sheet always exactly one layer above its parent?
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **THE BUG THIS FILE EXISTS FOR WAS REPORTED AS "it should be impossible for another window to be stacked between a
 * window and its sheets".** `set_sheet_parent` made the pair adjacent when it was made — but adjacency in the LIST is
 * not adjacency on the GLASS, and every door that reorders the list can break it: `-orderFront:` on the sheet moves the
 * sheet to the top of the stack on its own, raising a third window moves that one above both, and an `NSAlert`'s panel
 * is at `NSModalPanelWindowLevel` while the document is at 0 — so the pair was two windows with a level between them as
 * well as two nodes.
 *
 * **SO THE ORDER IS DERIVED RATHER THAN MAINTAINED, AND THIS FILE ASKS THE DERIVATION.** `../src/output.c` is compiled
 * in as it ships and `aws_output_walk` is called with an emitter that records what it is handed; the stacking is then a
 * LIST of ids that can be read. What is asserted:
 *
 *   1. THE PAIR IS ADJACENT whatever the list says: the three orders that used to break it — the sheet at the tail
 *      (raised on its own), a third window between them in the list, and the sheet at a HIGHER LEVEL than its parent —
 *      all draw parent then sheet, with nothing between.
 *   2. A SHEET IS NOT DRAWN TWICE. Its own level pass does not draw it (it has no place of its own), which is why the
 *      walk's level finder skips it as well as its inner loop.
 *   3. AND A SHEET WHOSE PARENT IS NOT ON THE SCREEN IS AN ORDINARY WINDOW: hidden parent, sheet drawn in its own pass
 *      — the predicate is "drawn with the parent", and a parent nobody can see has nothing to be drawn with.
 *   4. THE BAR IS STILL ABOVE A PAIR THAT IS BELOW IT, which is the one thing an order that puts the sheet at its
 *      parent's level must not lose: a sheet at `NSModalPanelWindowLevel` belongs under the menu bar like the window it
 *      hangs from.
 *
 * **THE HIT TEST'S HALF IS THE SAME PREDICATE**, which is why one test can stand for both: `aws_surface_at` skips a
 * sheet in its own pass and asks the parent for it first of all (`sheet_at_point`), so the pointer meets the pair in the
 * order the walk drew them. `tests/seat_routing.c` is what sits on top of that answer.
 *
 * IT IS A UNIT TEST AND NOT A SCREENSHOT because the claim is one function's order and nothing about pixels — the same
 * reason `seat_routing.c` exists. `output.c`'s other half (the frame and the screenshot) needs a renderer and a
 * backend, so the few symbols this file does not exercise are stubbed here and named as stubs.
 */
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server.h>

/* ============================================================================================ *
 * THE SYMBOLS THIS FILE DOES NOT EXERCISE.                                                     *
 *                                                                                                *
 * `output.c` ships the frame path and the screenshot beside the walk, and those need a renderer *
 * and a backend that a unit test has no business making. The walk itself asks for NOTHING but   *
 * the surface list — which is the point of the split this file is testing.                       *
 * ============================================================================================ */
void aws_server_schedule_render(struct aws_server *server)
{
	(void)server;
}

/* AND THE CALLBACK A SKIPPED FRAME COMPLETES, which the frame path beside the walk now names. Nothing here asks for a
 * frame, so the stub does nothing — but the symbol has to exist for the file to link. */
void aws_surface_frame_done(struct aws_surface *surface, uint32_t time)
{
	(void)surface;
	(void)time;
}

/* AND THE OUTPUT'S THREE FACTS, WHICH THE FRAME PATH READS AND THE WALK DOES NOT. A zero-size output is the honest
 * answer for a test that never composites; nothing in `aws_output_walk` asks for either. */
int aws_output_width(const struct aws_server *server)
{
	(void)server;
	return 0;
}

int aws_output_height(const struct aws_server *server)
{
	(void)server;
	return 0;
}

int aws_output_scale_integer(const struct aws_server *server)
{
	(void)server;
	return 1;
}

/* THE COMPOSITE ORDER'S EMITTER IS CALLED WITH A NULL SURFACE FOR THE MENU BAR, so the recorder keeps two sequences:
 * the surfaces, in the order they were drawn, and where the bar went among them. */
#define MAX_SURFACES 16

static struct aws_surface *s_drawn[MAX_SURFACES];
static int s_drawn_count;
static int s_bar_at;			/* index in the sequence, or -1 */

static void record(struct aws_server *server, struct aws_surface *surface, uint32_t time,
		   const struct aws_box *box)
{
	(void)server;
	(void)time;
	/* **THE ORDER IS WHAT THIS TEST IS ABOUT, SO THE BOX IS IGNORED.** A sheet's adjacency is a claim about the sequence
	 * whatever part of the output the frame happens to repaint — which is exactly why the walk visits every surface and
	 * leaves the box to the emitter (DAMAGE-PLAN.md §6.1). */
	(void)box;
	if (surface == NULL) {
		s_bar_at = s_drawn_count;
		return;
	}
	if (s_drawn_count < MAX_SURFACES) {
		s_drawn[s_drawn_count++] = surface;
	}
}

static int s_failed;

static void check(int ok, const char *what)
{
	printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		s_failed++;
	}
}

/* A WINDOW, AS FAR AS THE ORDER IS CONCERNED: a level, the three flags, and a place in the list. The buffer is a
 * non-NULL address that is never dereferenced — `surface->current_buffer` is only ever COMPARED by the walk (and by
 * `aws_surface_drawn_with_parent`), which is the same shortcut `seat_routing.c` takes with surfaces themselves. */
static struct wl_resource s_buffer;

static void window(struct aws_surface *surface, int level)
{
	memset(surface, 0, sizeof *surface);
	surface->level = level;
	surface->mapped = 1;
	surface->visible = 1;
	surface->current_buffer = &s_buffer;
	wl_list_init(&surface->link);
	wl_list_init(&surface->frame_callbacks);
}

/* THE LIST IS THE STACK: head is the bottom, tail is the top. Laters appended. */
static void stack(struct aws_server *server, struct aws_surface *surface)
{
	wl_list_remove(&surface->link);
	wl_list_insert(server->surfaces.prev, &surface->link);
}

static void reset(struct aws_server *server)
{
	memset(server, 0, sizeof *server);
	wl_list_init(&server->surfaces);
	s_drawn_count = 0;
	s_bar_at = -1;
}

static int index_of(struct aws_surface *surface)
{
	int i;

	for (i = 0; i < s_drawn_count; i++) {
		if (s_drawn[i] == surface) {
			return i;
		}
	}
	return -1;
}

/* THE INVARIANT ITSELF, ASKED OF A SEQUENCE THAT HAS ALREADY BEEN RECORDED: the sheet is at the parent's index + 1 and
 * appears exactly once. */
static int pair_is_adjacent(struct aws_surface *parent, struct aws_surface *sheet)
{
	int p = index_of(parent);
	int s = index_of(sheet);
	int i, seen = 0;

	for (i = 0; i < s_drawn_count; i++) {
		if (s_drawn[i] == sheet) {
			seen++;
		}
	}
	return p >= 0 && s == p + 1 && seen == 1;
}

int main(void)
{
	struct aws_server server;
	struct aws_surface document, sheet, other, dockish;

	setvbuf(stdout, NULL, _IONBF, 0);

	printf("1. the pair is adjacent whatever the list says\n");
	{
		/* THE SHEET AT THE TAIL — what `[sheet orderFront:nil]` leaves in the list, and the order that started
		 * this: bottom→top is document, other, sheet, and the sheet must still be drawn immediately above the
		 * document. */
		reset(&server);
		window(&document, 0);
		window(&sheet, 0);
		window(&other, 0);
		stack(&server, &document);
		stack(&server, &other);
		stack(&server, &sheet);
		sheet.sheet_parent = &document;
		aws_output_walk(&server, record, NULL);
		check(pair_is_adjacent(&document, &sheet),
		      "a sheet raised on its own is STILL drawn immediately above its parent");
		check(index_of(&other) > index_of(&sheet),
		      "and the window that was above the parent in the list is drawn above the PAIR");
	}
	{
		/* THE THIRD WINDOW BETWEEN THEM IN THE LIST, which is what raising it after the pair left behind. */
		reset(&server);
		window(&document, 0);
		window(&sheet, 0);
		window(&other, 0);
		stack(&server, &document);
		stack(&server, &other);
		stack(&server, &sheet);
		sheet.sheet_parent = &document;
		other.level = 0;
		stack(&server, &other);		/* raising `other` puts it at the tail… */
		wl_list_remove(&other.link);	/* …and this puts it back in the middle, which is the state under test */
		wl_list_insert(document.link.next, &other.link);
		aws_output_walk(&server, record, NULL);
		check(pair_is_adjacent(&document, &sheet),
		      "a window interposed between them in the LIST cannot be drawn between them");
	}
	{
		/* AND A SHEET AT A HIGHER LEVEL THAN ITS PARENT — the real `NSAlert` case: the panel is at
		 * `NSModalPanelWindowLevel` and the document is at 0. */
		reset(&server);
		window(&document, 0);
		window(&sheet, 8);
		window(&other, 3);
		stack(&server, &document);
		stack(&server, &other);
		stack(&server, &sheet);
		sheet.sheet_parent = &document;
		aws_output_walk(&server, record, NULL);
		check(pair_is_adjacent(&document, &sheet),
		      "a sheet at a HIGHER LEVEL is drawn with its parent and not at its own level");
		check(index_of(&other) > index_of(&sheet),
		      "so a window at a level BETWEEN the two is above the pair, never inside it");
	}

	printf("2. and it is drawn once\n");
	{
		reset(&server);
		window(&document, 0);
		window(&sheet, 8);
		stack(&server, &document);
		stack(&server, &sheet);
		sheet.sheet_parent = &document;
		aws_output_walk(&server, record, NULL);
		check(s_drawn_count == 2, "two windows make two draws — the sheet's own level pass draws nothing");
		check(index_of(&sheet) == 1, "with the sheet the one above its parent");
	}

	printf("3. a sheet whose parent is not on the screen is an ordinary window\n");
	{
		reset(&server);
		window(&document, 0);
		window(&sheet, 0);
		window(&other, 0);
		stack(&server, &document);
		stack(&server, &other);
		stack(&server, &sheet);
		sheet.sheet_parent = &document;
		document.visible = 0;		/* the parent is ordered out */
		aws_output_walk(&server, record, NULL);
		check(index_of(&document) < 0, "a hidden parent is not drawn");
		check(index_of(&sheet) == s_drawn_count - 1,
		      "and its sheet is drawn in its own place, at the top of the stack, rather than nowhere");
	}

	printf("4. the menu bar is still above the pair\n");
	{
		/* A PAIR AT LEVEL 0 DRAWING A SHEET THAT WAS GIVEN A HIGH LEVEL, AND A BAR WINDOW AT THE BAR'S LEVEL: the bar
		 * must be drawn after both, which is what makes the sheet sit under the strip rather than in it. */
		reset(&server);
		window(&document, 0);
		window(&sheet, 8);
		window(&dockish, AWS_MENU_BAR_LEVEL);
		stack(&server, &document);
		stack(&server, &dockish);
		stack(&server, &sheet);
		sheet.sheet_parent = &document;
		aws_output_walk(&server, record, NULL);
		check(s_bar_at >= 0, "the bar was emitted");
		check(s_bar_at >= 0 && index_of(&document) < s_bar_at && index_of(&sheet) < s_bar_at,
		      "and it is emitted AFTER both halves of the pair, so a sheet cannot cover it");
	}

	printf("sheet_stack: %s\n", s_failed == 0 ? "all checks passed" : "FAILURES");
	return s_failed == 0 ? 0 : 1;
}
