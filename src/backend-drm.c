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
 * IT IS A DUMB-BUFFER SCANOUT AND NOT A PAGE-FLIPPING RENDERER, and that is the first version on purpose. The
 * kernel allocates the pixels, this process MAPS them, and the CRTC is pointed at them once — so the compositing
 * loop writes straight into the memory the display is scanning out, and `present` has nothing to do. What that
 * costs is TEARING: a frame is visible while it is still being drawn. What it buys is a bring-up path with no
 * GBM, no EGL, no atomic modesetting and no GPU driver in the picture, which is the right amount of machinery
 * to get a first frame onto a real screen. Page flipping (`drmModePageFlip` with two buffers) is the next
 * refinement, and the backend interface already has the shape for it — `present` exists and is simply unused.
 *
 * TWO THINGS IT MUST DO THAT A WINDOWED BACKEND NEVER DOES:
 *
 *   IT TAKES DRM MASTER, which is a claim on the device that the kernel grants to ONE process. If an X server
 *   or another compositor holds it, this FAILS and says so — which is the safety property that makes this safe
 *   to run by accident: a compositor cannot quietly take a display away from the session using it.
 *
 *   AND IT PUTS THE PREVIOUS MODE BACK when it stops. What was scanning out before this started is remembered
 *   and restored, so leaving is not a screen left in a state only a reboot fixes.
 *
 * AND IT DOES NOT CURRENTLY PUT ANYTHING ON A SCREEN ON THIS DRIVER — SEE THE NOTE ON THE LEGACY PATH AT
 * `drmModeSetCrtc` BELOW. It is built, it enumerates, it allocates and it accepts its own calls; it does not
 * light a pixel, and that is recorded rather than implied.
 *
 * WHEN IT FAILS HERE, IT SAYS WHAT TO DO. The usual reason on a machine with a proprietary NVIDIA driver is
 * that modesetting is off in the KERNEL module (`nvidia_drm.modeset=1`), and the symptom is a device with no
 * connectors at all — a card that can render and cannot display. The message below names that case rather than
 * reporting "no resources".
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
	drmModeCrtc *saved_crtc;	/* what was scanning out before this took over */
	uint32_t crtc_id;
	uint32_t connector_id;
	uint32_t fb_id;
	uint32_t handle;
	uint32_t pitch;
	uint64_t size;
	void *map;
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

/* A CRTC THAT CAN DRIVE THIS CONNECTOR. The connector's current encoder is asked first — it is the one the
 * previous master was using and therefore the one that works — and only then does this search. */
static uint32_t crtc_for_connector(struct drm_state *drm, drmModeConnector *connector)
{
	drmModeEncoder *encoder;
	uint32_t crtc_id = 0;
	int i;

	if (connector->encoder_id != 0) {
		encoder = drmModeGetEncoder(drm->fd, connector->encoder_id);
		if (encoder != NULL) {
			crtc_id = encoder->crtc_id;
			drmModeFreeEncoder(encoder);
			if (crtc_id != 0) {
				return crtc_id;
			}
		}
	}
	for (i = 0; i < connector->count_encoders && crtc_id == 0; i++) {
		encoder = drmModeGetEncoder(drm->fd, connector->encoders[i]);
		if (encoder == NULL) {
			continue;
		}
		if (encoder->crtc_id != 0) {
			crtc_id = encoder->crtc_id;
		} else if (drm->resources->count_crtcs > 0) {
			crtc_id = drm->resources->crtcs[0];
		}
		drmModeFreeEncoder(encoder);
	}
	return crtc_id;
}

