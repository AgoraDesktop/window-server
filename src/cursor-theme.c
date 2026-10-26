/*
 * cursor-theme.c — reading an Xcursor theme, which is a directory of files.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * THE FORMAT, IN FULL, BECAUSE IT IS SMALL ENOUGH THAT A READER IS BETTER THAN A LIBRARY. An `Xcur` file is
 * a 16-byte header — magic, header size, version, the number of table-of-contents entries — then one 12-byte
 * TOC entry per chunk (type, subtype, byte offset), then the chunks. The only chunk that matters is the
 * IMAGE one (`0xfffd0002`): a 36-byte header whose subtype is the NOMINAL SIZE (the design size, in points
 * at 96 dpi), then width, height, hotspot x, hotspot y, a frame delay in milliseconds, then width × height
 * premultiplied ARGB pixels. A `COMMENT` chunk (`0xfffe0001`) is skipped; nothing else exists.
 *
 * **AND THE FILES ARE IN THE BYTE ORDER OF WHOEVER WROTE THEM**, not a stated one — libXcursor simply
 * writes its own. So the first u32 after the magic decides it: a file header of 16 in one order and
 * something else in the other is unambiguous, and that also makes these files readable on the big-endian
 * BSDs Agora targets as well as on x86.
 *
 * EVERY READ IS BOUNDS-CHECKED AGAINST THE MAPPED FILE. A theme is somebody else's data — it can be
 * truncated, fetched from a URL, or be the wrong file entirely (a `.md` renamed to `left_ptr`) — and a
 * compositor that walked off the end of one would be a pointer that took the session with it. A file that
 * does not add up is simply "no such cursor", which the caller already has to handle.
 *
 * **NOTHING HERE IS RESAMPLED OR COMPOSITED.** The image a caller gets is the theme's own pixels at the
 * theme's own size; the only transformation is undoing the premultiplication, once, so the two renderers
 * can blend the image the way they blend everything else.
 */

#include "cursor-theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* WHERE THE VENDORED THEME IS INSTALLED. CMake replaces this; the fallback only exists so this file can be
 * compiled on its own by a test that passes its own directory. */
#ifndef AWS_CURSOR_DEFAULT_THEME
#define AWS_CURSOR_DEFAULT_THEME "/usr/local/share/icons/Vimix"
#endif

#define THEME_CURSORS_DIRECTORY "cursors"
#define THEME_PATH_MAX 1024
#define THEME_MAX_FILE_BYTES (16u * 1024u * 1024u)
#define THEME_MAX_TOC_ENTRIES 4096u
#define THEME_MAX_IMAGE_SIDE 1024
#define THEME_MAX_FRAME_DELAY 600000
#define THEME_FILE_HEADER_SIZE 16u
#define THEME_FILE_VERSION 0x00010000u
#define THEME_TOC_ENTRY_SIZE 12u
#define THEME_IMAGE_CHUNK 0xfffd0002u

struct theme_cursor {
	char *name;
	int image_count;
	int image_capacity;
	struct aws_cursor_image *images;	/* owns every image's pixels */
	int variant_count;
	struct aws_cursor_frames *variants;	/* each points INTO `images`, so grouped after parsing */
};

struct aws_cursor_theme {
	char *directory;
	struct theme_cursor *cursors;
	int cursor_count;
	int cursor_capacity;
};

/* ------------------------------------------------------------------------- */
/* Little-endian reads, with the file's own order resolved once               */
/* ------------------------------------------------------------------------- */

static uint32_t theme_u32(const unsigned char *bytes, int swapped)
{
	uint32_t value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
			 ((uint32_t)bytes[3] << 24);

	if (!swapped) {
		return value;
	}
	return (value >> 24) | ((value >> 8) & 0x0000FF00u) | ((value << 8) & 0x00FF0000u) | (value << 24);
}

/* **UN-PREMULTIPLY, because the renderers blend straight alpha.** The files hold B×A/255 and friends;
 * `cursor_over` in cursor.c and GL's `GL_SRC_ALPHA` blend both want the colour and the alpha separate, and
 * doing the division here means neither of them has to know where the image came from. A channel that
 * claims to be larger than its own alpha is bad data and is clamped rather than allowed to wrap. */
