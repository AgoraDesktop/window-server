/*
 * client — the smallest xdg-shell client, used to test the window server without libagora-app.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WHAT IT PROVES. The window server can be tested against a client that is nothing but the protocol: connect,
 * be given a role, be CONFIGURED, attach a shared-memory buffer, commit it, and be told the frame was taken.
 * Every one of those steps is a step the server can get wrong on its own, and the screenshot that comes out is
 * the evidence — which is why this draws one flat colour rather than anything clever: the assertion is WHERE
 * the colour landed, not what it looks like.
 *
 * IT ALSO PINS THE ONE THING THAT IS EASY TO FORGET IN A CLIENT: the first commit carries NO buffer. That
 * commit is what asks for a role to be configured, and a client that attaches first is committing before the
 * compositor has said the surface may be shown.
 *
 *   client [--width W] [--height H] [--frames N] [--hold] [--colour RRGGBB] [--image PATH]
 *          [--x X] [--y Y] [--level L] [--status-length N] [--dmabuf]
 *          [--buffer-scale N] [--quadrants]
 *
 * **`--buffer-scale` IS THE HIDPI CLIENT'S HALF OF `wl_surface`.** It allocates a buffer N times the frame, tells the
 * surface so with `set_buffer_scale`, and leaves the compositor to place it as the frame it described. `--quadrants`
 * fills that buffer with four colours, so a screenshot proves the WHOLE buffer was downscaled into the window rather
 * than its first quarter being drawn at double size.
 *
 * **`--image` IS THE WALLPAPER'S DOOR.** A JPEG is decoded with libjpeg — the same decoder Core Graphics uses in
 * this tree — scaled to COVER the window and cropped at its centre. The flat colour remains the default, because
 * the server tests assert WHERE a colour landed and must not depend on an image decoder.
 */

#define _GNU_SOURCE

#include <wayland-client.h>
#include "agora-shell-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"

#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <unistd.h>

#ifdef AWS_HAVE_GBM
/* **THE CLIENT'S HALF OF A GPU BUFFER.** GBM allocates one on the render node and exports the file descriptor the
 * compositor imports; without it, `--dmabuf` is a request this build cannot answer and the client says so. */
#include <gbm.h>
#include <drm_fourcc.h>
#endif

struct client {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct agora_shell *shell;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	/* **WHETHER THE PIXELS LIVE IN GPU MEMORY.** `--dmabuf` asks for it, and the fallback to shared memory is SAID
	 * OUT LOUD rather than silent: a test that quietly stopped testing the thing it named would be worse than one
	 * that failed. */
	int use_dmabuf;
	/* **THE SELECTION'S TWO ROLES, WHICH ARE TWO PROCESSES.** `--offer` makes this client own the selection with one
	 * type and answer requests by writing the payload; `--request` asks for a type, reads what it gets, and prints it.
	 * A test then runs both against one server, which is the only way to prove the fd really crosses. */
	const char *offer_type;
	const char *offer_payload;
	const char *request_type;
	int selection_answered;			/* the requester's fd arrived */
	int selection_refused;			/* and the reason it did not */

	struct wl_surface *surface;
	struct agora_window *window;
	int frame_x, frame_y;
	int level;			/* the NSWindow level to ask for, so two clients can be ordered */
	struct wl_buffer *buffer;
	uint32_t *pixels;

	/* **AND A SECOND BUFFER, FOR THE ONE TEST THAT IS ABOUT BUFFER LIFETIME RATHER THAN PIXELS.** A release is a
	 * promise that the compositor has finished with those pixels, so it may only ever name a buffer that has been
	 * REPLACED — never the one on the glass. One buffer cannot show that (its release and its content are the same
	 * buffer), so `--double` keeps two and alternates them, which is what every real client does and what
	 * `libagora-app/platform/AGWindowServer-Wayland.c` does for the reason in `README.md` §2c. */
	int double_buffer;
	struct wl_buffer *buffers[2];
	uint32_t *buffer_pixels[2];
	int slot;
	int releases;
	int releases_while_current;

	int width;
	int height;
	int status_length;	/* non-zero: a status item the SERVER places, and the frame asked for is ignored */
	int configured;
	int frames;
	int max_frames;
	/* **STAY CONNECTED AFTER THE LAST FRAME, WITHOUT DRAWING ANOTHER.** A wallpaper is a STILL
	 * picture: the client that redraws on every frame callback makes the compositor composite full
	 * screen as fast as it can answer — MEASURED, 233 a second and 86% of a core — for pixels that
	 * never change. `--hold` is how a test says "this one is scenery". */
	int hold;
	uint32_t colour;
	/* **A CLIENT THAT RENDERS AT 2x.** `--buffer-scale N` makes the BUFFER N times the frame it described and says so
	 * with `wl_surface.set_buffer_scale`, which is what a HiDPI client does: the compositor must place it as the
	 * frame it asked for and downsample the buffer into it. `--quadrants` fills the buffer with four colours so a
	 * downscale is visible in a screenshot: if the compositor drew only the first quarter of the buffer, three of
	 * the four colours would be missing. */
	int buffer_scale;
	int quadrants;
	int buffer_width;
	int buffer_height;

