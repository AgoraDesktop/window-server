/*
 * systemicon.c — the resolver behind the System menu's mark.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * THE LADDER IS THE WHOLE OF THIS FILE: exact ID, alias, each `ID_LIKE` token in order, family, then the
 * hamburger. The tables are generated from `assets/system-menu-icons/`; this file contains the order and nothing
 * about any particular distro, so adding a distro is a TSV row and adding an icon is a generated entry.
 *
 * THE RASTERIZER IS THE OTHER HALF. A mark's paths are filled even-odd or nonzero into a coverage grid, once per
 * size, on first use: the 16×16 logical grid for 1x and a device grid for every scale an output asks for. The grid
 * is what the compositor blends; it never asks what the mark means, and it never rasterizes a frame. The hamburger
 * and the exact OS marks all use the same path format.
 */

#include "systemicon.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osdetect.h"
#include "systemicon-data.h"
#include "systemicon-table.h"

static struct aws_system_icon s_fallback = {
	AWS_SYSTEM_MENU_ICON_FALLBACK,
	aws_icon_hamburger_paths,
	sizeof(aws_icon_hamburger_paths) / sizeof(aws_icon_hamburger_paths[0]),
	AWS_ICON_FILL_EVEN_ODD,
	{ 0 },
	0,
	NULL,
	0,
};

static void normalize_token(const char *value, char *destination, size_t capacity)
{
	size_t used = 0;
	char quote = '\0';

	if (capacity == 0) {
		return;
	}
	while (*value != '\0' && isspace((unsigned char)*value)) {
		value++;
	}
	if (*value == '"' || *value == '\'') {
		quote = *value++;
	}
	while (*value != '\0') {
		unsigned char character = (unsigned char)*value++;

		if (quote != '\0') {
			if (character == (unsigned char)quote) {
				break;
			}
		} else if (isspace(character)) {
			break;
		}
		if (character >= 'A' && character <= 'Z') {
			character = (unsigned char)(character - 'A' + 'a');
		}
		if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
		    character == '.' || character == '_' || character == '-') {
			if (used + 1 < capacity) {
				destination[used++] = (char)character;
			}
		}
	}
	destination[used] = '\0';
}

/* A SPACE-SEPARATED LIST WITHOUT `strtok_r`, which is POSIX and not declared by a strict C11 compile. The
 * caller owns the buffer; this only writes NULs into the spaces it walks past. */
static char *next_token(char **cursor)
{
	char *token = *cursor;
	char *end;

	if (token == NULL) {
		return NULL;
	}
	while (*token == ' ') {
		token++;
	}
	if (*token == '\0') {
		*cursor = token;
		return NULL;
	}
	end = token;
	while (*end != '\0' && *end != ' ') {
		end++;
	}
	if (*end == '\0') {
		*cursor = end;
	} else {
		*end = '\0';
		*cursor = end + 1;
	}
	return token;
}

static const struct aws_distro_entry *find_distro(const char *id)
{
	size_t i;

	for (i = 0; i < aws_distro_entry_count; i++) {
		if (strcmp(aws_distro_entries[i].id, id) == 0) {
			return &aws_distro_entries[i];
		}
	}
	return NULL;
}

static const struct aws_alias_entry *find_alias(const char *alias)
{
	size_t i;

	for (i = 0; i < aws_alias_entry_count; i++) {
		if (strcmp(aws_alias_entries[i].alias, alias) == 0) {
			return &aws_alias_entries[i];
		}
	}
	return NULL;
}

static const struct aws_family_entry *find_family(const char *family)
{
	size_t i;

	for (i = 0; i < aws_family_entry_count; i++) {
		if (strcmp(aws_family_entries[i].family, family) == 0) {
			return &aws_family_entries[i];
		}
	}
	return NULL;
}

/* A TOKEN WITH AN EXACT MARK, AN ALIAS, OR A DISTRO ROW CARRYING ONE. Families are deliberately NOT consulted
 * here: the resolver must try every `ID_LIKE` token before it settles for the family fallback, and a family whose
 * icon happens to be the hamburger would otherwise end the search too early. */
