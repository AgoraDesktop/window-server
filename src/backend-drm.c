/*
 * backend-drm.c — a real output: Kernel Mode Setting, on the machine's own display.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
 *
 * THIS IS THE SHIPPING BACKEND. The headless one is for tests and the X11 one is for development; this is the
 * one a person's screen is attached to, and it is the only backend that needs the kernel's permission rather
 * than a window from somebody else.
 *
 * IT USES THE ATOMIC API, BECAUSE THE LEGACY ONE IS INERT ON AN ATOMIC DRIVER. That is measured rather than
 * preferred. `drmModeSetCrtc` on this machine's nvidia-drm returns 0 and lights nothing, while the driver's own
 * state says why:
 *
 *     connector 77: CONNECTED, encoder_id=0        encoder 76: crtc_id=0
 *     connector 79: CONNECTED, encoder_id=0        encoder 78: crtc_id=0
 *
 * NOTHING IS BOUND TO ANYTHING in the legacy model — no encoder on a connector, no CRTC on an encoder, no
 * framebuffer on a CRTC — and a desktop is nonetheless on the screen, so the legacy model is being RECORDED
 * while the atomic one programmes the hardware. A successful ioctl and a lit pixel are not the same thing, and
 * a compositor that confused the two claimed a display it never drove.
 *
 * IT IS A DUMB-BUFFER SCANOUT AND NOT A PAGE-FLIPPING RENDERER, which is the first version on purpose. The
 * kernel allocates the pixels, this process maps them, and the plane is pointed at them once — so the
 * compositing loop writes straight into the memory the display is scanning out. What that costs is TEARING:
 * a frame is visible while it is still being drawn. What it buys is a bring-up path with no GBM, no EGL and no
 * GPU driver in the picture. Page flipping (`DRM_MODE_PAGE_FLIP_EVENT` on a second buffer) is the next
 * refinement and the backend interface already has the shape for it.
 *
 * TWO THINGS IT MUST DO THAT A WINDOWED BACKEND NEVER DOES:
 *
 *   IT TAKES DRM MASTER, the kernel's one-claimant lock on the device. If another compositor holds it this
 *   FAILS and says so — the safety property that makes the backend hard to run by accident.
 *
 *   AND IT RELEASES THE DISPLAY ON THE WAY OUT, in the right order: the CRTC is turned off and the connector
 *   detached before the framebuffer and the buffer under it are destroyed. A compositor that leaves a screen
 *   showing its last frame has taken a display away from whoever owns it — and one that destroys a buffer a
 *   live CRTC is scanning is worse.
 *
 * WHEN IT FAILS HERE, IT SAYS WHAT TO DO. The usual reason on a machine with a proprietary NVIDIA driver is
 * modesetting being off IN THE KERNEL (`nvidia_drm.modeset=1`), and the symptom is a card with no connectors at
 * all — one that can render and cannot display.
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

struct drm_state {
	int fd;
	drmModeRes *resources;
	uint32_t crtc_id;
	uint32_t connector_id;
	uint32_t plane_id;		/* the PRIMARY plane of that CRTC */
	uint32_t blob_id;		/* the mode, in the form an atomic request wants */
	uint32_t fb_id;
	uint32_t handle;
	uint32_t pitch;
	uint64_t size;
	void *map;
	int mode_is_set;		/* the commit happened, so there is something to turn OFF */
};

/* THE MODE: the connector's PREFERRED one, or its first, because a preferred mode is the panel's own answer to
 * "what shape are you" and anything else is this process guessing. */
static const drmModeModeInfo *preferred_mode(drmModeConnector *connector)
{
	int i;

	for (i = 0; i < connector->count_modes; i++) {
		if (connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			return &connector->modes[i];
		}
	}
	return connector->count_modes > 0 ? &connector->modes[0] : NULL;
}

/* A CRTC THAT CAN DRIVE THIS CONNECTOR, chosen by what the encoder says it is able to reach rather than by
 * taking the first one and hoping. */
static uint32_t crtc_for_connector(struct drm_state *drm, drmModeConnector *connector)
{
	int i, k;

	for (i = 0; i < connector->count_encoders; i++) {
		drmModeEncoder *encoder = drmModeGetEncoder(drm->fd, connector->encoders[i]);

		if (encoder == NULL) {
			continue;
		}
		if (encoder->crtc_id != 0) {
			uint32_t id = encoder->crtc_id;

			drmModeFreeEncoder(encoder);
			return id;
		}
		for (k = 0; k < drm->resources->count_crtcs; k++) {
			if ((encoder->possible_crtcs & (1u << k)) != 0) {
				uint32_t id = drm->resources->crtcs[k];

				drmModeFreeEncoder(encoder);
				return id;
			}
		}
		drmModeFreeEncoder(encoder);
	}
	return 0;
}

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
 * matter — without the second a compositor can be handed a plane it cannot route to; without the first it can be
 * handed a cursor plane. */
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

