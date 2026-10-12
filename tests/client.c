/*
 * client — the smallest xdg-shell client, used to test the window server without libagora-app.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
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
 *   client [--width W] [--height H] [--frames N] [--colour RRGGBB]
 */

#define _GNU_SOURCE

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct client {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;

	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct wl_buffer *buffer;
	uint32_t *pixels;

	int width;
	int height;
	int configured;
	int frames;
	int max_frames;
	uint32_t colour;
};

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

/* ONE FRAME, IN THE ORDER THE PROTOCOL WANTS IT: the callback is requested BEFORE the commit it belongs to,
 * and the attach comes before the commit that presents it. */
static void submit_frame(struct client *client)
{
	struct wl_callback *callback = wl_surface_frame(client->surface);

	wl_callback_add_listener(callback, &frame_listener, client);
	wl_surface_attach(client->surface, client->buffer, 0, 0);
	wl_surface_damage_buffer(client->surface, 0, 0, client->width, client->height);
	wl_surface_commit(client->surface);
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
	(void)data; (void)buffer;
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { wm_base_ping };

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height,
			       struct wl_array *states)
{
	(void)data; (void)toplevel; (void)width; (void)height; (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
	(void)data; (void)toplevel;
}

static void toplevel_configure_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h)
{
	(void)d; (void)t; (void)w; (void)h;
}

static void toplevel_wm_capabilities(void *d, struct xdg_toplevel *t, struct wl_array *c)
{
	(void)d; (void)t; (void)c;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	toplevel_configure,
	toplevel_close,
	toplevel_configure_bounds,
	toplevel_wm_capabilities,
};

/* THE ACK, AND THEN THE DRAW. Everything here is the documented order and nothing is a shortcut: the ack
 * comes first, the buffer is attached after, and only the commit presents it. */
static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
	struct client *client = data;

	xdg_surface_ack_configure(xdg_surface, serial);
	client->configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = { xdg_surface_configure };

static int create_buffer(struct client *client)
{
	int stride = client->width * 4;
	size_t size = (size_t)stride * (size_t)client->height;
	struct wl_shm_pool *pool;
	char name[64];
	int fd;
	void *data;
	int x, y;

	snprintf(name, sizeof name, "/agora-window-server-client-%d", (int)getpid());
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
	client->pixels = data;
	/* A FLAT COLOUR, and one that nothing else in the test uses: the assertion is that THIS colour is at the
	 * cascade's first position and the compositor's own background is everywhere else. */
	for (y = 0; y < client->height; y++) {
		for (x = 0; x < client->width; x++) {
			client->pixels[y * client->width + x] = client->colour;
		}
	}
	pool = wl_shm_create_pool(client->shm, fd, (int32_t)size);
	client->buffer = wl_shm_pool_create_buffer(pool, 0, client->width, client->height, stride,
						   WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(client->buffer, &buffer_listener, client);
	return 0;
}

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
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		client->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(client->wm_base, &wm_base_listener, client);
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
	client.colour = 0x00C050FFu;	/* a blue no background would be */
	client.max_frames = 3;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
			client.width = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
			client.height = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			client.max_frames = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--colour") == 0 && i + 1 < argc) {
			client.colour = (uint32_t)strtoul(argv[++i], NULL, 16);
		}
	}

	client.display = wl_display_connect(NULL);
	if (client.display == NULL) {
		fprintf(stderr, "client: could not connect: %s\n", strerror(errno));
		return 1;
	}
	client.registry = wl_display_get_registry(client.display);
	wl_registry_add_listener(client.registry, &registry_listener, &client);
	wl_display_roundtrip(client.display);
	if (client.compositor == NULL || client.shm == NULL || client.wm_base == NULL) {
		fprintf(stderr, "client: the compositor is missing a required global\n");
		return 1;
	}

	client.surface = wl_compositor_create_surface(client.compositor);
	client.xdg_surface = xdg_wm_base_get_xdg_surface(client.wm_base, client.surface);
	xdg_surface_add_listener(client.xdg_surface, &xdg_surface_listener, &client);
	client.toplevel = xdg_surface_get_toplevel(client.xdg_surface);
	xdg_toplevel_add_listener(client.toplevel, &toplevel_listener, &client);
	xdg_toplevel_set_title(client.toplevel, "window server test client");

	/* THE EMPTY COMMIT: this is the one that asks to be configured, and it carries no buffer on purpose. */
	wl_surface_commit(client.surface);
	while (!client.configured && wl_display_dispatch(client.display) != -1) {
	}
	if (!client.configured) {
		fprintf(stderr, "client: the compositor never configured the surface\n");
		return 1;
	}

	if (create_buffer(&client) != 0) {
		fprintf(stderr, "client: could not make a buffer\n");
		return 1;
	}
	/* AND THEN ASK FOR A FRAME EACH TIME ONE LANDS, which is how a client that animates stays honest: the
	 * compositor's callback is the permission to draw again. */
	submit_frame(&client);
	while (client.frames < client.max_frames) {
		if (wl_display_dispatch(client.display) == -1) {
			break;
		}
	}
	printf("client: %d frame(s), colour %06X, %dx%d\n", client.frames, client.colour, client.width,
	       client.height);
	wl_display_disconnect(client.display);
	return client.frames > 0 ? 0 : 1;
}
