/*
 * systemicon.h — which mark the System menu draws, and what shape it is.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * THE MARK IS DATA AND NOT AN ENUM. There are eleven distinct exact marks now — FreeBSD stands in for DragonFlyBSD
 * — plus the hamburger, and there will be more later. A resolver that returned an enum would grow a new value every
 * time the data grew. It returns a pointer to an icon object instead; the id is a string, and the lookup tables are
 * generated from the TSVs in `assets/system-menu-icons/`.
 *
 * THE SHAPE IS DATA TOO. An icon is a list of subpaths in normalized coordinates, filled even-odd or nonzero and
 * rasterized once into a 16×16 coverage grid. The compositor blends that grid with the one ink colour; it never
 * asks what the mark means. The hamburger and the exact OS marks all use the same path format.
 *
 * THE CACHE MAKES THE OBJECT MUTABLE. `coverage` and `coverage_ready` are written on first use, which is why the
 * lookups and the server's pointer are non-const. The path arrays themselves are immutable and live in
 * `systemicon-data.c`.
 */

#ifndef AGORA_SYSTEMICON_H
#define AGORA_SYSTEMICON_H

#include <stddef.h>
#include <stdint.h>

struct aws_os_identity;

#define AWS_SYSTEM_ICON_PIXELS 16

/* The System menu square is drawn with this; the rasterizer emits this many rows and columns. */
#define AWS_SYSTEM_ICON_COVERAGE_BYTES (AWS_SYSTEM_ICON_PIXELS * AWS_SYSTEM_ICON_PIXELS)

enum aws_icon_fill {
	AWS_ICON_FILL_EVEN_ODD = 0,
	AWS_ICON_FILL_NONZERO
};

enum aws_icon_op {
	AWS_ICON_MOVE = 0,
	AWS_ICON_LINE,
	AWS_ICON_QUAD,
	AWS_ICON_CUBIC,
	AWS_ICON_CLOSE
};

struct aws_icon_point {
	float x;
	float y;
};

/* A subpath command. `point[0]` is the destination for MOVE/LINE, the control point for QUAD, the first control
 * point for CUBIC, and unused for CLOSE. Coordinates are normalized to [0,1], y down. */
struct aws_icon_command {
	enum aws_icon_op op;
	struct aws_icon_point point[3];
};

struct aws_icon_path {
	const struct aws_icon_command *commands;
	size_t command_count;
};

struct aws_system_icon {
	const char *id;				/* "debian", "freebsd", ... */
	const struct aws_icon_path *paths;	/* NULL until the mark has paths */
	size_t path_count;
	enum aws_icon_fill fill;
	uint8_t coverage[AWS_SYSTEM_ICON_COVERAGE_BYTES];
	int coverage_ready;
};

#define AWS_SYSTEM_MENU_ICON_FALLBACK "hamburger"

/* BY ID, AND BY ID-OR-ALIAS. `by_name` is the CLI's door: it accepts an icon id ("debian"), an alias ("mint"),
 * a distro id whose family has a mark ("rocky"), or a family name ("suse"). It answers NULL for anything else. */
struct aws_system_icon *aws_system_icon_by_id(const char *id);
struct aws_system_icon *aws_system_icon_by_name(const char *name);

/* THE RESOLVER: exact ID, alias, each ID_LIKE token in order, family, then the hamburger. Never NULL. */
struct aws_system_icon *aws_system_menu_icon_resolve(const struct aws_os_identity *identity);

/* THE FAMILY THE RESOLVER USED, for `--dump-os-identity`. Answers "unknown" when no family was found. */
const char *aws_system_menu_icon_family(const struct aws_os_identity *identity);

/* THE AVAILABLE MARKS, for `--list-system-menu-icons`. The order is the generated table's order. */
size_t aws_system_icon_count(void);
struct aws_system_icon *aws_system_icon_at(size_t index);

/* THE 16×16 COVERAGE GRID, rasterized on first use and cached on the icon. Returns NULL only for a NULL icon.
 * An icon with no paths answers an all-zero grid rather than NULL, so the caller's loop is the same for every
 * mark. */
const uint8_t *aws_system_icon_coverage(struct aws_system_icon *icon);

#endif
