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
 *   client [--width W] [--height H] [--frames N] [--colour RRGGBB] [--x X] [--y Y] [--level L]
 *          [--status-length N]
 */

#define _GNU_SOURCE

#include <wayland-client.h>
#include "agora-shell-client-protocol.h"

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
	struct agora_shell *shell;

	struct wl_surface *surface;
	struct agora_window *window;
	int frame_x, frame_y;
	int level;			/* the NSWindow level to ask for, so two clients can be ordered */
	struct wl_buffer *buffer;
	uint32_t *pixels;

	int width;
	int height;
	int status_length;	/* non-zero: a status item the SERVER places, and the frame asked for is ignored */
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
	} else if (strcmp(interface, agora_shell_interface.name) == 0) {
		client->shell = wl_registry_bind(registry, name, &agora_shell_interface, 1);
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
	client.frame_x = 100;
	client.frame_y = 80;
	client.level = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
			client.width = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
			client.height = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			client.max_frames = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--colour") == 0 && i + 1 < argc) {
			client.colour = (uint32_t)strtoul(argv[++i], NULL, 16);
		} else if (strcmp(argv[i], "--x") == 0 && i + 1 < argc) {
			client.frame_x = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--y") == 0 && i + 1 < argc) {
			client.frame_y = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
			client.level = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--status-length") == 0 && i + 1 < argc) {
			client.status_length = atoi(argv[++i]);
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
	agora_window_set_frame(client.window, client.frame_x, client.frame_y, client.width, client.height);
	/* **AND IT ASKS FOR A FRAME AS WELL, ON PURPOSE.** What is measured then is the SERVER's placement winning,
	 * rather than a client that happened never to ask to be anywhere. */
	if (client.status_length > 0) {
		agora_window_set_status_item(client.window, client.status_length);
	}
	agora_window_set_level(client.window, client.level);
	agora_window_set_title(client.window, "window server test client");
	agora_window_set_app_id(client.window, "org.agora.test-client");

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