/* **THE MODESET, AND THE ONLY STEP THAT MAKES LIGHT.** An atomic modeset is a list of (object, property, value)
 * that the kernel applies TOGETHER or not at all: the connector is pointed at the CRTC, the CRTC at the mode
 * blob and told to be ACTIVE, and the primary plane at the framebuffer with the rectangles that say which part
 * of it goes where. */
static int drm_atomic_modeset(struct aws_server *server, int on)
{
	struct drm_state *drm = server->backend.data;
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	uint64_t w = (uint64_t)server->backend.width;
	uint64_t h = (uint64_t)server->backend.height;
	int ret;

	if (req == NULL) {
		return -1;
	}
	if (on) {
		drm_atomic_set(drm->fd, req, drm->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID",
			       drm->crtc_id);
		drm_atomic_set(drm->fd, req, drm->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", drm->blob_id);
		drm_atomic_set(drm->fd, req, drm->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
		drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", drm->fb_id);
		drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", drm->crtc_id);
		drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", 0);
		drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0);
		/* THE SOURCE RECTANGLE IS IN 16.16 FIXED POINT and the destination is in pixels — two units for the
		 * same rectangle, which is where a mode that comes out half-sized comes from. */
		drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", w << 16);
		drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", h << 16);
		drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0);
		drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0);
		drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", w);
		drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", h);
	} else {
		/* TURNING IT OFF, AND BEFORE THE FRAMEBUFFER GOES: a CRTC still scanning out something about to be
		 * destroyed is refused by the kernel at best and a stale scan-out at worst. */
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

/* **AND `present`, WHICH IS NOT OPTIONAL EITHER.** The frame buffer IS what the display scans out, so there is
 * nothing to hand over — but a driver still has to be TOLD the memory it is scanning has changed, or it shows
 * the first frame and never another. That is `drmModeDirtyFB`, and without it a compositor paints into memory
 * nobody reads. */
static void drm_present(struct aws_server *server)
{
	struct drm_state *drm = server->backend.data;

	if (drm->fb_id != 0) {
		drmModeDirtyFB(drm->fd, drm->fb_id, NULL, 0);
	}
}

/* ---------------------------------------------------------------------------------------------------
 * start and stop
 * --------------------------------------------------------------------------------------------------- */

static void drm_stop(struct aws_server *server)
{
	struct drm_state *drm = server->backend.data;

	if (drm == NULL) {
		return;
	}
	/* **THE DISPLAY IS RELEASED FIRST, AND IN THIS ORDER.** The CRTC is turned off and the connector detached
	 * BEFORE the framebuffer and the buffer under it are destroyed, because a CRTC still scanning them out makes
	 * that destruction refused at best and a stale scan-out at worst. And it is released at all because a
	 * compositor that leaves the screen showing its last frame has taken a display away from whoever owns it. */
	if (drm->mode_is_set) {
		drm_atomic_modeset(server, 0);
		drm->mode_is_set = 0;
	}
	if (drm->blob_id != 0) {
		drmModeDestroyPropertyBlob(drm->fd, drm->blob_id);
		drm->blob_id = 0;
	}
	if (drm->fb_id != 0) {
		drmModeRmFB(drm->fd, drm->fb_id);
		drm->fb_id = 0;
	}
	if (drm->map != NULL && drm->size > 0) {
		munmap(drm->map, (size_t)drm->size);
		drm->map = NULL;
	}
	if (drm->handle != 0) {
		struct drm_mode_destroy_dumb destroy;

		memset(&destroy, 0, sizeof destroy);
		destroy.handle = drm->handle;
		drmIoctl(drm->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		drm->handle = 0;
	}
	/* THE CLAIM IS GIVEN BACK, so whoever had the display before can have it again. */
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

int aws_backend_drm_start(struct aws_server *server)
{
	const char *path = getenv("AWS_DRM_DEVICE") != NULL ? getenv("AWS_DRM_DEVICE") : "/dev/dri/card0";
	struct drm_state *drm;
	drmModeConnector *connector = NULL;
	drmModeModeInfo mode;
	struct drm_mode_create_dumb create;
	struct drm_mode_map_dumb map;
	uint64_t offset = 0;
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
	 * nothing — which is worse than one that refuses. */
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
		const drmModeModeInfo *found = preferred_mode(connector);

		if (found == NULL) {
			fprintf(stderr, "window-server: the connected display offers no modes\n");
			drmModeFreeConnector(connector);
			drm_stop(server);
			return -1;
		}
		mode = *found;	/* a copy: the connector's own belongs to drmModeGetConnector's allocation */
	}
	drm->crtc_id = crtc_for_connector(drm, connector);
	if (drm->crtc_id == 0) {
		fprintf(stderr, "window-server: found a connected display but no CRTC it can be driven by\n");
		drmModeFreeConnector(connector);
		drm_stop(server);
		return -1;
	}
	drm->connector_id = connector->connector_id;

	/* **MASTER IS THE CLAIM ON THE DEVICE, AND IT FAILS WHEN SOMEBODY ELSE HAS THE DISPLAY.** That failure is
	 * the reason this backend is hard to start by accident: an X server or another compositor holding master
	 * keeps it. */
	if (drmSetMaster(drm->fd) != 0) {
		fprintf(stderr, "window-server: %s is held by another DRM master (an X server or another\n"
				"                compositor); this cannot take the display while that is running\n",
			path);
		drmModeFreeConnector(connector);
		drm_stop(server);
		return -1;
	}

	/* THE PIXELS ARE THE KERNEL'S AND THE MAPPING IS THIS PROCESS'S, which is what makes the compositing loop
	 * write straight onto the screen with no copy. */
	memset(&create, 0, sizeof create);
	create.width = (uint32_t)mode.hdisplay;
	create.height = (uint32_t)mode.vdisplay;
	create.bpp = 32;
	if (drmIoctl(drm->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
		fprintf(stderr, "window-server: could not allocate a %ux%u scanout buffer: %s\n", create.width,
			create.height, strerror(errno));
		drmModeFreeConnector(connector);
		drm_stop(server);
		return -1;
	}
	drm->handle = create.handle;
	drm->pitch = create.pitch;
	drm->size = create.size;
	memset(&map, 0, sizeof map);
	map.handle = create.handle;
	if (drmIoctl(drm->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
		fprintf(stderr, "window-server: could not map the scanout buffer: %s\n", strerror(errno));
		drmModeFreeConnector(connector);
		drm_stop(server);
		return -1;
	}
	offset = map.offset;
	drm->map = mmap(NULL, (size_t)drm->size, PROT_READ | PROT_WRITE, MAP_SHARED, drm->fd, (off_t)offset);
	if (drm->map == MAP_FAILED) {
		drm->map = NULL;
		fprintf(stderr, "window-server: mmap of the scanout buffer failed: %s\n", strerror(errno));
		drmModeFreeConnector(connector);
		drm_stop(server);
		return -1;
	}
	/* 24 DEPTH AND 32 BITS PER PIXEL IS XRGB8888, the format every client here sends and the one the
	 * compositing loop writes. */
	if (drmModeAddFB(drm->fd, create.width, create.height, 24, 32, drm->pitch, drm->handle, &drm->fb_id) != 0) {
		fprintf(stderr, "window-server: could not make a framebuffer for the scanout buffer\n");
		drmModeFreeConnector(connector);
		drm_stop(server);
		return -1;
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
	server->backend.width = (int)create.width;
	server->backend.height = (int)create.height;

	/* **THE DRY RUN: EVERY STEP UP TO THE COMMIT AND NOTHING AFTER IT.** Taking a display away from the session
	 * using it is the most disruptive thing any backend in this tree can do, so there is a way to prove the
	 * whole preparation — the device, the connector, the mode, the CRTC, the plane, the buffer, the framebuffer,
	 * the blob — WITHOUT the commit that makes light. `AWS_DRM_DRY_RUN=1` asks for it. */
	if (getenv("AWS_DRM_DRY_RUN") != NULL) {
		fprintf(stderr,
			"window-server: DRY RUN — every step up to the commit succeeded, and NOTHING was committed:\n"
			"                device      %s\n"
			"                connector   %u (%s), %u mode(s)\n"
			"                mode        %ux%u%s\n"
			"                crtc        %u\n"
			"                plane       %u (PRIMARY)\n"
			"                buffer      %ux%u, pitch %u, %llu bytes, mapped at %p\n"
			"                framebuffer %u (XRGB8888), mode blob %u\n"
			"                Master was taken and is given back; no pixel moves.\n",
			path, drm->connector_id,
			drmModeGetConnectorTypeName(connector->connector_type) != NULL
				? drmModeGetConnectorTypeName(connector->connector_type) : "?",
			connector->count_modes, mode.hdisplay, mode.vdisplay,
			(mode.type & DRM_MODE_TYPE_PREFERRED) ? " (PREFERRED)" : "", drm->crtc_id, drm->plane_id,
			create.width, create.height, drm->pitch, (unsigned long long)drm->size, drm->map, drm->fb_id,
			drm->blob_id);
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

	server->backend.pixels = drm->map;
	server->backend.name = "drm";
	server->backend.data = drm;
	/* **AND `present` IS NO LONGER NULL.** The frame buffer IS what the display scans out, so there is nothing
	 * to hand over — but a driver still has to be told the memory changed. */
	server->backend.present = drm_present;
	server->backend.stop = drm_stop;
	fprintf(stderr, "window-server: driving %s at %ux%u (atomic: connector %u, crtc %u, plane %u)\n", path,
		create.width, create.height, drm->connector_id, drm->crtc_id, drm->plane_id);
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