static uint32_t theme_straight(uint32_t pixel)
{
	uint32_t alpha = pixel >> 24;
	uint32_t r, g, b;

	if (alpha == 0) {
		return 0;
	}
	if (alpha == 255) {
		return pixel | 0xFF000000u;
	}
	r = (((pixel >> 16) & 0xFFu) * 255u + alpha / 2u) / alpha;
	g = (((pixel >> 8) & 0xFFu) * 255u + alpha / 2u) / alpha;
	b = ((pixel & 0xFFu) * 255u + alpha / 2u) / alpha;
	if (r > 255u) {
		r = 255u;
	}
	if (g > 255u) {
		g = 255u;
	}
	if (b > 255u) {
		b = 255u;
	}
	return (alpha << 24) | (r << 16) | (g << 8) | b;
}

static unsigned char *theme_read_file(const char *path, size_t *size)
{
	FILE *file;
	unsigned char *data;
	long length;
	size_t got;

	file = fopen(path, "rb");
	if (file == NULL) {
		return NULL;
	}
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
	    (unsigned long)length > THEME_MAX_FILE_BYTES || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return NULL;
	}
	data = malloc((size_t)length);
	if (data == NULL) {
		fclose(file);
		return NULL;
	}
	got = fread(data, 1, (size_t)length, file);
	fclose(file);
	if (got != (size_t)length) {
		free(data);
		return NULL;
	}
	*size = got;
	return data;
}

/* ------------------------------------------------------------------------- */
/* Decoding one cursor file                                                   */
/* ------------------------------------------------------------------------- */

/* ONE IMAGE CHUNK, OR NOTHING. `position` is where the chunk's header starts; the file has already been
 * checked to contain the header, and this checks the image that the header describes. */
static int theme_read_image(const unsigned char *data, size_t size, size_t position, int swapped,
			    struct aws_cursor_image *image)
{
	uint32_t chunk_header, chunk_type, nominal, version;
	uint32_t width, height, hotspot_x, hotspot_y, delay;
	size_t pixels_at;
	size_t count, i;
	uint32_t *pixels;

	if (position + 36u > size) {
		return -1;
	}
	chunk_header = theme_u32(data + position, swapped);
	chunk_type = theme_u32(data + position + 4, swapped);
	nominal = theme_u32(data + position + 8, swapped);
	version = theme_u32(data + position + 12, swapped);
	width = theme_u32(data + position + 16, swapped);
	height = theme_u32(data + position + 20, swapped);
	hotspot_x = theme_u32(data + position + 24, swapped);
	hotspot_y = theme_u32(data + position + 28, swapped);
	delay = theme_u32(data + position + 32, swapped);

	if (chunk_type != THEME_IMAGE_CHUNK || version != 1u || chunk_header < 36u || nominal == 0u) {
		return -1;
	}
	if (width == 0u || height == 0u || width > THEME_MAX_IMAGE_SIDE || height > THEME_MAX_IMAGE_SIDE) {
		return -1;
	}
	/* A HOTSPOT INSIDE ITS OWN IMAGE, which is the one property every compositor relies on: outside it, the
	 * pointer's tip would be a pixel the image does not have. */
	if (hotspot_x >= width || hotspot_y >= height) {
		return -1;
	}
	pixels_at = position + chunk_header;
	count = (size_t)width * (size_t)height;
	if (pixels_at + count * sizeof(uint32_t) > size) {
		return -1;
	}
	pixels = malloc(count * sizeof *pixels);
	if (pixels == NULL) {
		return -1;
	}
	for (i = 0; i < count; i++) {
		pixels[i] = theme_straight(theme_u32(data + pixels_at + i * sizeof(uint32_t), swapped));
	}
	image->nominal_size = (int)nominal;
	image->width = (int)width;
	image->height = (int)height;
	image->hotspot_x = (int)hotspot_x;
	image->hotspot_y = (int)hotspot_y;
	image->delay_ms = (delay > THEME_MAX_FRAME_DELAY) ? THEME_MAX_FRAME_DELAY : (int)delay;
	image->pixels = pixels;
	return 0;
}

/* THE WHOLE FILE, AS A LIST OF IMAGES IN FILE ORDER — which is nominal size ascending and, within one
 * nominal size, the frames of an animation. Returns 0 and an empty list for a file that is not an Xcursor
 * file at all; the caller turns an empty list into "no such cursor". */