	/* **A CLIENT WHOSE PIXELS AND WHOSE DAMAGE DISAGREE ON PURPOSE.** A compositor that recomposites the whole window
	 * looks identical to one that honours the rectangles whenever the client repainted the whole window — so the strip
	 * modes POISON the rest of the buffer and damage only the strip. If the poison reaches the screen the compositor
	 * ignored the damage; if it does not, the rest of the window is the picture it already had. `--damage strip` is the
	 * partial commit, `whole` is a commit that damages nothing (Wayland's whole-buffer rule), and `move` is a
	 * `set_frame` — a full-redraw trigger that must repaint BOTH the new place and the ground the window left. */
	int damage_mode;		/* 0 none, 1 strip, 2 whole-buffer rule, 3 move, 4 two strips */
	int strip_y0;
	int strip_y1;
	int strip2_y0;			/* the second, DISJOINT strip: a region's reason for existing */
	int strip2_y1;
	uint32_t strip_colour;
	uint32_t third_colour;
	int move_x;
	int move_y;
	/* **AND A PAUSE BETWEEN COMMITS, WHICH IS WHAT LETS A HOST'S EXPOSE LAND FIRST.** A nested window that has just
	 * mapped can be exposed by the X server a frame or two later, and an expose is a FULL redraw by definition — on a
	 * client that had already poisoned its buffer that full frame would put the poison on the glass for the right
	 * reason. A delay is the test's way of letting the host say everything it has to say while the buffer is still an
	 * honest picture. */
	int frame_delay_ms;

	/* THE WALLPAPER IMAGE, when --image named one: RGB, three bytes per sample. */
	const char *image_path;
	unsigned char *image_rgb;
	int image_width;
	int image_height;
};

struct image_error {
	struct jpeg_error_mgr pub;
	jmp_buf escape;
};

static void image_error_exit(j_common_ptr cinfo)
{
	struct image_error *error = (struct image_error *)cinfo->err;

	longjmp(error->escape, 1);
}

/* A JPEG, decoded to RGB. NO, not RGBA: a JPEG has no alpha, and the destination's XRGB pixel is opaque anyway. */
static int load_jpeg(struct client *client, const char *path)
{
	struct jpeg_decompress_struct cinfo;
	struct image_error error;
	FILE *file;
	unsigned char *row;
	int created = 0;

	file = fopen(path, "rb");
	if (file == NULL) {
		fprintf(stderr, "client: cannot open image %s: %s\n", path, strerror(errno));
		return -1;
	}
	memset(&cinfo, 0, sizeof cinfo);
	cinfo.err = jpeg_std_error(&error.pub);
	error.pub.error_exit = image_error_exit;
	if (setjmp(error.escape) != 0) {
		if (created) {
			jpeg_destroy_decompress(&cinfo);
		}
		fclose(file);
		fprintf(stderr, "client: %s is not a readable JPEG\n", path);
		return -1;
	}
	jpeg_create_decompress(&cinfo);
	created = 1;
	jpeg_stdio_src(&cinfo, file);
	(void)jpeg_read_header(&cinfo, TRUE);
	cinfo.out_color_space = JCS_RGB;
	(void)jpeg_start_decompress(&cinfo);
	client->image_width = (int)cinfo.output_width;
	client->image_height = (int)cinfo.output_height;
	if (client->image_width <= 0 || client->image_height <= 0) {
		fprintf(stderr, "client: %s has no pixels\n", path);
		jpeg_destroy_decompress(&cinfo);
		fclose(file);
		return -1;
	}
	client->image_rgb = malloc((size_t)client->image_width * (size_t)client->image_height * 3u);
	row = malloc((size_t)client->image_width * 3u);
	if (client->image_rgb == NULL || row == NULL) {
		free(client->image_rgb);
		client->image_rgb = NULL;
		free(row);
		jpeg_destroy_decompress(&cinfo);
		fclose(file);
		return -1;
	}
	while (cinfo.output_scanline < cinfo.output_height) {
		unsigned char *rows[1] = { row };

		(void)jpeg_read_scanlines(&cinfo, rows, 1);
		memcpy(client->image_rgb + (size_t)(cinfo.output_scanline - 1) * (size_t)client->image_width * 3u,
		       row, (size_t)client->image_width * 3u);
	}
	free(row);
	(void)jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	fclose(file);
	return 0;
}

/* COVER, THEN CROP AT THE CENTRE, with bilinear sampling. The desktop's image and the window are usually both
 * 16:9, but a caller that resizes the host window must not get a stretched or letterboxed wallpaper. */
