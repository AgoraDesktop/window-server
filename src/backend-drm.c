/*
 * backend-drm.c — a real output: Kernel Mode Setting, on the machine's own display.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * THIS IS THE SHIPPING BACKEND. The headless one is for tests and the X11 one is for development; this is the
 * one a person's screen is attached to, and the only backend that needs the kernel's permission rather than a
 * window from somebody else.
 *
 * IT USES THE ATOMIC API, BECAUSE THE LEGACY ONE IS INERT ON AN ATOMIC DRIVER. That is measured rather than
 * preferred: `drmModeSetCrtc` on nvidia-drm returns 0 and lights nothing, because the driver's own state shows
 * no encoder bound to a connector and no CRTC bound to an encoder — the legacy model is RECORDED while the
 * atomic one programmes the hardware. A successful ioctl and a lit pixel are not the same thing, and a
 * compositor that confused the two claimed a display it never drove.
 *
 * IT DOUBLE-BUFFERS AND FLIPS AT VBLANK, WHICH IS WHAT PAGES ARE. One buffer is being scanned out while the
 * compositor draws into the other, and `drmModeAtomicCommit` with DRM_MODE_PAGE_FLIP_EVENT hands the new one
 * over AT THE NEXT VBLANK — so the display never shows a frame that is half drawn. The alternative, which is
 * what this backend did first, is to draw straight into the memory being scanned and accept the tear.
 *
 * AND THE FLIP IS ASYNCHRONOUS, WHICH IS THE PART THAT IS EASY TO GET WRONG. With two buffers there is exactly
 * one to draw into, and it is free only once the PREVIOUS flip has completed — so the RENDER LOOP asks `busy`
 * before it draws, and `busy` answers yes until the flip event has been read off the DRM descriptor, which is on
 * the event loop. Drawing early would be drawing into a buffer the display may still be reading, which is the
 * tear it was avoiding wearing a different hat. The wait is still there; it is spent in the event loop, where
 * the rest of the compositor keeps working, instead of inside `present`. That also paces the compositor to the
 * display's refresh rather than to how fast it can paint — which is the correct rate, and the one clients are
 * told about through their frame callbacks.
 *
 * TWO THINGS IT MUST DO THAT A WINDOWED BACKEND NEVER DOES:
 *
 *   IT TAKES DRM MASTER, the kernel's one-claimant lock on the device. If another compositor holds it this
 *   FAILS and says so.
 *
 *   AND IT RELEASES THE DISPLAY ON THE WAY OUT, in the reverse of the order it was taken: the plane lets go of
 *   the framebuffer and the CRTC, the CRTC goes inactive, the connector lets go of the CRTC — and only then are
 *   the framebuffers and the buffers destroyed. A compositor that leaves a screen showing its last frame has
 *   taken a display away from whoever owns it, and one that destroys a buffer a live CRTC is scanning is worse.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AWS_HAVE_DRM

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

/* ONE PAGE. Two of these exist and the compositor always draws into the one that is not being scanned out. */
struct drm_buffer {
	uint32_t handle;
	uint32_t fb_id;
	uint32_t pitch;
	uint64_t size;
	void *map;
};

struct drm_state {
	int fd;
	drmModeRes *resources;
	uint32_t crtc_id;
	uint32_t connector_id;
	uint32_t plane_id;		/* the PRIMARY plane of that CRTC */
	uint32_t blob_id;		/* the mode, in the form an atomic request wants */
	struct drm_buffer buffers[2];
	int front;			/* which of the two is being scanned out */
	int flip_pending;
	int mode_is_set;
	struct wl_event_source *source;	/* the DRM descriptor, on the event loop */
};

/* ---------------------------------------------------------------------------------------------------
 * the atomic API
 * --------------------------------------------------------------------------------------------------- */

/* A PROPERTY BY NAME. It answers the property's ID, because that is what `drmModeAtomicAddProperty` wants, and
 * writes out its CURRENT VALUE, because that is what choosing a PRIMARY plane needs — two questions in one walk
 * of the property list. Zero means the object has no such property. */