static int theme_read_cursor(const char *path, struct theme_cursor *cursor)
{
	unsigned char *data;
	size_t size = 0;
	uint32_t header_size, version, toc_count;
	int swapped;
	uint32_t entry;
	int i;

	data = theme_read_file(path, &size);
	if (data == NULL) {
		return -1;
	}
	if (size < THEME_FILE_HEADER_SIZE || memcmp(data, "Xcur", 4) != 0) {
		free(data);
		return -1;
	}
	header_size = theme_u32(data + 4, 0);
	swapped = 0;
	if (header_size != THEME_FILE_HEADER_SIZE) {
		header_size = theme_u32(data + 4, 1);
		swapped = 1;
	}
	version = theme_u32(data + 8, swapped);
	toc_count = theme_u32(data + 12, swapped);
	if (header_size != THEME_FILE_HEADER_SIZE || version != THEME_FILE_VERSION ||
	    toc_count == 0u || toc_count > THEME_MAX_TOC_ENTRIES) {
		free(data);
		return -1;
	}
	if ((size_t)header_size + (size_t)toc_count * THEME_TOC_ENTRY_SIZE > size) {
		free(data);
		return -1;
	}
	for (i = 0; i < (int)toc_count; i++) {
		const unsigned char *toc = data + header_size + (size_t)i * THEME_TOC_ENTRY_SIZE;
		struct aws_cursor_image image;
		size_t position;

		entry = theme_u32(toc, swapped);
		if (entry != THEME_IMAGE_CHUNK) {
			continue;	/* a COMMENT chunk says who made the theme; nothing here draws it */
		}
		position = (size_t)theme_u32(toc + 8, swapped);
		memset(&image, 0, sizeof image);
		if (theme_read_image(data, size, position, swapped, &image) == 0) {
			if (cursor->image_count == cursor->image_capacity) {
				int capacity = (cursor->image_capacity == 0) ? 8 : cursor->image_capacity * 2;
				struct aws_cursor_image *grown = realloc(cursor->images, (size_t)capacity * sizeof *grown);

				if (grown == NULL) {
					free((void *)image.pixels);
					continue;
				}
				cursor->images = grown;
				cursor->image_capacity = capacity;
			}
			cursor->images[cursor->image_count++] = image;
		}
	}
	free(data);
	return 0;
}

/* **THE FRAMES OF ONE SIZE ARE THE CONTIGUOUS RUN OF IMAGES WITH THE SAME NOMINAL SIZE.** Done after the
 * whole file is parsed, and not while it is being parsed, because the image array grows by reallocation and
 * a frame list built as it went would point into a buffer that had moved. */
static void theme_cursor_group(struct theme_cursor *cursor)
{
	int start = 0;
	int i;

	cursor->variant_count = 0;
	if (cursor->image_count == 0) {
		return;
	}
	cursor->variants = calloc((size_t)cursor->image_count, sizeof *cursor->variants);
	if (cursor->variants == NULL) {
		return;
	}
	for (i = 1; i <= cursor->image_count; i++) {
		if (i == cursor->image_count ||
		    cursor->images[i].nominal_size != cursor->images[start].nominal_size) {
			cursor->variants[cursor->variant_count].frames = &cursor->images[start];
			cursor->variants[cursor->variant_count].count = i - start;
			cursor->variant_count++;
			start = i;
		}
	}
}

static void theme_cursor_free(struct theme_cursor *cursor)
{
	int i;

	for (i = 0; i < cursor->image_count; i++) {
		free((void *)cursor->images[i].pixels);
	}
	free(cursor->images);
	free(cursor->variants);
	free(cursor->name);
	memset(cursor, 0, sizeof *cursor);
}

/* ------------------------------------------------------------------------- */
/* Selecting a variant, and the theme's cache                                 */
/* ------------------------------------------------------------------------- */

/* THE ARTWORK'S SIZE IS ITS LARGER SIDE, because that is what "a 32-pixel pointer" means to whoever sets a
 * cursor size — an I-beam's height and a hand's width are both the box the theme designed. */
static int theme_image_extent(const struct aws_cursor_image *image)
{
	return (image->width > image->height) ? image->width : image->height;
}

/* **THE SMALLEST VARIANT THAT COVERS THE SIZE ASKED FOR, OR THE LARGEST WHEN NONE DOES** — libXcursor's own
 * rule, kept because it is what a person moving a theme between desktops expects, and because a variant that
 * was enlarged to fit would be the blocky magnified image this module exists to avoid. */
static const struct aws_cursor_frames *theme_cursor_best(const struct theme_cursor *cursor, double device_size)
{
	int best = -1;
	int best_extent = 0;
	int largest = -1;
	int largest_extent = 0;
	int i;

	for (i = 0; i < cursor->variant_count; i++) {
		int extent = theme_image_extent(&cursor->variants[i].frames[0]);

		if (extent > largest_extent) {
			largest_extent = extent;
			largest = i;
		}
		if ((double)extent < device_size) {
			continue;
		}
		if (best < 0 || extent < best_extent) {
			best = i;
			best_extent = extent;
		}
	}
	if (best >= 0) {
		return &cursor->variants[best];
	}
	if (largest >= 0) {
		return &cursor->variants[largest];
	}
	return NULL;
}