static uint32_t sample_image(struct client *client, int x, int y, double scale, double origin_x, double origin_y)
{
	double sx = ((double)x + 0.5) / scale + origin_x - 0.5;
	double sy = ((double)y + 0.5) / scale + origin_y - 0.5;
	int x0 = (int)sx;
	int y0 = (int)sy;
	int x1, y1;
	double fx, fy;
	unsigned char *p00;
	unsigned char *p10;
	unsigned char *p01;
	unsigned char *p11;
	double r, g, b;

	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x0 >= client->image_width) x0 = client->image_width - 1;
	if (y0 >= client->image_height) y0 = client->image_height - 1;
	x1 = (x0 + 1 < client->image_width) ? x0 + 1 : x0;
	y1 = (y0 + 1 < client->image_height) ? y0 + 1 : y0;
	fx = sx - (double)x0;
	fy = sy - (double)y0;
	if (fx < 0.0) fx = 0.0;
	if (fy < 0.0) fy = 0.0;
	p00 = client->image_rgb + ((size_t)y0 * (size_t)client->image_width + (size_t)x0) * 3u;
	p10 = client->image_rgb + ((size_t)y0 * (size_t)client->image_width + (size_t)x1) * 3u;
	p01 = client->image_rgb + ((size_t)y1 * (size_t)client->image_width + (size_t)x0) * 3u;
	p11 = client->image_rgb + ((size_t)y1 * (size_t)client->image_width + (size_t)x1) * 3u;
	r = (1.0 - fx) * (1.0 - fy) * p00[0] + fx * (1.0 - fy) * p10[0]
	    + (1.0 - fx) * fy * p01[0] + fx * fy * p11[0];
	g = (1.0 - fx) * (1.0 - fy) * p00[1] + fx * (1.0 - fy) * p10[1]
	    + (1.0 - fx) * fy * p01[1] + fx * fy * p11[1];
	b = (1.0 - fx) * (1.0 - fy) * p00[2] + fx * (1.0 - fy) * p10[2]
	    + (1.0 - fx) * fy * p01[2] + fx * fy * p11[2];
	return ((uint32_t)(r + 0.5) << 16) | ((uint32_t)(g + 0.5) << 8) | (uint32_t)(b + 0.5);
}

static void submit_frame(struct client *client);

static void frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
	struct client *client = data;

	(void)time;
	wl_callback_destroy(callback);
	client->frames++;
	if (client->max_frames > 0 && client->frames >= client->max_frames) {
		/* THE TEST'S STOP, and it is the same stop a real client uses: stop asking for frames. */
		return;
	}
	submit_frame(client);
}

static const struct wl_callback_listener frame_listener = { frame_done };

/* **A HORIZONTAL BAND OF THE BUFFER, IN ONE COLOUR.** The strip modes' whole vocabulary: the base picture, the strip
 * the commit names, and the poison the commit does not. */
static void paint_band(struct client *client, int y0, int y1, uint32_t colour)
{
	uint32_t *pixels = client->pixels;
	int x, y;

	if (pixels == NULL) {
		return;
	}
	if (y0 < 0) {
		y0 = 0;
	}
	if (y1 > client->buffer_height) {
		y1 = client->buffer_height;
	}
	for (y = y0; y < y1; y++) {
		for (x = 0; x < client->buffer_width; x++) {
			pixels[(size_t)y * (size_t)client->buffer_width + (size_t)x] = colour;
		}
	}
}

#define POISON_COLOUR 0x00FF00FFu	/* magenta: no other test in this tree uses it */

/* **WHAT THIS FRAME'S BUFFER HOLDS, WHICH IS NOT ALWAYS WHAT THIS FRAME'S DAMAGE NAMES.** The frame index is
 * `client->frames`, the count of frame callbacks already received: 0 is the first commit, which always paints the whole
 * buffer and damages the whole buffer. */
static void damage_mode_paint(struct client *client)
{
	int index = client->frames;

	if (client->damage_mode == 0) {
		return;
	}
	/* **TWO PLAIN FRAMES FIRST, AND THEY ARE NOT DECORATION.** A window that has just mapped can be exposed by the host
	 * a frame or two later, and a compositor must answer an expose with a FULL frame — which on a client that had already
	 * poisoned its buffer would put the poison on the glass for the right reason. Letting the exposure land while the
	 * buffer still holds an honest picture is what makes the poison mean "the compositor over-painted" rather than "the
	 * host asked for the window again". */
	if (index < 2) {
		paint_band(client, 0, client->buffer_height, client->colour);
		return;
	}
	if (client->damage_mode == 4 && index >= 2) {
		/* **TWO STRIPS AND THE GAP BETWEEN THEM, WHICH IS THE WHOLE POINT OF A REGION.** The gap is poisoned like
		 * everything else, so a compositor that unions the two rectangles into one bounding box paints the gap from the
		 * poisoned buffer and the gap goes magenta. */
		paint_band(client, 0, client->buffer_height, POISON_COLOUR);
		paint_band(client, client->strip_y0, client->strip_y1, client->strip_colour);
		paint_band(client, client->strip2_y0, client->strip2_y1, client->strip_colour);
		return;
	}
	if (client->damage_mode == 2 && index >= 3) {
		/* THE THIRD PICTURE, COMMITTED WITH NO DAMAGE: the whole-buffer rule has to repaint the window. */
		paint_band(client, 0, client->buffer_height, client->third_colour);
		return;
	}
	if (client->damage_mode == 3 && index >= 3) {
		/* THE WINDOW MOVED, and the buffer is painted correctly so the assertion is about the move rather than
		 * about the poison. The commit still damages NOTHING: `set_frame` is the full-redraw trigger. */
		paint_band(client, 0, client->buffer_height, client->colour);
		paint_band(client, client->strip_y0, client->strip_y1, client->strip_colour);
		return;
	}
	/* **THE POISON, THEN THE STRIP.** Everything the strip does not cover is a colour that must never reach the
	 * glass; only the strip is damaged. */
	paint_band(client, 0, client->buffer_height, POISON_COLOUR);
	paint_band(client, client->strip_y0, client->strip_y1, client->strip_colour);
}