static const char *icon_id_for_token(const char *token)
{
	const struct aws_system_icon *icon;
	const struct aws_distro_entry *distro;
	const struct aws_alias_entry *alias;

	icon = aws_system_icon_by_id(token);
	if (icon != NULL) {
		return icon->id;
	}
	distro = find_distro(token);
	if (distro != NULL && distro->icon != NULL) {
		return distro->icon;
	}
	alias = find_alias(token);
	if (alias != NULL) {
		return alias->icon;
	}
	return NULL;
}

static const char *family_for_token(const char *token)
{
	const struct aws_distro_entry *distro = find_distro(token);
	const struct aws_family_entry *family;

	if (distro != NULL) {
		return distro->family;
	}
	family = find_family(token);
	if (family != NULL) {
		return family->family;
	}
	return NULL;
}

static const char *family_for_identity(const struct aws_os_identity *identity)
{
	char like[256];
	char *cursor;
	char *token;
	const char *family;

	if (identity == NULL) {
		return NULL;
	}
	if (identity->id[0] != '\0') {
		family = family_for_token(identity->id);
		if (family != NULL) {
			return family;
		}
	}
	if (identity->id_like[0] != '\0') {
		snprintf(like, sizeof like, "%s", identity->id_like);
		cursor = like;
		while ((token = next_token(&cursor)) != NULL) {
			family = family_for_token(token);
			if (family != NULL) {
				return family;
			}
		}
	}
	if (identity->is_bsd) {
		return "bsd";
	}
	return NULL;
}

struct aws_system_icon *aws_system_icon_by_id(const char *id)
{
	size_t i;

	if (id == NULL || id[0] == '\0') {
		return NULL;
	}
	for (i = 0; i < aws_icon_object_count; i++) {
		if (strcmp(aws_icon_objects[i].id, id) == 0) {
			return &aws_icon_objects[i];
		}
	}
	return NULL;
}

struct aws_system_icon *aws_system_icon_by_name(const char *name)
{
	char normalized[64];
	const char *icon_id;
	struct aws_system_icon *icon;
	const struct aws_family_entry *family;

	if (name == NULL) {
		return NULL;
	}
	normalize_token(name, normalized, sizeof normalized);
	if (normalized[0] == '\0') {
		return NULL;
	}
	icon = aws_system_icon_by_id(normalized);
	if (icon != NULL) {
		return icon;
	}
	icon_id = icon_id_for_token(normalized);
	if (icon_id != NULL) {
		icon = aws_system_icon_by_id(icon_id);
		if (icon != NULL) {
			return icon;
		}
	}
	family = find_family(normalized);
	if (family != NULL) {
		return aws_system_icon_by_id(family->icon);
	}
	return NULL;
}

struct aws_system_icon *aws_system_menu_icon_resolve(const struct aws_os_identity *identity)
{
	char like[256];
	char *cursor;
	char *token;
	const char *icon_id;
	const char *family;
	struct aws_system_icon *icon;
	const struct aws_family_entry *family_entry;

	if (identity != NULL && identity->id[0] != '\0') {
		icon_id = icon_id_for_token(identity->id);
		if (icon_id != NULL) {
			icon = aws_system_icon_by_id(icon_id);
			if (icon != NULL) {
				return icon;
			}
		}
	}
	if (identity != NULL && identity->id_like[0] != '\0') {
		snprintf(like, sizeof like, "%s", identity->id_like);
		cursor = like;
		while ((token = next_token(&cursor)) != NULL) {
			icon_id = icon_id_for_token(token);
			if (icon_id != NULL) {
				icon = aws_system_icon_by_id(icon_id);
				if (icon != NULL) {
					return icon;
				}
			}
		}
	}
	family = family_for_identity(identity);
	if (family != NULL) {
		family_entry = find_family(family);
		if (family_entry != NULL) {
			icon = aws_system_icon_by_id(family_entry->icon);
			if (icon != NULL) {
				return icon;
			}
		}
	}
	icon = aws_system_icon_by_id(AWS_SYSTEM_MENU_ICON_FALLBACK);
	return icon != NULL ? icon : &s_fallback;
}