static uint32_t drm_property(int fd, uint32_t object_id, uint32_t object_type, const char *name, uint64_t *value)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, object_id, object_type);
	uint32_t found = 0;
	uint32_t i;

	if (props == NULL) {
		return 0;
	}
	for (i = 0; i < props->count_props && found == 0; i++) {
		drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);

		if (prop == NULL) {
			continue;
		}
		if (strcmp(prop->name, name) == 0) {
			found = prop->prop_id;
			if (value != NULL) {
				*value = props->prop_values[i];
			}
		}
		drmModeFreeProperty(prop);
	}
	drmModeFreeObjectProperties(props);
	return found;
}

static int drm_atomic_set(int fd, drmModeAtomicReq *req, uint32_t object_id, uint32_t object_type,
			  const char *name, uint64_t value)
{
	uint32_t prop = drm_property(fd, object_id, object_type, name, NULL);

	if (prop == 0) {
		fprintf(stderr, "window-server: object %u has no `%s` property\n", object_id, name);
		return -1;
	}
	return drmModeAtomicAddProperty(req, object_id, prop, value) < 0 ? -1 : 0;
}

/* THE PRIMARY PLANE FOR A CRTC: the one the kernel calls PRIMARY and that this CRTC can drive. Both conditions
 * matter — without the second a compositor can be handed a plane it cannot route to; without the first a cursor
 * plane. */
static uint32_t drm_primary_plane(int fd, uint32_t crtc_id, const uint32_t *crtcs, int count)
{
	drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
	uint32_t result = 0;
	int index = -1;
	int i;

	for (i = 0; i < count; i++) {
		if (crtcs[i] == crtc_id) {
			index = i;
		}
	}
	if (planes == NULL || index < 0) {
		if (planes != NULL) {
			drmModeFreePlaneResources(planes);
		}
		return 0;
	}
	for (i = 0; i < (int)planes->count_planes && result == 0; i++) {
		drmModePlane *plane = drmModeGetPlane(fd, planes->planes[i]);
		uint64_t type = 0;

		if (plane == NULL) {
			continue;
		}
		if ((plane->possible_crtcs & (1u << index)) != 0 &&
		    drm_property(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type) != 0 &&
		    type == DRM_PLANE_TYPE_PRIMARY) {
			result = plane->plane_id;
		}
		drmModeFreePlane(plane);
	}
	drmModeFreePlaneResources(planes);
	return result;
}

/* THE PLANE'S HALF OF EVERY COMMIT: which buffer it scans, and which part of it goes where. Called by both the
 * initial modeset and every flip, so there is one place where the rectangles are spelled — the source in 16.16
 * fixed point and the destination in pixels, two units for one rectangle, which is where a half-sized mode
 * comes from. */
static int drm_set_plane(struct aws_server *server, drmModeAtomicReq *req, uint32_t fb_id)
{
	struct drm_state *drm = server->backend.data;
	uint64_t w = (uint64_t)server->backend.width;
	uint64_t h = (uint64_t)server->backend.height;

	drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", fb_id);
	drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", drm->crtc_id);
	drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", 0);
	drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0);
	drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", w << 16);
	drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", h << 16);
	drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0);
	drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0);
	drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", w);
	drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", h);
	return 0;
}

/* **THE MODESET, AND THE ONLY STEP THAT MAKES LIGHT.** An atomic modeset is a list of (object, property, value)
 * the kernel applies together or not at all: the connector is pointed at the CRTC, the CRTC at the mode blob
 * and told to be ACTIVE, and the plane at a framebuffer. */