/* ONE FRAME, IN THE ORDER THE PROTOCOL WANTS IT: the callback is requested BEFORE the commit it belongs to,
 * and the attach comes before the commit that presents it. */
static void submit_frame(struct client *client)
{
	struct wl_callback *callback = wl_surface_frame(client->surface);
	struct wl_buffer *buffer = client->buffer;

	/* **ALTERNATING IS THE WHOLE OF `--double`, AND IT IS DONE BEFORE THE COMMIT** so `slot` always names the
	 * buffer this client last handed over — the one the compositor is showing, and therefore the one a release
	 * must never name. */
	if (client->double_buffer) {
		client->slot ^= 1;
		buffer = client->buffers[client->slot];
	}
	wl_callback_add_listener(callback, &frame_listener, client);
	damage_mode_paint(client);
	if (client->frame_delay_ms > 0 && client->frames > 0) {
		usleep((useconds_t)client->frame_delay_ms * 1000u);
	}
	wl_surface_attach(client->surface, buffer, 0, 0);
	/* **THE DAMAGE IS THE FRAME'S STATEMENT ABOUT ITS PIXELS, AND EACH MODE IS A DIFFERENT ONE.** */
	if (client->damage_mode == 4 && client->frames >= 2) {
		wl_surface_damage_buffer(client->surface, 0, client->strip_y0, client->buffer_width,
					 client->strip_y1 - client->strip_y0);
		wl_surface_damage_buffer(client->surface, 0, client->strip2_y0, client->buffer_width,
					 client->strip2_y1 - client->strip2_y0);
	} else if ((client->damage_mode == 1 && client->frames >= 2) ||
	    ((client->damage_mode == 2 || client->damage_mode == 3) && client->frames == 2)) {
		wl_surface_damage_buffer(client->surface, 0, client->strip_y0, client->buffer_width,
					 client->strip_y1 - client->strip_y0);
	} else if (client->damage_mode == 3 && client->frames >= 2) {
		/* NOTHING DAMAGED, AND THE WINDOW MOVED: the frame is everything, because the ground it left is not the
		 * client's to damage. */
		agora_window_set_frame(client->window, client->move_x, client->move_y, client->width,
				       client->height);
	} else if (client->damage_mode == 2 && client->frames >= 2) {
		/* NOTHING DAMAGED, AND A BUFFER ATTACHED: Wayland says that means THE WHOLE BUFFER. */
	} else {
		wl_surface_damage_buffer(client->surface, 0, 0, client->buffer_width, client->buffer_height);
	}
	wl_surface_commit(client->surface);
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
	struct client *client = data;

	client->releases++;
	/* **A RELEASE THAT NAMES THE BUFFER ON THE GLASS IS THE COMPOSITOR TAKING PIXELS IT IS STILL DRAWING.** That is
	 * the bug this counter exists for: `README.md` §2c, and the blink it cost, which no other test in this tree can
	 * see because every other client draws one still picture. */
	if (client->double_buffer && client->buffers[client->slot] == buffer) {
		client->releases_while_current++;
	}
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };




/* **ONE PAINT, INTO WHATEVER MEMORY IS BEING FILLED.** Shared memory is a mapping and a GPU buffer is a mapping
 * too, and the pixels a test asserts on are the same either way — which is what makes `--dmabuf` a comparison of
 * two storage paths rather than two different pictures. */
static void fill_pixels(struct client *client, void *data, int stride)
{
	uint32_t *pixels = data;
	int words = stride / 4;
	int x, y;

	if (client->image_rgb != NULL) {
		double scale_x = (double)client->buffer_width / (double)client->image_width;
		double scale_y = (double)client->buffer_height / (double)client->image_height;
		double scale = (scale_x > scale_y) ? scale_x : scale_y;
		double origin_x = ((double)client->image_width - (double)client->buffer_width / scale) / 2.0;
		double origin_y = ((double)client->image_height - (double)client->buffer_height / scale) / 2.0;

		for (y = 0; y < client->buffer_height; y++) {
			for (x = 0; x < client->buffer_width; x++) {
				pixels[y * words + x] = sample_image(client, x, y, scale, origin_x, origin_y);
			}
		}
	} else if (client->quadrants) {
		/* **FOUR COLOURS, ONE PER QUARTER OF THE BUFFER.** A downscale that took the first pixel of each block, or a
		 * compositor that drew only the buffer's top-left quarter, would show one colour; a correct one shows all
		 * four, each in its own quarter of the placed window. */
		for (y = 0; y < client->buffer_height; y++) {
			for (x = 0; x < client->buffer_width; x++) {
				int right = x >= client->buffer_width / 2;
				int bottom = y >= client->buffer_height / 2;

				pixels[y * words + x] = bottom ? (right ? 0x00FFFFFFu : 0x000000FFu)
							       : (right ? 0x0000FF00u : 0x00FF0000u);
			}
		}
	} else {
		/* A FLAT COLOUR, and one that nothing else in the test uses: the assertion is that THIS colour is at the
		 * cascade's first position and the compositor's own background is everywhere else. */
		for (y = 0; y < client->buffer_height; y++) {
			for (x = 0; x < client->buffer_width; x++) {
				pixels[y * words + x] = client->colour;
			}
		}
	}
}