const char *aws_system_menu_icon_family(const struct aws_os_identity *identity)
{
	const char *family = family_for_identity(identity);

	return family != NULL ? family : "unknown";
}

size_t aws_system_icon_count(void)
{
	return aws_icon_object_count;
}

struct aws_system_icon *aws_system_icon_at(size_t index)
{
	if (index >= aws_icon_object_count) {
		return NULL;
	}
	return &aws_icon_objects[index];
}

/* ============================================================================================ *
 * THE RASTERIZER.                                                                               *
 *                                                                                                *
 * THE MARK IS SMALL ENOUGH TO SUPERSAMPLE RATHER THAN TO BE CLEVER ABOUT. A 16×16 box with 4×4
 * samples per pixel is 4096 point-in-polygon questions, once per icon. Scanline coverage would be
 * faster and would need edge tables, active-edge sorting and a horizontal-coverage pass; at this
 * size the simple answer is exact enough and much easier to keep correct when the twelve marks
 * arrive.
 *
 * **AND THE SAME CODE ANSWERS EVERY DEVICE SIZE, WHICH IS WHY IT TAKES ONE.** The supersample
 * count is per PIXEL, so a 32×32 grid is a genuine raster at that resolution — the bars of the
 * hamburger land on exact device rows at 2x and gain nothing but sharpness — instead of a 16×16
 * grid enlarged four times, which is what the mark used to be at every scale. The edges are built
 * once per call because flattening a path is the expensive half and the size does not change them.
 *
 * EVEN-ODD IS THE FILL RULE. It makes holes in a mark a later subpath rather than a winding
 * convention, which is what the OS marks — eyes, swirl gaps, blowfish spots — need.
 * ============================================================================================ */

#define AWS_ICON_QUAD_SEGMENTS 8
#define AWS_ICON_CUBIC_SEGMENTS 12
#define AWS_ICON_SUPERSAMPLE 4

struct aws_icon_edge {
	double x0;
	double y0;
	double x1;
	double y1;
};

static int append_edge(struct aws_icon_edge *edges, size_t capacity, size_t *count,
		       double x0, double y0, double x1, double y1)
{
	if (*count >= capacity) {
		return -1;
	}
	edges[*count].x0 = x0;
	edges[*count].y0 = y0;
	edges[*count].x1 = x1;
	edges[*count].y1 = y1;
	(*count)++;
	return 0;
}

static int point_inside(const struct aws_icon_edge *edges, size_t count, double x, double y,
			enum aws_icon_fill fill)
{
	int winding = 0;
	size_t i;

	for (i = 0; i < count; i++) {
		const struct aws_icon_edge *edge = &edges[i];

		/* The half-open rule counts a vertex on one side and not the other, so shared endpoints of a
		 * closed subpath are not counted twice. */
		if ((edge->y0 > y) != (edge->y1 > y)) {
			double crossing = edge->x0 +
					  (y - edge->y0) * (edge->x1 - edge->x0) / (edge->y1 - edge->y0);

			if (x < crossing) {
				if (fill == AWS_ICON_FILL_EVEN_ODD) {
					winding ^= 1;
				} else {
					winding += edge->y1 > edge->y0 ? 1 : -1;
				}
			}
		}
	}
	return winding != 0;
}

/* **THE EDGES OF A MARK, FLATTENED FROM ITS CURVES — ONCE PER RASTER, NOT PER PIXEL.** Curves become line segments
 * here and the fill below only does algebra, so a 32×32 grid costs the same edge walk a 16×16 one does. The caller
 * frees the answer; NULL means the mark has no paths, which both callers treat as an empty grid. */
static struct aws_icon_edge *build_edges(const struct aws_system_icon *icon, size_t *count_out)
{
	size_t command_count = 0;
	size_t capacity;
	size_t path_index;
	struct aws_icon_edge *edges;
	size_t edge_count = 0;