static int drm_atomic_modeset(struct aws_server *server, int on)
{
	struct drm_state *drm = server->backend.data;
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	int ret;

	if (req == NULL) {
		return -1;
	}
	if (on) {
		drm_atomic_set(drm->fd, req, drm->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID",
			       drm->crtc_id);
		drm_atomic_set(drm->fd, req, drm->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", drm->blob_id);
		drm_atomic_set(drm->fd, req, drm->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
		drm_set_plane(server, req, drm->buffers[0].fb_id);
	} else {
		/* TURNING IT OFF, AND IN THE REVERSE OF THE ORDER THAT SET IT UP: the plane lets go of the framebuffer
		 * and the CRTC, then the CRTC goes inactive, then the connector lets go of the CRTC. Clearing ACTIVE
		 * while the plane still holds the framebuffer is EINVAL — measured, on every exit, before this order
		 * was right. */
		drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", 0);
		drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", 0);
		drm_atomic_set(drm->fd, req, drm->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 0);
		drm_atomic_set(drm->fd, req, drm->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", 0);
	}
	ret = drmModeAtomicCommit(drm->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (ret != 0) {
		fprintf(stderr, "window-server: the atomic %s was refused: %s\n", on ? "modeset" : "teardown",
			strerror(errno));
	}
	drmModeAtomicFree(req);
	return ret;
}

/* ---------------------------------------------------------------------------------------------------
 * the flip
 * --------------------------------------------------------------------------------------------------- */

static int drm_busy(struct aws_server *server)
{
	struct drm_state *drm = server->backend.data;

	return drm->flip_pending;
}

/* THE FLIP HAPPENED: the page being scanned is free, so `drm_busy` answers no and the render loop draws again.
 * Nothing else is needed — the frame it skipped left `render_pending` set, and the loop comes back round. */
static void drm_page_flip(int fd, unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec, void *data)
{
	struct drm_state *drm = data;

	(void)fd; (void)sequence; (void)tv_sec; (void)tv_usec;
	drm->flip_pending = 0;
}

/* **THE DRM DESCRIPTOR IS ON THE EVENT LOOP, WHICH IS WHAT REPLACED THE WAIT.** A flip event is read here, and
 * reading it is what makes the next frame possible. */
static int drm_dispatch(int fd, uint32_t mask, void *data)
{
	struct aws_server *server = data;
	drmEventContext context;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		fprintf(stderr, "window-server: the DRM descriptor went away\n");
		server->running = 0;
		return 0;
	}
	memset(&context, 0, sizeof context);
	context.version = DRM_EVENT_CONTEXT_VERSION;
	context.page_flip_handler = drm_page_flip;
	if (drmHandleEvent(fd, &context) != 0) {
		fprintf(stderr, "window-server: reading a DRM event failed: %s\n", strerror(errno));
	}
	return 0;
}

/* **THE PRESENT, WHICH NO LONGER WAITS.** It commits the flip and returns; the event that says the flip happened
 * arrives on the DRM descriptor, which is on the event loop, and only then does the render loop draw again —
 * because `drm_busy` is what it asks first. The wait is still there; it is spent in the event loop, where the
 * rest of the compositor keeps working, instead of inside this function, where nothing could. */
static void drm_present(struct aws_server *server)
{
	struct drm_state *drm = server->backend.data;
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	int back = 1 - drm->front;
	int ret;

	if (req == NULL || drm->buffers[back].fb_id == 0) {
		if (req != NULL) {
			drmModeAtomicFree(req);
		}
		return;
	}
	drm_set_plane(server, req, drm->buffers[back].fb_id);
	/* NONBLOCK BECAUSE THE POINT IS TO GET TO THE EVENT: the flip happens at the next vblank and
	 * DRM_MODE_PAGE_FLIP_EVENT is what tells this process when it has. */
	drm->flip_pending = 1;
	ret = drmModeAtomicCommit(drm->fd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, drm);
	drmModeAtomicFree(req);
	if (ret != 0) {
		fprintf(stderr, "window-server: the page flip was refused: %s\n", strerror(errno));
		drm->flip_pending = 0;
		return;
	}
	drm->front = back;
	/* AND THE COMPOSITOR DRAWS INTO THE OTHER ONE NEXT TIME. */
	server->backend.pixels = drm->buffers[1 - back].map;
}

/* ---------------------------------------------------------------------------------------------------
 * start and stop
 * --------------------------------------------------------------------------------------------------- */

static void drm_destroy_buffer(struct drm_state *drm, struct drm_buffer *buffer)
{
	if (buffer->fb_id != 0) {
		drmModeRmFB(drm->fd, buffer->fb_id);
		buffer->fb_id = 0;
	}
	if (buffer->map != NULL && buffer->size > 0) {
		munmap(buffer->map, (size_t)buffer->size);
		buffer->map = NULL;
	}
	if (buffer->handle != 0) {
		struct drm_mode_destroy_dumb destroy;

		memset(&destroy, 0, sizeof destroy);
		destroy.handle = buffer->handle;
		drmIoctl(drm->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		buffer->handle = 0;
	}
}

static void drm_stop(struct aws_server *server)
{
	struct drm_state *drm = server->backend.data;
	int i;

	if (drm == NULL) {
		return;
	}
	if (drm->mode_is_set) {
		drm_atomic_modeset(server, 0);
		drm->mode_is_set = 0;
	}
	if (drm->source != NULL) {
		wl_event_source_remove(drm->source);
		drm->source = NULL;
	}
	if (drm->blob_id != 0) {
		drmModeDestroyPropertyBlob(drm->fd, drm->blob_id);
		drm->blob_id = 0;
	}
	for (i = 0; i < 2; i++) {
		drm_destroy_buffer(drm, &drm->buffers[i]);
	}
	if (drm->fd >= 0) {
		drmDropMaster(drm->fd);
	}
	if (drm->resources != NULL) {
		drmModeFreeResources(drm->resources);
	}
	if (drm->fd >= 0) {
		close(drm->fd);
	}
	free(drm);
	server->backend.data = NULL;
	server->backend.pixels = NULL;
}

/* A PAGE: kernel memory, mapped into this process, and a framebuffer the atomic API can scan. */
static int drm_create_buffer(struct drm_state *drm, int width, int height, struct drm_buffer *buffer)
{
	struct drm_mode_create_dumb create;
	struct drm_mode_map_dumb map;

	memset(&create, 0, sizeof create);
	create.width = (uint32_t)width;
	create.height = (uint32_t)height;
	create.bpp = 32;
	if (drmIoctl(drm->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
		fprintf(stderr, "window-server: could not allocate a %dx%d buffer: %s\n", width, height,
			strerror(errno));
		return -1;
	}
	buffer->handle = create.handle;
	buffer->pitch = create.pitch;
	buffer->size = create.size;
	memset(&map, 0, sizeof map);
	map.handle = create.handle;
	if (drmIoctl(drm->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
		fprintf(stderr, "window-server: could not map a buffer: %s\n", strerror(errno));
		return -1;
	}
	buffer->map = mmap(NULL, (size_t)buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED, drm->fd,
			   (off_t)map.offset);
	if (buffer->map == MAP_FAILED) {
		buffer->map = NULL;
		fprintf(stderr, "window-server: mmap of a buffer failed: %s\n", strerror(errno));
		return -1;
	}
	/* 24 DEPTH AND 32 BITS PER PIXEL IS XRGB8888, the format every client here sends and the one the
	 * compositing loop writes. */
	if (drmModeAddFB(drm->fd, create.width, create.height, 24, 32, buffer->pitch, buffer->handle,
			 &buffer->fb_id) != 0) {
		fprintf(stderr, "window-server: could not make a framebuffer: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

int aws_backend_drm_start(struct aws_server *server)
{
	const char *path = getenv("AWS_DRM_DEVICE") != NULL ? getenv("AWS_DRM_DEVICE") : "/dev/dri/card0";
	struct drm_state *drm;
	drmModeConnector *connector = NULL;
	drmModeModeInfo mode;
	int i;

	drm = calloc(1, sizeof *drm);
	if (drm == NULL) {
		return -1;
	}
	/* SET BEFORE ANYTHING CAN FAIL, so that every `drm_stop` on the way out has something to clean up. */
	server->backend.data = drm;
	drm->fd = -1;

	drm->fd = open(path, O_RDWR | O_CLOEXEC);
	if (drm->fd < 0) {
		fprintf(stderr, "window-server: could not open %s: %s\n", path, strerror(errno));
		drm_stop(server);
		return -1;
	}
	/* **ATOMIC IS NOT AN OPTION HERE, IT IS THE API.** The legacy modeset is accepted by nvidia-drm and lights
	 * nothing, so a build that found no atomic support would be a compositor that reports success and shows
	 * nothing — worse than one that refuses. */
	if (drmSetClientCap(drm->fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
		fprintf(stderr, "window-server: %s does not support atomic modesetting, which this backend "
				"requires\n", path);
		drm_stop(server);
		return -1;
	}
	/* THE RESOURCES FIRST, SO THAT "NO DISPLAY" IS REPORTED AS THAT AND NOT AS A PERMISSION PROBLEM. */
	drm->resources = drmModeGetResources(drm->fd);
	if (drm->resources == NULL) {
		fprintf(stderr,
			"window-server: %s has no KMS resources at all. On a machine with the proprietary NVIDIA\n"
			"                driver this is KMS being OFF IN THE KERNEL: `nvidia_drm.modeset=1` (a module\n"
			"                parameter, which needs the initramfs rebuilt and a reboot) is what turns a card\n"
			"                that can render into one that can display.\n",
			path);
		drm_stop(server);
		return -1;
	}
	for (i = 0; i < drm->resources->count_connectors && connector == NULL; i++) {
		drmModeConnector *candidate = drmModeGetConnector(drm->fd, drm->resources->connectors[i]);

		if (candidate == NULL) {
			continue;
		}
		if (candidate->connection == DRM_MODE_CONNECTED && candidate->count_modes > 0) {
			connector = candidate;
		} else {
			drmModeFreeConnector(candidate);
		}
	}
	if (connector == NULL) {
		fprintf(stderr,
			"window-server: %s has %d connector(s) and NONE is connected with a mode, so there is no\n"
			"                display to drive.\n",
			path, drm->resources->count_connectors);
		drm_stop(server);
		return -1;
	}
	{
		const drmModeModeInfo *found = NULL;

		for (i = 0; i < connector->count_modes && found == NULL; i++) {
			if (connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
				found = &connector->modes[i];
			}
		}
		if (found == NULL && connector->count_modes > 0) {
			found = &connector->modes[0];
		}
		if (found == NULL) {
			fprintf(stderr, "window-server: the connected display offers no modes\n");
			drmModeFreeConnector(connector);
			drm_stop(server);
			return -1;
		}
		mode = *found;	/* a copy: the connector's own belongs to drmModeGetConnector's allocation */
	}
	/* A CRTC THIS CONNECTOR CAN BE DRIVEN BY, chosen from what the encoder says it can reach rather than by
	 * taking the first one and hoping. */
	drm->crtc_id = 0;
	for (i = 0; i < connector->count_encoders && drm->crtc_id == 0; i++) {
		drmModeEncoder *encoder = drmModeGetEncoder(drm->fd, connector->encoders[i]);
		int k;

		if (encoder == NULL) {
			continue;
		}
		for (k = 0; k < drm->resources->count_crtcs && drm->crtc_id == 0; k++) {
			if ((encoder->possible_crtcs & (1u << k)) != 0) {
				drm->crtc_id = drm->resources->crtcs[k];
			}
		}
		drmModeFreeEncoder(encoder);
	}
	if (drm->crtc_id == 0) {
		fprintf(stderr, "window-server: found a connected display but no CRTC it can be driven by\n");
		drmModeFreeConnector(connector);
		drm_stop(server);
		return -1;
	}
	drm->connector_id = connector->connector_id;

	/* **MASTER IS THE CLAIM ON THE DEVICE, AND IT FAILS WHEN SOMEBODY ELSE HAS THE DISPLAY.** */
	if (drmSetMaster(drm->fd) != 0) {
		fprintf(stderr, "window-server: %s is held by another DRM master (an X server or another\n"
				"                compositor); this cannot take the display while that is running\n",
			path);
		drmModeFreeConnector(connector);
		drm_stop(server);
		return -1;
	}

	/* **TWO PAGES**, because one buffer cannot be both scanned and drawn. */
	for (i = 0; i < 2; i++) {
		if (drm_create_buffer(drm, mode.hdisplay, mode.vdisplay, &drm->buffers[i]) != 0) {
			drmModeFreeConnector(connector);
			drm_stop(server);
			return -1;
		}
	}
	/* THE PRIMARY PLANE, WHICH AN ATOMIC REQUEST NAMES AND `drmModeSetCrtc` ONLY IMPLIED. */
	drm->plane_id = drm_primary_plane(drm->fd, drm->crtc_id, drm->resources->crtcs,
					  drm->resources->count_crtcs);
	if (drm->plane_id == 0) {
		fprintf(stderr, "window-server: no PRIMARY plane for CRTC %u\n", drm->crtc_id);
		drmModeFreeConnector(connector);
		drm_stop(server);
		return -1;
	}
	/* AND THE MODE AS A BLOB, which is how an atomic request carries one. */
	if (drmModeCreatePropertyBlob(drm->fd, &mode, sizeof mode, &drm->blob_id) != 0) {
		fprintf(stderr, "window-server: could not make a mode blob: %s\n", strerror(errno));
		drmModeFreeConnector(connector);
		drm_stop(server);
		return -1;
	}
	server->backend.width = (int)mode.hdisplay;
	server->backend.height = (int)mode.vdisplay;

	/* **THE DRY RUN: EVERY STEP UP TO THE COMMIT AND NOTHING AFTER IT.** Taking a display away from the session
	 * using it is the most disruptive thing any backend in this tree can do, so the whole preparation can be
	 * proven — device, connector, mode, CRTC, plane, both buffers, both framebuffers, the blob — WITHOUT the
	 * commit that makes light. `AWS_DRM_DRY_RUN=1` asks for it. */
	if (getenv("AWS_DRM_DRY_RUN") != NULL) {
		fprintf(stderr,
			"window-server: DRY RUN — every step up to the commit succeeded, and NOTHING was committed:\n"
			"                device      %s\n"
			"                connector   %u (%s), %u mode(s)\n"
			"                mode        %ux%u%s\n"
			"                crtc        %u\n"
			"                plane       %u (PRIMARY)\n"
			"                pages       2 x (%ux%u, pitch %u, %llu bytes) at %p and %p\n"
			"                framebuffers %u and %u, mode blob %u\n"
			"                Master was taken and is given back; no pixel moves.\n",
			path, drm->connector_id,
			drmModeGetConnectorTypeName(connector->connector_type) != NULL
				? drmModeGetConnectorTypeName(connector->connector_type) : "?",
			connector->count_modes, mode.hdisplay, mode.vdisplay,
			(mode.type & DRM_MODE_TYPE_PREFERRED) ? " (PREFERRED)" : "", drm->crtc_id, drm->plane_id,
			mode.hdisplay, mode.vdisplay, drm->buffers[0].pitch,
			(unsigned long long)drm->buffers[0].size, drm->buffers[0].map, drm->buffers[1].map,
			drm->buffers[0].fb_id, drm->buffers[1].fb_id, drm->blob_id);
		drmModeFreeConnector(connector);
		drm_stop(server);
		return -1;
	}

	drmModeFreeConnector(connector);

	/* **THE COMMIT — THE ONE STEP THAT MAKES LIGHT.** Everything above it was preparation. */
	if (drm_atomic_modeset(server, 1) != 0) {
		drm_stop(server);
		return -1;
	}
	drm->mode_is_set = 1;
	/* PAGE 0 IS NOW ON THE SCREEN, SO THE COMPOSITOR STARTS ON PAGE 1. */
	drm->front = 0;
	server->backend.pixels = drm->buffers[1].map;
	server->backend.name = "drm";
	server->backend.data = drm;
	server->backend.present = drm_present;
	server->backend.busy = drm_busy;
	server->backend.stop = drm_stop;
	/* AND THE DESCRIPTOR GOES ON THE LOOP, which is where the flip events are read. */
	drm->source = wl_event_loop_add_fd(server->loop, drm->fd, WL_EVENT_READABLE, drm_dispatch, server);
	fprintf(stderr, "window-server: driving %s at %ux%u (atomic, 2 pages, async flip: connector %u, crtc %u, plane %u)\n",
		path, mode.hdisplay, mode.vdisplay, drm->connector_id, drm->crtc_id, drm->plane_id);
	return 0;
}

#else /* !AWS_HAVE_DRM */

int aws_backend_drm_start(struct aws_server *server)
{
	(void)server;
	fprintf(stderr, "window-server: this build has no drm backend (configured without libdrm)\n");
	return -1;
}

#endif