/* ONE SHARED-MEMORY BUFFER, MADE INTO THE SLOT IT IS ASKED FOR. `slot` is -1 for the plain single-buffer client,
 * which is what every other test here uses. */
static int create_buffer_slot(struct client *client, int slot)
{
	int stride = client->buffer_width * 4;
	size_t size = (size_t)stride * (size_t)client->buffer_height;
	struct wl_shm_pool *pool;
	struct wl_buffer **out = slot >= 0 ? &client->buffers[slot] : &client->buffer;
	uint32_t **pixels_out = slot >= 0 ? &client->buffer_pixels[slot] : &client->pixels;
	char name[64];
	int fd;
	void *data;

	snprintf(name, sizeof name, "/agora-window-server-client-%d-%d", (int)getpid(),
		 slot >= 0 ? slot : 0);
	fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		return -1;
	}
	shm_unlink(name);
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return -1;
	}
	*pixels_out = data;
	fill_pixels(client, data, stride);
	pool = wl_shm_create_pool(client->shm, fd, (int32_t)size);
	*out = wl_shm_pool_create_buffer(pool, 0, client->buffer_width, client->buffer_height, stride,
					 WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(*out, &buffer_listener, client);
	return 0;
}

static int create_buffer(struct client *client)
{
	return create_buffer_slot(client, -1);
}

#ifdef AWS_HAVE_GBM
/* **THE OTHER STORAGE PATH: A BUFFER THE GPU CAN SAMPLE WHERE IT WAS WRITTEN.** GBM allocates on the render node and
 * hands out a file descriptor; the compositor imports it once and samples it every frame, so the per-frame upload
 * shared memory needs does not happen. The buffer is LINEAR on purpose: it is the one modifier every driver
 * advertises, so the test exercises the protocol rather than a vendor's tiling. */
static int create_dmabuf_buffer(struct client *client)
{
	static const char *const nodes[] = { "/dev/dri/renderD128", "/dev/dri/renderD129", "/dev/dri/card0" };
	struct gbm_device *device = NULL;
	struct gbm_bo *bo = NULL;
	struct zwp_linux_buffer_params_v1 *params;
	uint64_t modifier;
	uint32_t stride = 0, offset;
	void *map, *map_data = NULL;
	int drm_fd = -1;
	int fd;
	size_t i;

	if (client->dmabuf == NULL) {
		fprintf(stderr, "client: --dmabuf asked for but the server does not offer zwp_linux_dmabuf_v1\n");
		return -1;
	}
	for (i = 0; i < sizeof nodes / sizeof nodes[0] && device == NULL; i++) {
		drm_fd = open(nodes[i], O_RDWR | O_CLOEXEC);
		if (drm_fd < 0) {
			continue;
		}
		device = gbm_create_device(drm_fd);
		if (device == NULL) {
			close(drm_fd);
			drm_fd = -1;
		}
	}
	if (device == NULL) {
		fprintf(stderr, "client: no render node could be opened for --dmabuf\n");
		return -1;
	}
	{
		uint64_t modifiers[1] = { DRM_FORMAT_MOD_LINEAR };

		bo = gbm_bo_create_with_modifiers(device, (uint32_t)client->buffer_width, (uint32_t)client->buffer_height,
						  DRM_FORMAT_XRGB8888, modifiers, 1);
	}
	if (bo == NULL) {
		fprintf(stderr, "client: gbm could not allocate a %dx%d buffer\n", client->buffer_width,
			client->buffer_height);
		goto fail;
	}
	map = gbm_bo_map(bo, 0, 0, (uint32_t)client->buffer_width, (uint32_t)client->buffer_height,
			 GBM_BO_TRANSFER_WRITE, &stride, &map_data);
	if (map == MAP_FAILED) {
		fprintf(stderr, "client: the GPU buffer could not be mapped\n");
		goto fail;
	}
	fill_pixels(client, map, (int)stride);
	gbm_bo_unmap(bo, map_data);
	map_data = NULL;

	fd = gbm_bo_get_fd(bo);
	if (fd < 0) {
		goto fail;
	}
	offset = gbm_bo_get_offset(bo, 0);
	modifier = gbm_bo_get_modifier(bo);
	params = zwp_linux_dmabuf_v1_create_params(client->dmabuf);
	zwp_linux_buffer_params_v1_add(params, fd, 0, offset, stride,
				       (uint32_t)(modifier >> 32), (uint32_t)modifier);
	client->buffer = zwp_linux_buffer_params_v1_create_immed(params, client->buffer_width,
								 client->buffer_height,
								 DRM_FORMAT_XRGB8888, 0);
	zwp_linux_buffer_params_v1_destroy(params);
	close(fd);
	/* **THE IMPORT HAS ITS OWN REFERENCE, SO THE CLIENT'S OBJECTS GO NOW.** The compositor made the EGLImage inside
	 * the `create_immed` request, synchronously, before this function returned. */
	gbm_bo_destroy(bo);
	gbm_device_destroy(device);
	close(drm_fd);
	if (client->buffer == NULL) {
		return -1;
	}
	wl_buffer_add_listener(client->buffer, &buffer_listener, client);
	return 0;

fail:
	if (map_data != NULL) {
		gbm_bo_unmap(bo, map_data);
	}
	if (bo != NULL) {
		gbm_bo_destroy(bo);
	}
	gbm_device_destroy(device);
	close(drm_fd);
	return -1;
}
#else /* !AWS_HAVE_GBM */
static int create_dmabuf_buffer(struct client *client)
{
	(void)client;
	fprintf(stderr, "client: --dmabuf needs a build with gbm (AWS_HAVE_GBM)\n");
	return -1;
}
#endif /* AWS_HAVE_GBM */

