/*
 * backend-drm.c — a real output: Kernel Mode Setting, on the machine's own display.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
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

#include "scale.h"
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
#include <drm_fourcc.h>
#ifdef AWS_HAVE_GBM
#include <gbm.h>
#endif

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
	struct aws_server *server;	/* for the `gbm_surface` a flip handler releases into */
	const char *path;		/* the device, as it was named to the caller */
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
	/* The mode and what the dry-run report wants to say about it — kept here because the modeset that uses them
	 * now happens in `start_present`, after a renderer has been chosen. */
	drmModeModeInfo mode;
	int mode_preferred;
	unsigned count_modes;
	uint32_t connector_type;
#ifdef AWS_HAVE_GBM
	/* **THE GPU'S PAGES, WHICH ARE NOT MAPPED AND NOT DUMB.** The GBM device is this backend's (made from the
	 * card's fd); the surface is the GL renderer's, leant back through `server->backend.gbm_surface` so that a
	 * flip completion can return the buffer it stopped scanning. `scanout_*` is what the plane is showing,
	 * `pending_*` what it is being flipped to. */
	struct gbm_device *gbm;
	struct gbm_bo *scanout_bo;
	uint32_t scanout_fb;
	struct gbm_bo *pending_bo;
	uint32_t pending_fb;