	*count_out = 0;
	if (icon == NULL || icon->paths == NULL || icon->path_count == 0) {
		return NULL;
	}
	for (path_index = 0; path_index < icon->path_count; path_index++) {
		command_count += icon->paths[path_index].command_count;
	}
	/* One edge per line/close, at most twelve per cubic, and a little slack for implicit closing edges. */
	capacity = command_count * AWS_ICON_CUBIC_SEGMENTS + icon->path_count * 2 + 1;
	edges = malloc(capacity * sizeof *edges);
	if (edges == NULL) {
		return NULL;
	}

	for (path_index = 0; path_index < icon->path_count; path_index++) {
		const struct aws_icon_path *path = &icon->paths[path_index];
		double start_x = 0.0, start_y = 0.0;
		double current_x = 0.0, current_y = 0.0;
		int open = 0;
		size_t command_index;

		for (command_index = 0; command_index < path->command_count; command_index++) {
			const struct aws_icon_command *command = &path->commands[command_index];

			switch (command->op) {
			case AWS_ICON_MOVE:
				if (open) {
					append_edge(edges, capacity, &edge_count, current_x, current_y,
						    start_x, start_y);
				}
				start_x = current_x = command->point[0].x;
				start_y = current_y = command->point[0].y;
				open = 1;
				break;
			case AWS_ICON_LINE:
				if (!open) {
					break;
				}
				append_edge(edges, capacity, &edge_count, current_x, current_y,
					    command->point[0].x, command->point[0].y);
				current_x = command->point[0].x;
				current_y = command->point[0].y;
				break;
			case AWS_ICON_QUAD:
				if (!open) {
					break;
				}
				for (int step = 1; step <= AWS_ICON_QUAD_SEGMENTS; step++) {
					double t = (double)step / AWS_ICON_QUAD_SEGMENTS;
					double inverse = 1.0 - t;
					double x = inverse * inverse * current_x +
						   2.0 * inverse * t * command->point[0].x +
						   t * t * command->point[1].x;
					double y = inverse * inverse * current_y +
						   2.0 * inverse * t * command->point[0].y +
						   t * t * command->point[1].y;

					append_edge(edges, capacity, &edge_count, current_x, current_y, x, y);
					current_x = x;
					current_y = y;
				}
				break;
			case AWS_ICON_CUBIC:
				if (!open) {
					break;
				}
				for (int step = 1; step <= AWS_ICON_CUBIC_SEGMENTS; step++) {
					double t = (double)step / AWS_ICON_CUBIC_SEGMENTS;
					double inverse = 1.0 - t;
					double x = inverse * inverse * inverse * current_x +
						   3.0 * inverse * inverse * t * command->point[0].x +
						   3.0 * inverse * t * t * command->point[1].x +
						   t * t * t * command->point[2].x;
					double y = inverse * inverse * inverse * current_y +
						   3.0 * inverse * inverse * t * command->point[0].y +
						   3.0 * inverse * t * t * command->point[1].y +
						   t * t * t * command->point[2].y;

					append_edge(edges, capacity, &edge_count, current_x, current_y, x, y);
					current_x = x;
					current_y = y;
				}
				break;
			case AWS_ICON_CLOSE:
				if (open) {
					append_edge(edges, capacity, &edge_count, current_x, current_y,
						    start_x, start_y);
					open = 0;
				}
				break;
			}
		}
		/* A subpath without a CLOSE is still a filled region; the fill rule closes it. */
		if (open) {
			append_edge(edges, capacity, &edge_count, current_x, current_y, start_x, start_y);
		}
	}
	*count_out = edge_count;
	return edges;
}

/* **THE FILL, AT WHATEVER SIZE THE CALLER ASKED FOR.** `pixels` is the box's side in DEVICE pixels, and the sample
 * positions are fractions of the box, so 16, 21 and 32 are the same paths rasterized at three resolutions rather
 * than three views of one 16×16 image. `grid` is `pixels * pixels` bytes and is overwritten, not blended into. */