/* **THE OWNER'S HALF: WRITE AND CLOSE.** The fd is the write end of a pipe whose other end is already on its way to the
 * asker, so the server neither sees the bytes nor waits for them. */
static void selection_requested(void *data, struct agora_shell *shell, const char *mime_type, int32_t fd)
{
	struct client *client = data;
	size_t length;

	(void)shell;
	if (client->offer_payload == NULL || fd < 0) {
		close(fd);
		return;
	}
	length = strlen(client->offer_payload);
	if (write(fd, client->offer_payload, length) != (ssize_t)length) {
		fprintf(stderr, "client: could not write the selection payload\n");
	}
	close(fd);
	if (getenv("AG_CLIENT_DIAG") != NULL) {
		fprintf(stderr, "  DIAG selection: served %s (%zu byte(s))\n", mime_type, length);
	}
}

/* **THE READER'S HALF: READ TO EOF AND PRINT.** What lands on stdout is what the test asserts, so the payload crosses
 * two processes and a pipe without this program proving anything about itself. */
static void selection_sent(void *data, struct agora_shell *shell, const char *mime_type, int32_t fd)
{
	struct client *client = data;
	char buffer[4096];
	ssize_t got;

	(void)shell;
	client->selection_answered = 1;
	if (fd < 0) {
		return;
	}
	got = read(fd, buffer, sizeof buffer - 1);
	close(fd);
	if (got < 0) {
		got = 0;
	}
	buffer[got] = '\0';
	printf("selection %s: %s\n", mime_type, buffer);
	fflush(stdout);
}

static void selection_refused(void *data, struct agora_shell *shell, const char *mime_type, uint32_t reason)
{
	struct client *client = data;

	(void)shell;
	client->selection_refused = (int)reason + 1;
	printf("selection %s: refused (%u)\n", mime_type, (unsigned)reason);
	fflush(stdout);
}

static void selection_offer(void *data, struct agora_shell *shell, const char *mime_type)
{
	(void)data; (void)shell;
	if (getenv("AG_CLIENT_DIAG") != NULL) {
		fprintf(stderr, "  DIAG selection: offered %s\n", mime_type);
	}
}

static void selection_lost(void *data, struct agora_shell *shell)
{
	(void)data; (void)shell;
}

static const struct agora_shell_listener shell_listener = {
	selection_offer,
	selection_lost,
	selection_requested,
	selection_sent,
	selection_refused,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
			    uint32_t version)
{
	struct client *client = data;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		/* v4 IS WHAT `damage_buffer` ARRIVED IN, and binding less is a protocol error at the first call rather
		 * than a graceful degradation — which is how this was found. */
		client->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
						      version < 4 ? version : 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		client->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
		/* **v3 IS WHAT THIS SERVER OFFERS: THE FLAT (format, modifier) LIST.** Binding a higher version would be
		 * asking for feedback events the server will never send, which is a client that waits for something that
		 * cannot arrive. */
		client->dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface,
						  version < 3 ? version : 3);
	} else if (strcmp(interface, agora_shell_interface.name) == 0) {
		/* **SEVEN, WHICH IS WHERE THE SELECTION IS** — and clamped to what the server advertises, because a client
		 * cannot bind above its server and the protocol says so by refusing. */
		client->shell = wl_registry_bind(registry, name, &agora_shell_interface, version < 7 ? version : 7);
		if (client->shell != NULL && agora_shell_get_version(client->shell) >= 7) {
			agora_shell_add_listener(client->shell, &shell_listener, client);
		}
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_global_remove,
};