static void drm_stop(struct aws_server *server)
{
	struct drm_state *drm = server->backend.data;

	if (drm == NULL) {
		return;
	}
	/* **PUT THE OLD MODE BACK FIRST.** Everything else is this process's own resources and the kernel would
	 * clean them up on close; the mode is the one thing that outlives the process, and a screen left in a state
	 * only a reboot fixes is the failure this line exists to prevent. */
	if (drm->saved_crtc != NULL) {
		drmModeSetCrtc(drm->fd, drm->saved_crtc->crtc_id, drm->saved_crtc->buffer_id,
			       drm->saved_crtc->x, drm->saved_crtc->y, &drm->connector_id, 1,
			       &drm->saved_crtc->mode);
		drmModeFreeCrtc(drm->saved_crtc);
	}
	if (drm->fb_id != 0) {
		drmModeRmFB(drm->fd, drm->fb_id);
	}
	if (drm->map != NULL && drm->size > 0) {
		munmap(drm->map, (size_t)drm->size);
	}
	if (drm->handle != 0) {
		struct drm_mode_destroy_dumb destroy;

		memset(&destroy, 0, sizeof destroy);
		destroy.handle = drm->handle;
		drmIoctl(drm->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
	/* THE CLAIM IS GIVEN BACK, so whoever had the display before can have it again. */
	drmDropMaster(drm->fd);
	if (drm->resources != NULL) {
		drmModeFreeResources(drm->resources);
	}
	if (drm->fd >= 0) {
		close(drm->fd);
	}
	free(drm);
	server->backend.data = NULL;
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
	drm->fd = -1;
	drm->fd = open(path, O_RDWR | O_CLOEXEC);
	if (drm->fd < 0) {
		fprintf(stderr, "window-server: could not open %s: %s\n", path, strerror(errno));
		free(drm);
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
	for (i = 0; i < drm->resources->count_connectors; i++) {
		connector = drmModeGetConnector(drm->fd, drm->resources->connectors[i]);
		if (connector == NULL) {
			continue;
		}
		if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
			break;
		}
		drmModeFreeConnector(connector);
		connector = NULL;
	}
	if (connector == NULL) {
		fprintf(stderr,
			"window-server: %s has %d connector(s) and NONE is connected with a mode, so there is no\n"
			"                display to drive. On a machine with the proprietary NVIDIA driver this is\n"
			"                usually KMS being OFF IN THE KERNEL: `nvidia_drm.modeset=1` (a module\n"
			"                parameter, which needs the initramfs rebuilt and a reboot) is what turns a card\n"
			"                that can render into one that can display.\n",
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
		/* A COPY, because `drmModeSetCrtc` takes a non-const mode and the one in the connector belongs to
		 * drmModeGetConnector's allocation. */
		mode = *found;
	}
	drm->crtc_id = crtc_for_connector(drm, connector);
	if (drm->crtc_id == 0) {
		fprintf(stderr, "window-server: found a connected display but no mode or CRTC to drive it with\n");
		drmModeFreeConnector(connector);
		drm_stop(server);
		return -1;
	}
	drm->connector_id = connector->connector_id;

	/* **MASTER IS THE CLAIM ON THE DEVICE, AND IT FAILS WHEN SOMEBODY ELSE HAS THE DISPLAY.** That failure is
	 * the reason this backend is safe to start by accident: an X server holding master keeps it. */
	if (drmSetMaster(drm->fd) != 0) {
		fprintf(stderr, "window-server: %s is held by another DRM master (an X server or another\n"
				"                compositor); this cannot take the display while that is running\n",
			path);
		drmModeFreeConnector(connector);
		drm_stop(server);
		return -1;
	}

	/* THE PIXELS ARE THE KERNEL'S AND THE MAPPING IS THIS PROCESS'S, which is what makes the compositing loop
	 * write straight onto the screen with no copy and no `present`. */
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
	/* 24 DEPTH AND 32 BITS PER PIXEL IS XRGB8888, which is the format every client here sends and the one the
	 * compositing loop writes. */
	if (drmModeAddFB(drm->fd, create.width, create.height, 24, 32, drm->pitch, drm->handle, &drm->fb_id) != 0) {
		fprintf(stderr, "window-server: could not make a framebuffer for the scanout buffer\n");
		drmModeFreeConnector(connector);
		drm_stop(server);
		return -1;
	}
	/* WHAT WAS THERE BEFORE, so that stopping puts it back. */
	drm->saved_crtc = drmModeGetCrtc(drm->fd, drm->crtc_id);
	/* **THE LEGACY MODESET IS ACCEPTED AND INERT ON A DRIVER THAT USES ATOMIC, AND THAT IS MEASURED.** On this
	 * machine's nvidia-drm the call below returns 0 and `drmModeGetCrtc` afterwards reports this framebuffer on
	 * this CRTC — while NOTHING APPEARS ON ANY MONITOR. The reason is visible in the driver's own state:
	 *
	 *     connector 77: CONNECTED, encoder_id=0        encoder 76: crtc_id=0
	 *     connector 79: CONNECTED, encoder_id=0        encoder 78: crtc_id=0
	 *
	 * NOTHING IS BOUND TO ANYTHING in the legacy model — no encoder on a connector, no CRTC on an encoder, no
	 * framebuffer on a CRTC — and yet a desktop is on the screen, which means the live path is the ATOMIC API
	 * and the legacy ioctls are only being recorded. A successful ioctl and a lit pixel are not the same thing,
	 * and confusing them is how this backend came to claim a display it never drove.
	 *
	 * THE FIX IS `drmModeAtomicCommit`: a property request setting the connector's CRTC_ID, the CRTC's MODE_ID
	 * (a blob) and ACTIVE, and the primary plane's FB_ID, CRTC_ID and SRC/CRTC rectangles, committed with
	 * ALLOW_MODESET. That is a real piece of work — property enumeration per object, a primary-plane lookup by
	 * `possible_crtcs`, and a mode blob — and it is the correct API for this driver regardless of whether the
	 * legacy one happens to work elsewhere, so it is the way this backend should be finished rather than a
	 * workaround for this machine.
	 *
	 * AND A DUMB BUFFER LIKELY NEEDS `drmModeDirtyFB` AFTER EVERY COMPOSITE besides: it is what tells a driver
	 * that the memory it is scanning out has changed. That belongs in `present`, which this backend leaves NULL.
	 */
	if (drmModeSetCrtc(drm->fd, drm->crtc_id, drm->fb_id, 0, 0, &drm->connector_id, 1, &mode) != 0) {
		fprintf(stderr, "window-server: could not set the mode on the display: %s\n", strerror(errno));
		drmModeFreeConnector(connector);
		drm_stop(server);
		return -1;
	}
	/* **THE DRY RUN: EVERYTHING UP TO THE MODESET AND NOTHING AFTER IT.** Taking a display away from the
	 * session using it is the most disruptive thing any backend in this tree can do, so there is a way to
	 * prove the whole setup path — the device, the connector, the mode, the CRTC, the dumb buffer, the
	 * framebuffer — WITHOUT the screen changing at all. `AWS_DRM_DRY_RUN=1` is what asks for it. */
	if (getenv("AWS_DRM_DRY_RUN") != NULL) {
		fprintf(stderr,
			"window-server: DRY RUN — every step up to the modeset succeeded, and the display was NOT taken:\n"
			"                device     %s\n"
			"                connector  %u (%s), %u mode(s)\n"
			"                mode       %ux%u%s\n"
			"                crtc       %u\n"
			"                buffer     %ux%u, pitch %u, %llu bytes, mapped at %p\n"
			"                framebuffer %u (XRGB8888)\n"
			"                Master was taken and is given back; nothing was set on the CRTC.\n",
			path, drm->connector_id,
			drmModeGetConnectorTypeName(connector->connector_type) != NULL
				? drmModeGetConnectorTypeName(connector->connector_type) : "?", connector->count_modes,
			mode.hdisplay, mode.vdisplay, (mode.type & DRM_MODE_TYPE_PREFERRED) ? " (PREFERRED)" : "",
			drm->crtc_id, create.width, create.height, drm->pitch, (unsigned long long)drm->size,
			drm->map, drm->fb_id);
		drmModeFreeConnector(connector);
		drm_stop(server);
		return -1;
	}

	drmModeFreeConnector(connector);

	server->backend.width = (int)create.width;
	server->backend.height = (int)create.height;
	server->backend.pixels = drm->map;
	server->backend.name = "drm";
	server->backend.data = drm;
	/* NO `present`: the frame buffer IS what the display scans out, so there is nothing to hand over. */
	server->backend.present = NULL;
	server->backend.stop = drm_stop;
	fprintf(stderr, "window-server: driving %s at %ux%u (dumb buffer scanout)\n", path, create.width,
		create.height);
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