#endif
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
static int drm_atomic_modeset(struct aws_server *server, int on, uint32_t fb_id)
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
		drm_set_plane(server, req, fb_id);
	} else {
		/* TURNING IT OFF, AND IN THE REVERSE OF THE ORDER THAT SET IT UP: the plane lets go of the framebuffer
		 * and the CRTC, then the CRTC goes inactive, then the connector lets go of the CRTC.
		 *
		 * **AND THE MODE GOES WITH THE ACTIVE BIT, WHICH IS THE WHOLE OF AN EINVAL THIS USED TO LOG.** The kernel's
		 * atomic core keeps one invariant between a CRTC's `enable` and its mode: `enable` is set by MODE_ID and
		 * cleared by MODE_ID = 0 (drm_atomic_set_mode_for_crtc), and a CRTC with no connectors must have
		 * `enable == false`. Clearing the connector's CRTC_ID and the CRTC's ACTIVE made the connector mask empty
		 * while the mode blob was still set, so drm_atomic_helper_check_modeset refused the commit (and the core's
		 * "disabled with mode blob" WARN_ON fired). MEASURED on a real DRM session: every ordinary SIGTERM logged
		 * "the atomic teardown was refused: Invalid argument". Clearing MODE_ID is what makes the teardown a
		 * disable rather than an enabled CRTC with nothing attached. */
		drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", 0);
		drm_atomic_set(drm->fd, req, drm->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", 0);
		drm_atomic_set(drm->fd, req, drm->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 0);
		drm_atomic_set(drm->fd, req, drm->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", 0);
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
#ifdef AWS_HAVE_GBM
	/* **AND THE BUFFER THAT JUST STOPPED BEING SCANNED GOES BACK TO THE GBM SURFACE.** The flip is the only
	 * moment the compositor knows the old front is not being read by the display any more, so it is the only
	 * moment `gbm_surface_release_buffer` is allowed. Its framebuffer goes with it; the new front was created
	 * when the renderer locked it, before the commit that named it. */
	if (drm->pending_fb != 0) {
		if (drm->scanout_fb != 0) {
			drmModeRmFB(drm->fd, drm->scanout_fb);
		}
		if (drm->scanout_bo != NULL && drm->server != NULL &&
		    drm->server->backend.gbm_surface != NULL) {
			gbm_surface_release_buffer(drm->server->backend.gbm_surface, drm->scanout_bo);
		}
		drm->scanout_bo = drm->pending_bo;
		drm->scanout_fb = drm->pending_fb;
		drm->pending_bo = NULL;
		drm->pending_fb = 0;
	}
#endif
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

#ifdef AWS_HAVE_GBM
/* **A GBM BUFFER AS A FRAMEBUFFER THE PLANE CAN SCAN.** The modifier is passed when the driver accepts it, because a
 * GBM buffer is often tiled and the plane has to be told the layout; a driver that refuses the modifier call gets
 * the older implicit-linear one, which is right for the drivers that do not tile anyway. */
static uint32_t drm_gbm_fb_create(struct drm_state *drm, struct gbm_bo *bo)
{
	uint32_t fb = 0;
	uint32_t handles[4] = { 0, 0, 0, 0 };
	uint32_t pitches[4] = { 0, 0, 0, 0 };
	uint32_t offsets[4] = { 0, 0, 0, 0 };
	uint64_t modifiers[4] = { 0, 0, 0, 0 };
	uint32_t width = gbm_bo_get_width(bo);
	uint32_t height = gbm_bo_get_height(bo);

	handles[0] = gbm_bo_get_handle(bo).u32;
	pitches[0] = gbm_bo_get_stride(bo);
	offsets[0] = gbm_bo_get_offset(bo, 0);
	modifiers[0] = gbm_bo_get_modifier(bo);
	if (drmModeAddFB2WithModifiers(drm->fd, width, height, DRM_FORMAT_XRGB8888, handles, pitches, offsets,
				       modifiers, &fb, DRM_MODE_FB_MODIFIERS) == 0) {
		return fb;
	}
	if (drmModeAddFB2(drm->fd, width, height, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &fb, 0) == 0) {
		return fb;
	}
	fprintf(stderr, "window-server: could not make a framebuffer from a %ux%u GBM buffer: %s\n", width, height,
		strerror(errno));
	return 0;
}

/* **THE GPU PRESENT: A PLANE COMMIT PER FRAME, AND ONE EXTRA MODESET FOR THE FIRST.** A GBM frame arrives here
 * already drawn and locked; this side only has to name it to the plane. The first one is special because the CRTC
 * has not been programmed yet — the compositor lit the display with its first frame rather than with a dummy page,
 * which is why the DRM backend no longer allocates its fallback pages before it knows who is drawing. Every later
 * one is an async flip whose completion releases the buffer that stopped being scanned. */
static int drm_gbm_present(struct aws_server *server, struct gbm_bo *bo)
{
	struct drm_state *drm = server->backend.data;
	uint32_t fb;
	drmModeAtomicReq *req;
	int ret;

	if (drm == NULL || bo == NULL) {
		return -1;
	}
	if (drm->pending_fb != 0) {
		/* The render loop's `busy` gate should make this impossible. Refusing is better than overwriting the
		 * buffer whose flip event still has to release it. */
		return -1;
	}
	fb = drm_gbm_fb_create(drm, bo);
	if (fb == 0) {
		return -1;
	}
	if (!drm->mode_is_set) {
		if (drm_atomic_modeset(server, 1, fb) != 0) {
			drmModeRmFB(drm->fd, fb);
			return -1;
		}
		drm->mode_is_set = 1;
		drm->scanout_bo = bo;
		drm->scanout_fb = fb;
		return 0;
	}
	req = drmModeAtomicAlloc();
	if (req == NULL) {
		drmModeRmFB(drm->fd, fb);
		return -1;
	}
	drm_set_plane(server, req, fb);
	drm->flip_pending = 1;
	ret = drmModeAtomicCommit(drm->fd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, drm);
	drmModeAtomicFree(req);
	if (ret != 0) {
		fprintf(stderr, "window-server: the GBM page flip was refused: %s\n", strerror(errno));
		drm->flip_pending = 0;
		drmModeRmFB(drm->fd, fb);
		return -1;
	}
	drm->pending_bo = bo;
	drm->pending_fb = fb;
	return 0;
}
#endif /* AWS_HAVE_GBM */

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

/* **THE SWITCH, HALF OF IT: GIVE THE DISPLAY UP AND KEEP EVERYTHING ELSE.** A VT release is not a shutdown — the
 * connectors, the mode, the blob and both dumb buffers all stay, because the compositor is coming back in a moment and
 * re-probing a display costs a modeset nobody needs. What goes is the CLAIM on the device (`drmDropMaster`, or the next
 * session cannot take it) and the CRTC's content (`drm_atomic_modeset(…, 0)`, so the kernel is not lighting a frame
 * this process has stopped maintaining while it hands the screen over).
 *
 * THE DESCRIPTOR SOURCE GOES TOO, and that is the part a compositor gets wrong: flip events and hotplug arrive on it,
 * and a paused backend that keeps reading them answers events for a device it does not own. */
static void drm_pause(struct aws_server *server)
{
	struct drm_state *drm = server->backend.data;

	if (drm == NULL) {
		return;
	}
	if (drm->source != NULL) {
		wl_event_source_remove(drm->source);
		drm->source = NULL;
	}
	if (drm->mode_is_set) {
		(void)drm_atomic_modeset(server, 0, 0);
		drm->mode_is_set = 0;
	}
#ifdef AWS_HAVE_GBM
	/* **A FLIP IN FLIGHT IS GIVEN UP, NOT WAITED FOR.** The terminal is being taken away; the event that would
	 * release the pending buffer may never arrive on a device this process no longer owns. The frames are dropped
	 * and the last COMPLETED one is what a resume re-modesets — which is the frame the screen was showing. */
	drm->flip_pending = 0;
	if (drm->pending_fb != 0) {
		drmModeRmFB(drm->fd, drm->pending_fb);
		if (drm->pending_bo != NULL && server->backend.gbm_surface != NULL) {
			gbm_surface_release_buffer(server->backend.gbm_surface, drm->pending_bo);
		}
		drm->pending_bo = NULL;
		drm->pending_fb = 0;
	}
#endif
	if (drm->fd >= 0) {
		(void)drmDropMaster(drm->fd);
	}
}

/* **AND THE OTHER HALF: TAKE IT BACK AND DRAW AGAIN.** `drmSetMaster` can fail here — another session may have claimed
 * the device in the window between the two, which is legal and is not this compositor's to fix — so the failure is
 * named and the backend stays paused rather than pretending. A successful resume re-modesets (the kernel dropped the
 * CRTC when the terminal changed hands) and marks the scene stale; the loop's next pass repaints every pixel. */
static void drm_resume(struct aws_server *server)
{
	struct drm_state *drm = server->backend.data;

	if (drm == NULL) {
		return;
	}
	if (drm->fd < 0) {
		return;
	}
	if (drmSetMaster(drm->fd) != 0) {
		fprintf(stderr, "window-server: could not take the display back (%s) — staying paused\n",
			strerror(errno));
		return;
	}
	{
		/* **THE LAST FRAME THAT ACTUALLY REACHED THE GLASS.** On a GBM output that is the buffer the plane was
		 * scanning; on the CPU path it is the front page. With neither (a resume before the first present) there is
		 * nothing to program yet, and the next present's initial modeset is the one that lights it. */
		uint32_t fb = 0;

#ifdef AWS_HAVE_GBM
		fb = drm->scanout_fb;
#endif
		if (fb == 0) {
			fb = drm->buffers[drm->front].fb_id;
		}
		if (fb != 0) {
			if (drm_atomic_modeset(server, 1, fb) != 0) {
				fprintf(stderr, "window-server: could not re-set the mode after a terminal switch\n");
				(void)drmDropMaster(drm->fd);
				return;
			}
			drm->mode_is_set = 1;
		}
	}
	if (drm->source == NULL) {
		drm->source = wl_event_loop_add_fd(server->loop, drm->fd, WL_EVENT_READABLE, drm_dispatch, server);
	}
	server->render_pending = 1;
}

static void drm_stop(struct aws_server *server)
{
	struct drm_state *drm = server->backend.data;
	int i;

	if (drm == NULL) {
		return;
	}
	if (drm->mode_is_set) {
		drm_atomic_modeset(server, 0, 0);
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
#ifdef AWS_HAVE_GBM
	/* **THE GPU'S BUFFERS GO BACK BEFORE THE DEVICE THAT MADE THEM.** A GBM surface is the renderer's and has
	 * already been destroyed by the time this runs (the server shuts the renderer down first), so the loan is
	 * checked rather than assumed — releasing into a freed surface is the one thing this must not do. */
	if (drm->pending_fb != 0) {
		drmModeRmFB(drm->fd, drm->pending_fb);
		drm->pending_fb = 0;
	}
	if (drm->pending_bo != NULL) {
		if (server->backend.gbm_surface != NULL) {
			gbm_surface_release_buffer(server->backend.gbm_surface, drm->pending_bo);
		}
		drm->pending_bo = NULL;
	}
	if (drm->scanout_fb != 0) {
		drmModeRmFB(drm->fd, drm->scanout_fb);
		drm->scanout_fb = 0;
	}
	if (drm->scanout_bo != NULL) {
		if (server->backend.gbm_surface != NULL) {
			gbm_surface_release_buffer(server->backend.gbm_surface, drm->scanout_bo);
		}
		drm->scanout_bo = NULL;
	}
	if (drm->gbm != NULL) {
		gbm_device_destroy(drm->gbm);
		drm->gbm = NULL;
	}
#endif
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

/* **AND THE SECOND HALF: LIGHT THE DISPLAY, BY WHICHEVER ROUTE THE RENDERER CHOSE.** A GL renderer that took the
 * GBM device draws its first frame before the CRTC is programmed; that frame's present is the initial modeset, so
 * nothing is allocated here. The CPU renderer is what needs the two dumb pages, and this is the first moment the
 * backend knows it is the one drawing.
 *
 * THE DRY RUN LIVES HERE FOR THE SAME REASON: it can only report what was actually prepared, and which pages (or
 * swapchain) exist is the question this function answers. It still gives the display back without committing. */
static int drm_start_present(struct aws_server *server)
{
	struct drm_state *drm = server->backend.data;
	const char *connector_type;
	int use_gbm = 0;
	int i;

	if (drm == NULL) {
		return -1;
	}
	connector_type = drmModeGetConnectorTypeName(drm->connector_type) != NULL
				 ? drmModeGetConnectorTypeName(drm->connector_type) : "?";
#ifdef AWS_HAVE_GBM
	use_gbm = drm->gbm != NULL && server->backend.gbm_present != NULL && server->renderer != NULL &&
		  strcmp(server->renderer->name, "gl") == 0;
#endif
	if (use_gbm) {
		if (getenv("AWS_DRM_DRY_RUN") != NULL) {
			fprintf(stderr,
				"window-server: DRY RUN — every step up to the first frame succeeded, and NOTHING was committed:\n"
				"                device      %s\n"
				"                connector   %u (%s), %u mode(s)\n"
				"                mode        %ux%u%s\n"
				"                crtc        %u\n"
				"                plane       %u (PRIMARY)\n"
				"                swapchain   GBM, XRGB8888, scanout-capable %ux%u\n"
				"                mode blob   %u\n"
				"                Master was taken and is given back; no pixel moves.\n",
				drm->path, drm->connector_id, connector_type, drm->count_modes, drm->mode.hdisplay,
				drm->mode.vdisplay, drm->mode_preferred ? " (PREFERRED)" : "", drm->crtc_id,
				drm->plane_id, server->backend.width, server->backend.height, drm->blob_id);
			drm_stop(server);
			return -1;
		}
		fprintf(stderr,
			"window-server: driving %s at %ux%u (atomic, GBM/GL swapchain, async flip: connector %u, crtc %u, plane %u)\n",
			drm->path, drm->mode.hdisplay, drm->mode.vdisplay, drm->connector_id, drm->crtc_id,
			drm->plane_id);
		return 0;
	}

	/* **THE CPU ROUTE: THE TWO PAGES IT ALWAYS HAD.** */
#ifdef AWS_HAVE_GBM
	/* A GBM DEVICE WHOSE RENDERER DID NOT TAKE IT IS A DEVICE NOBODY WILL USE. It is given back here rather than
	 * kept until teardown — and `gbm_present` with it, so nothing can hand this backend a buffer later. */
	if (drm->gbm != NULL) {
		server->backend.gbm_device = NULL;
		server->backend.gbm_present = NULL;
		gbm_device_destroy(drm->gbm);
		drm->gbm = NULL;
	}
#endif
	for (i = 0; i < 2; i++) {
		if (drm_create_buffer(drm, (int)drm->mode.hdisplay, (int)drm->mode.vdisplay, &drm->buffers[i]) != 0) {
			return -1;
		}
	}
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
			drm->path, drm->connector_id, connector_type, drm->count_modes, drm->mode.hdisplay,
			drm->mode.vdisplay, drm->mode_preferred ? " (PREFERRED)" : "", drm->crtc_id, drm->plane_id,
			drm->mode.hdisplay, drm->mode.vdisplay, drm->buffers[0].pitch,
			(unsigned long long)drm->buffers[0].size, drm->buffers[0].map, drm->buffers[1].map,
			drm->buffers[0].fb_id, drm->buffers[1].fb_id, drm->blob_id);
		drm_stop(server);
		return -1;
	}
	if (drm_atomic_modeset(server, 1, drm->buffers[0].fb_id) != 0) {
		return -1;
	}
	drm->mode_is_set = 1;
	/* PAGE 0 IS NOW ON THE SCREEN, SO THE COMPOSITOR STARTS ON PAGE 1. */
	drm->front = 0;
	server->backend.pixels = drm->buffers[1].map;
	server->backend.present = drm_present;
	fprintf(stderr,
		"window-server: driving %s at %ux%u (atomic, 2 pages, async flip: connector %u, crtc %u, plane %u)\n",
		drm->path, drm->mode.hdisplay, drm->mode.vdisplay, drm->connector_id, drm->crtc_id, drm->plane_id);
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
	/* **A POINT IS 1/72 INCH, SO THE PANEL DECIDES THE SCALE.** `aws_scale_for_display` measures the connector's
	 * active millimetres, so `pixels x 25.4 / mm / 72` is the number of device pixels that make a logical point on
	 * THIS display. That is what makes a 13" 4K panel and a 27" 1440p panel agree about the size of a menu bar and a
	 * window control: the dense panel gets a larger scale and more physical detail at the same point size. 96 dpi is
	 * the fallback only when the display reports no usable size, and at every resolution: the mode's height is not
	 * an input to this function, because a resolution is a pixel count and not a physical size.
	 *
	 * The measurement is exact rather than rounded to 1x or 2x — a 109-dpi screen really is 1.511… device pixels per
	 * point — and `aws_scale_from_physical_size` is where the EDID sanity checks live, because a wrong reported size
	 * must not resize every window. `--dpi` and `--scale` still override the measurement and the fallback alike. */
	if (server->backend.user_scale == 0) {
		double derived = aws_scale_for_display((int)mode.hdisplay, connector->mmWidth);

		if (derived > 0.0) {
			int fallback = aws_scale_from_physical_size((int)mode.hdisplay, connector->mmWidth) <= 0.0;
			/* **THE PANEL'S ANSWER FIRST, THEN THE USER'S COMFORT FACTOR.** `base` is what the connector said and
			 * is what the message below quotes; `derived` is the product, quantised at 1/256, and it is what the
			 * framebuffer, the logical screen and every client are built from. */
			double base;

			base = aws_scale_quantized(derived);
			derived = aws_scale_with_ui(base, server->backend.ui_scale);
			server->backend.base_scale = base;
			server->backend.scale = derived;
			if (fallback) {
				fprintf(stderr, "window-server: this display is %ux%u and did not report a usable size, so "
						"its scale is %g (%.0f dpi; override with --dpi or --scale)\n",
					mode.hdisplay, mode.vdisplay, base, AWS_FALLBACK_DPI);
			} else {
				fprintf(stderr, "window-server: this display is %ux%u on %d mm (%.0f dpi), so its "
						"scale is %g (override with --dpi or --scale)\n",
					mode.hdisplay, mode.vdisplay, connector->mmWidth,
					(double)mode.hdisplay * 25.4 / (double)connector->mmWidth, base);
			}
		}
	}
	/* **AND THE LOGICAL SCREEN IS THE MODE DIVIDED BY THAT SCALE.** It is kept rather than derived later for the
	 * same reason the X11 backend keeps the requested size: with a fractional scale, `width / scale` does not
	 * return a number that can be multiplied back, and a screen whose size changes depending on how it is asked
	 * about would be a screen of the wrong size. */
	{
		double scale = aws_output_scale(server);

		server->backend.logical_width = (int)((double)mode.hdisplay / scale + 0.5);
		server->backend.logical_height = (int)((double)mode.vdisplay / scale + 0.5);
		if (server->backend.logical_width < 1) {
			server->backend.logical_width = 1;
		}
		if (server->backend.logical_height < 1) {
			server->backend.logical_height = 1;
		}
	}

	/* **AND THE GPU'S ROUTE TO THE SCANNER, MADE BEFORE ANY FALLBACK PAGES.** A GBM device costs nothing to make
	 * and is what lets the GL renderer take the present; the dumb buffers the CPU renderer needs are now allocated
	 * only if it did not. That ordering is what makes an 8K session possible at all — two dumb pages at 8K are
	 * 265 MB, and the swapchain that replaces them is already the size the display reads.
	 *
	 * A card without GBM is not a failure: the CPU renderer is still a place to put pixels. */
#ifdef AWS_HAVE_GBM
	drm->gbm = gbm_create_device(drm->fd);
	if (drm->gbm != NULL) {
		server->backend.gbm_device = drm->gbm;
		server->backend.gbm_present = drm_gbm_present;
	} else {
		fprintf(stderr, "window-server: could not make a GBM device on %s; the CPU renderer will be used\n",
			path);
	}
#endif

	/* **WHAT THE MODESET NOW NEEDS LATER.** The mode was the connector's and the connector is about to be freed;
	 * the modeset itself happens in `start_present`, once a renderer has said whether the display is lit by the
	 * GPU or by a page this backend owns. */
	drm->server = server;
	drm->path = path;
	drm->mode = mode;
	drm->mode_preferred = (mode.type & DRM_MODE_TYPE_PREFERRED) != 0;
	drm->count_modes = connector->count_modes;
	drm->connector_type = connector->connector_type;
	drmModeFreeConnector(connector);

	server->backend.name = "drm";
	/* **THE POINTER IS OURS ON A SCANNER.** There is no host window here whose pointer the person can see, so the
	 * compositor composites the arrow in cursor.c over every frame — on both renderers. */
	server->backend.owns_cursor = 1;
	server->backend.data = drm;
	server->backend.pause = drm_pause;
	server->backend.resume = drm_resume;
	server->backend.busy = drm_busy;
	server->backend.stop = drm_stop;
	server->backend.start_present = drm_start_present;
	/* AND THE DESCRIPTOR GOES ON THE LOOP, which is where the flip events are read. */
	drm->source = wl_event_loop_add_fd(server->loop, drm->fd, WL_EVENT_READABLE, drm_dispatch, server);
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