static int theme_name_is_safe(const char *name)
{
	const char *p;

	if (name == NULL || name[0] == '\0' || name[0] == '.') {
		return 0;
	}
	for (p = name; *p != '\0'; p++) {
		if (*p == '/' || *p == '\\') {
			return 0;
		}
	}
	return 1;
}

static struct theme_cursor *theme_cursor_find(struct aws_cursor_theme *theme, const char *name)
{
	int i;

	for (i = 0; i < theme->cursor_count; i++) {
		if (strcmp(theme->cursors[i].name, name) == 0) {
			return &theme->cursors[i];
		}
	}
	return NULL;
}

static struct theme_cursor *theme_cursor_add(struct aws_cursor_theme *theme, const char *name)
{
	struct theme_cursor *cursor;
	char path[THEME_PATH_MAX];
	int length;

	length = snprintf(path, sizeof path, "%s/%s/%s", theme->directory, THEME_CURSORS_DIRECTORY, name);
	if (length < 0 || (size_t)length >= sizeof path) {
		return NULL;
	}
	if (theme->cursor_count == theme->cursor_capacity) {
		int capacity = (theme->cursor_capacity == 0) ? 8 : theme->cursor_capacity * 2;
		struct theme_cursor *grown = realloc(theme->cursors, (size_t)capacity * sizeof *grown);

		if (grown == NULL) {
			return NULL;
		}
		theme->cursors = grown;
		theme->cursor_capacity = capacity;
	}
	cursor = &theme->cursors[theme->cursor_count];
	memset(cursor, 0, sizeof *cursor);
	cursor->name = strdup(name);
	if (cursor->name == NULL) {
		return NULL;
	}
	if (theme_read_cursor(path, cursor) != 0 || cursor->image_count == 0) {
		theme_cursor_free(cursor);
		return NULL;
	}
	theme_cursor_group(cursor);
	if (cursor->variant_count == 0) {
		theme_cursor_free(cursor);
		return NULL;
	}
	theme->cursor_count++;
	return &theme->cursors[theme->cursor_count - 1];
}

/* ------------------------------------------------------------------------- */
/* The doors                                                                  */
/* ------------------------------------------------------------------------- */

const char *aws_cursor_theme_default_directory(void)
{
	const char *environment = getenv("AGORA_CURSOR_THEME");

	if (environment != NULL && environment[0] != '\0') {
		return environment;
	}
	return AWS_CURSOR_DEFAULT_THEME;
}

struct aws_cursor_theme *aws_cursor_theme_open(const char *directory)
{
	struct aws_cursor_theme *theme;
	struct stat status;
	char path[THEME_PATH_MAX];
	int length;

	if (directory == NULL || directory[0] == '\0') {
		return NULL;
	}
	length = snprintf(path, sizeof path, "%s/%s", directory, THEME_CURSORS_DIRECTORY);
	if (length < 0 || (size_t)length >= sizeof path) {
		return NULL;
	}
	if (stat(path, &status) != 0 || !S_ISDIR(status.st_mode)) {
		return NULL;
	}
	theme = calloc(1, sizeof *theme);
	if (theme == NULL) {
		return NULL;
	}
	theme->directory = strdup(directory);
	if (theme->directory == NULL) {
		free(theme);
		return NULL;
	}
	return theme;
}

void aws_cursor_theme_close(struct aws_cursor_theme *theme)
{
	int i;

	if (theme == NULL) {
		return;
	}
	for (i = 0; i < theme->cursor_count; i++) {
		theme_cursor_free(&theme->cursors[i]);
	}
	free(theme->cursors);
	free(theme->directory);
	free(theme);
}

const char *aws_cursor_theme_directory(const struct aws_cursor_theme *theme)
{
	return (theme != NULL) ? theme->directory : NULL;
}

const struct aws_cursor_frames *aws_cursor_theme_frames(struct aws_cursor_theme *theme, const char *name,
							double device_size)
{
	struct theme_cursor *cursor;

	if (theme == NULL || !theme_name_is_safe(name)) {
		return NULL;
	}
	cursor = theme_cursor_find(theme, name);
	if (cursor == NULL) {
		cursor = theme_cursor_add(theme, name);
	}
	if (cursor == NULL) {
		return NULL;
	}
	return theme_cursor_best(cursor, device_size);
}