int main(int argc, char **argv)
{
	struct client client;
	int i;

	memset(&client, 0, sizeof client);
	client.width = 320;
	client.height = 200;
	client.buffer_scale = 1;
	client.colour = 0x00C050FFu;	/* a blue no background would be */
	client.max_frames = 3;
	client.frame_x = 100;
	client.frame_y = 80;
	client.level = 0;
	client.strip_y0 = 40;
	client.strip_y1 = 60;
	client.strip2_y0 = 120;
	client.strip2_y1 = 140;
	client.strip_colour = 0x0000FF00u;
	client.third_colour = 0x00FFFF00u;
	client.move_x = 600;
	client.move_y = 300;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
			client.width = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
			client.height = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--hold") == 0) {
			client.hold = 1;
		} else if (strcmp(argv[i], "--dmabuf") == 0) {
			client.use_dmabuf = 1;
		} else if (strcmp(argv[i], "--double") == 0) {
			client.double_buffer = 1;
		} else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			client.max_frames = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--colour") == 0 && i + 1 < argc) {
			client.colour = (uint32_t)strtoul(argv[++i], NULL, 16);
		} else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
			client.image_path = argv[++i];
		} else if (strcmp(argv[i], "--x") == 0 && i + 1 < argc) {
			client.frame_x = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--y") == 0 && i + 1 < argc) {
			client.frame_y = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
			client.level = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--buffer-scale") == 0 && i + 1 < argc) {
			client.buffer_scale = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--offer") == 0 && i + 1 < argc) {
			client.offer_type = argv[++i];
		} else if (strcmp(argv[i], "--payload") == 0 && i + 1 < argc) {
			client.offer_payload = argv[++i];
		} else if (strcmp(argv[i], "--request") == 0 && i + 1 < argc) {
			client.request_type = argv[++i];
		} else if (strcmp(argv[i], "--quadrants") == 0) {
			client.quadrants = 1;
		} else if (strcmp(argv[i], "--status-length") == 0 && i + 1 < argc) {
			client.status_length = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--damage") == 0 && i + 1 < argc) {
			const char *mode = argv[++i];

			if (strcmp(mode, "strip") == 0) {
				client.damage_mode = 1;
			} else if (strcmp(mode, "whole") == 0) {
				client.damage_mode = 2;
			} else if (strcmp(mode, "move") == 0) {
				client.damage_mode = 3;
			} else if (strcmp(mode, "strips") == 0) {
				client.damage_mode = 4;
			} else {
				fprintf(stderr, "client: --damage wants strip, whole or move\n");
				return 2;
			}
		} else if (strcmp(argv[i], "--strip") == 0 && i + 1 < argc) {
			/* Y0:Y1:RRGGBB, in BUFFER pixels — the space the strip's damage is named in. */
			char *value = argv[++i];
			char *second = strchr(value, ':');
			char *third = second != NULL ? strchr(second + 1, ':') : NULL;

			if (second == NULL || third == NULL) {
				fprintf(stderr, "client: --strip wants Y0:Y1:RRGGBB\n");
				return 2;
			}
			client.strip_y0 = atoi(value);
			client.strip_y1 = atoi(second + 1);
			client.strip_colour = (uint32_t)strtoul(third + 1, NULL, 16);
		} else if (strcmp(argv[i], "--third") == 0 && i + 1 < argc) {
			client.third_colour = (uint32_t)strtoul(argv[++i], NULL, 16);
		} else if (strcmp(argv[i], "--frame-delay") == 0 && i + 1 < argc) {
			client.frame_delay_ms = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--move-to") == 0 && i + 1 < argc) {
			const char *value = argv[++i];
			const char *comma = strchr(value, ',');

			if (comma == NULL) {
				fprintf(stderr, "client: --move-to wants X,Y\n");
				return 2;
			}
			client.move_x = atoi(value);
			client.move_y = atoi(comma + 1);
		}
	}

	if (client.buffer_scale < 1) {
		client.buffer_scale = 1;
	}
	client.buffer_width = client.width * client.buffer_scale;
	client.buffer_height = client.height * client.buffer_scale;

	client.display = wl_display_connect(NULL);
	if (client.display == NULL) {
		fprintf(stderr, "client: could not connect: %s\n", strerror(errno));
		return 1;
	}
	client.registry = wl_display_get_registry(client.display);
	wl_registry_add_listener(client.registry, &registry_listener, &client);
	wl_display_roundtrip(client.display);

	/* **THE SELECTION'S TWO ROLES LEAVE THE WINDOW BEHIND.** A selection test wants two processes talking through one
	 * server and nothing else; a window on each side would add a compositor to what is being measured. */
	if (client.offer_type != NULL || client.request_type != NULL) {
		if (client.shell == NULL || agora_shell_get_version(client.shell) < 7) {
			fprintf(stderr, "client: the server has no selection (agora_shell is below version 7)\n");
			return 2;
		}
		if (client.request_type != NULL) {
			agora_shell_request_selection(client.shell, client.request_type);
			wl_display_flush(client.display);
			/* UNTIL AN ANSWER OR A DEADLINE: an answer is an fd or a refusal, and both end the wait. */
			for (i = 0; i < 200 && !client.selection_answered && client.selection_refused == 0; i++) {
				wl_display_dispatch(client.display);
			}
			return (client.selection_answered || client.selection_refused != 0) ? 0 : 1;
		}
		{
			/* **THE BYTES OF THE LIST, NOT A LIST OF POINTERS**: see the protocol's own note. Each type is written
			 * with its terminating NUL. */
			struct wl_array types;
			size_t length = strlen(client.offer_type) + 1;
			char *slot;

			wl_array_init(&types);
			slot = (char *)wl_array_add(&types, length);
			memcpy(slot, client.offer_type, length);
			agora_shell_set_selection(client.shell, &types);
			wl_array_release(&types);
			/* **AND IT STAYS ALIVE TO SERVE**, because the owner produces the bytes on demand: a client that
			 * offered and exited would have offered nothing. */
			for (i = 0; i < 200; i++) {
				wl_display_dispatch(client.display);
			}
			return 0;
		}
	}
	if (client.compositor == NULL || client.shm == NULL || client.shell == NULL) {
		fprintf(stderr, "client: the compositor is missing a required global\n");
		return 1;
	}

	client.surface = wl_compositor_create_surface(client.compositor);
	/* **THE AGORA ROLE, AND NO CONFIGURE DANCE.** xdg-shell makes a client wait to be told how big to be; this
	 * protocol does not, because the Mac's model is that the APPLICATION states its frame and the server honours
	 * it. So the client says where it goes and draws — and what the test then checks is that the server put it
	 * exactly there, which is the whole claim being made. */
	client.window = agora_shell_get_window(client.shell, client.surface);
	/* THE FRAME IS THE LOGICAL SIZE — the size the window IS, which is what the compositor places and hit-tests. */
	agora_window_set_frame(client.window, client.frame_x, client.frame_y, client.width, client.height);
	/* **AND THE BUFFER IS THE SCALED SIZE.** This is the pair a HiDPI client sends: "my surface is width x height and
	 * my buffer is `scale` device pixels per logical one". A compositor that ignores the second number composites the
	 * buffer 1:1 and the window overflows its own frame by the factor — which is what this test exists to rule out. */
	if (client.buffer_scale > 1) {
		wl_surface_set_buffer_scale(client.surface, client.buffer_scale);
	}
	/* **AND IT ASKS FOR A FRAME AS WELL, ON PURPOSE.** What is measured then is the SERVER's placement winning,
	 * rather than a client that happened never to ask to be anywhere. */
	if (client.status_length > 0) {
		agora_window_set_status_item(client.window, client.status_length);
	}
	agora_window_set_level(client.window, client.level);
	agora_window_set_title(client.window, "window server test client");
	agora_window_set_app_id(client.window, "io.github.agoradesktop.test-client");

	if (client.image_path != NULL && load_jpeg(&client, client.image_path) != 0) {
		fprintf(stderr, "client: falling back to the flat colour\n");
	}
	if (client.double_buffer) {
		/* **A SWAPCHAIN IS SHARED MEMORY EVEN HERE**, because what this mode is about is LIFETIME and not storage:
		 * a dmabuf would add a second reason for a release to be interesting, and `--dmabuf` already tests the
		 * storage path. */
		if (create_buffer_slot(&client, 0) != 0 || create_buffer_slot(&client, 1) != 0) {
			fprintf(stderr, "client: could not make the two buffers\n");
			free(client.image_rgb);
			return 1;
		}
		client.slot = 0;
		client.pixels = client.buffer_pixels[0];
	} else {
		if (client.use_dmabuf && create_dmabuf_buffer(&client) != 0) {
			fprintf(stderr, "client: falling back to shared memory\n");
			client.use_dmabuf = 0;
		}
		if (!client.use_dmabuf && create_buffer(&client) != 0) {
			fprintf(stderr, "client: could not make a buffer\n");
			free(client.image_rgb);
			return 1;
		}
	}
	free(client.image_rgb);
	client.image_rgb = NULL;
	/* AND THEN ASK FOR A FRAME EACH TIME ONE LANDS, which is how a client that animates stays honest: the
	 * compositor's callback is the permission to draw again. */
	submit_frame(&client);
	while (client.frames < client.max_frames) {
		if (wl_display_dispatch(client.display) == -1) {
			break;
		}
	}
	/* AND THEN JUST LISTEN. Nothing is drawn again, so nothing is asked for again, and the surface keeps the
	 * buffer it already has — which is what a still picture is. */
	while (client.hold) {
		if (wl_display_dispatch(client.display) == -1) {
			break;
		}
	}
	printf("client: %d frame(s), %s %dx%d\n", client.frames,
	       client.image_path != NULL ? client.image_path : "flat colour", client.width, client.height);
	/* **THE LEDGER THE `--double` TEST IS ABOUT: one release per REPLACED buffer, and none for the one on the
	 * glass.** `tests/run-release.sh` reads these two numbers. */
	if (client.double_buffer) {
		printf("client: releases=%d while_current=%d\n", client.releases,
		       client.releases_while_current);
	}
	wl_display_disconnect(client.display);
	return client.frames > 0 ? 0 : 1;
}