static void fill_grid(const struct aws_icon_edge *edges, size_t edge_count, uint8_t *grid, int pixels,
		      enum aws_icon_fill fill)
{
	const unsigned samples = AWS_ICON_SUPERSAMPLE * AWS_ICON_SUPERSAMPLE;
	int row, column;

	for (row = 0; row < pixels; row++) {
		for (column = 0; column < pixels; column++) {
			unsigned hits = 0;
			int sample_y;

			for (sample_y = 0; sample_y < AWS_ICON_SUPERSAMPLE; sample_y++) {
				double y = ((double)row +
					   ((double)sample_y + 0.5) / AWS_ICON_SUPERSAMPLE) /
					   (double)pixels;
				int sample_x;

				for (sample_x = 0; sample_x < AWS_ICON_SUPERSAMPLE; sample_x++) {
					double x = ((double)column +
						   ((double)sample_x + 0.5) / AWS_ICON_SUPERSAMPLE) /
						   (double)pixels;

					if (point_inside(edges, edge_count, x, y, fill)) {
						hits++;
					}
				}
			}
			grid[row * pixels + column] = (uint8_t)((hits * 255u + samples / 2u) / samples);
		}
	}
}

/* THE LOGICAL GRID, FILLED LAZILY. `coverage_ready` is set even when nothing could be filled, because a mark that
 * does not appear is better than one that rasterizes again on every frame. */
static void rasterize_icon(struct aws_system_icon *icon)
{
	struct aws_icon_edge *edges;
	size_t edge_count = 0;

	memset(icon->coverage, 0, sizeof icon->coverage);
	icon->coverage_ready = 1;
	edges = build_edges(icon, &edge_count);
	if (edges == NULL) {
		return;
	}
	fill_grid(edges, edge_count, icon->coverage, AWS_SYSTEM_ICON_PIXELS, icon->fill);
	free(edges);
}

const uint8_t *aws_system_icon_coverage(struct aws_system_icon *icon)
{
	if (icon == NULL) {
		return NULL;
	}
	if (!icon->coverage_ready) {
		rasterize_icon(icon);
	}
	return icon->coverage;
}

const uint8_t *aws_system_icon_coverage_at(struct aws_system_icon *icon, int pixels)
{
	struct aws_icon_grid *grown;
	struct aws_icon_edge *edges;
	size_t edge_count = 0;
	uint8_t *grid;
	size_t index;

	if (icon == NULL || pixels < 1 || pixels > AWS_SYSTEM_ICON_MAX_PIXELS) {
		return NULL;
	}
	/* THE 1x ANSWER ALREADY EXISTS, and it is the inline grid every diagnostic reads. */
	if (pixels == AWS_SYSTEM_ICON_PIXELS) {
		return aws_system_icon_coverage(icon);
	}
	for (index = 0; index < icon->grid_count; index++) {
		if (icon->grids[index].pixels == pixels) {
			return icon->grids[index].coverage;
		}
	}
	/* **THE SLOT BEFORE THE GRID**, so a failed allocation leaves the icon exactly as it was rather than with an
	 * entry pointing at nothing. The number of sizes is the number of distinct output scales a session has, which a
	 * fixed panel makes one — there is no eviction because there is nothing to evict. */
	grown = realloc(icon->grids, (icon->grid_count + 1) * sizeof *grown);
	if (grown == NULL) {
		return NULL;
	}
	icon->grids = grown;
	grid = calloc((size_t)pixels * (size_t)pixels, 1);
	if (grid == NULL) {
		return NULL;
	}
	edges = build_edges(icon, &edge_count);
	if (edges != NULL) {
		fill_grid(edges, edge_count, grid, pixels, icon->fill);
		free(edges);
	}
	icon->grids[icon->grid_count].pixels = pixels;
	icon->grids[icon->grid_count].coverage = grid;
	icon->grid_count++;
	return grid;
}
