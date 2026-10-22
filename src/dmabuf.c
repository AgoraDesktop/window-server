/*
 * dmabuf.c — `zwp_linux_dmabuf_v1`: the GPU buffers a client may hand over instead of pixels.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * WHAT THIS REPLACES, IN ONE SENTENCE: a `wl_shm` window arrives as CPU memory the compositor must read and upload
 * on every frame, and a dma-buf window arrives as a file descriptor the GPU can sample where the client put it. The
 * CPU cost this removes is proportional to window area times frame rate — 5.8 MB per frame for a 1600x900 window —
 * and it is the whole reason a GPU renderer still felt like a copy.
 *
 * **IT IS ADVERTISED ONLY WHEN THE ACTIVE RENDERER CAN IMPORT.** `aws_dmabuf_init` asks the renderer for the
 * (format, modifier) pairs it can sample; the CPU renderer has none, so no global is created and a client is never
 * told about a buffer that would be dropped. That is why this runs after `aws_renderer_init` and not with the other
 * globals, and it is the one ordering in `server.c` that this file constrains.
 *
 * **IT BINDS AT VERSION 3, AND THAT IS A DECISION RATHER THAN A SHORTCUT.** Version 4 added
 * `zwp_linux_dmabuf_feedback_v1`, whose job is to tell a client WHICH DEVICE and WHICH MODIFIERS are fast — a
 * multi-GPU question. A nested compositor on one GPU has exactly one answer, so it advertises the flat
 * `format`/`modifier` list that v3 provides; clients that know about v4 fall back to those events, which is what
 * the protocol was designed to allow. When a DRM backend shares a render device with a client on a second GPU, the
 * feedback interface becomes the correct answer and this is where it goes.
 *
 * **AND ONLY SINGLE-PLANE RGB BUFFERS, BECAUSE THAT IS WHAT THIS COMPOSITOR COMPOSITES.** A window is XRGB8888 or
 * ARGB8888 today; a two-plane NV12 buffer would need a colour-conversion pass the renderer does not have, and
 * accepting it would be a picture that is subtly wrong rather than a client that is told no.
 */

#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "linux-dmabuf-v1-server-protocol.h"

/* The most a client may hand over in one params object. Four is the protocol's own limit for the plane index. */
#define AWS_DMABUF_MAX_PLANES 4
/* A ceiling on what one bind may send, not on what EGL can report: the pair list is small in practice, and a
 * compositor that advertised a thousand modifiers would be spending its client's bandwidth on a table nobody reads. */
#define AWS_DMABUF_MAX_FORMATS 256

/* ---------------------------------------------------------------------------------------------------
 * what this compositor can accept
 * --------------------------------------------------------------------------------------------------- */

int aws_dmabuf_format_planes(uint32_t format)
{
	switch (format) {
	case AWS_DRM_FORMAT_XRGB8888:
	case AWS_DRM_FORMAT_ARGB8888:
	case AWS_DRM_FORMAT_XBGR8888:
	case AWS_DRM_FORMAT_ABGR8888:
	case AWS_DRM_FORMAT_RGBX8888:
	case AWS_DRM_FORMAT_RGBA8888:
	case AWS_DRM_FORMAT_BGRX8888:
	case AWS_DRM_FORMAT_BGRA8888:
	case AWS_DRM_FORMAT_RGB888:
	case AWS_DRM_FORMAT_BGR888:
		return 1;
	default:
		return 0;
	}
}

int aws_dmabuf_format_has_alpha(uint32_t format)
{
	switch (format) {
	case AWS_DRM_FORMAT_ARGB8888:
	case AWS_DRM_FORMAT_ABGR8888:
	case AWS_DRM_FORMAT_RGBA8888:
	case AWS_DRM_FORMAT_BGRA8888:
		return 1;
	default:
		return 0;
	}
}

static int dmabuf_format_supported(struct aws_server *server, uint32_t format, uint64_t modifier)
{
	int i;

	for (i = 0; i < server->dmabuf_format_count; i++) {
		if (server->dmabuf_formats[i].format != format) {
			continue;
		}
		/* **IMPLICIT MODIFIERS ARE ACCEPTED FOR ANY FORMAT WE ADVERTISE.** `DRM_FORMAT_MOD_INVALID` means "let
		 * the allocator choose"; the import path asks EGL to work it out, which is exactly what implicit means. */
		if (server->dmabuf_formats[i].modifier == modifier ||
		    modifier == AWS_DRM_FORMAT_MOD_INVALID) {
			return 1;
		}
	}
	return 0;
}

/* ---------------------------------------------------------------------------------------------------
 * the buffer
 * --------------------------------------------------------------------------------------------------- */

static void buffer_close_planes(struct aws_dmabuf_buffer *buffer)
{
	int i;

	for (i = 0; i < buffer->plane_count; i++) {
		if (buffer->planes[i].fd >= 0) {
			close(buffer->planes[i].fd);
			buffer->planes[i].fd = -1;
		}
	}
}

/* THE CLIENT DESTROYED THE BUFFER, OR THE CONNECTION DID: let the renderer release its import and free ours. */
static void dmabuf_buffer_destroyed(struct wl_resource *resource)
{
	struct aws_dmabuf_buffer *buffer = wl_resource_get_user_data(resource);

	if (buffer == NULL) {
		return;
	}
	if (buffer->server != NULL && buffer->server->renderer != NULL &&
	    buffer->server->renderer->dmabuf_release != NULL) {
		buffer->server->renderer->dmabuf_release(buffer->server, buffer);
	}
	buffer_close_planes(buffer);
	free(buffer);
}

static void dmabuf_buffer_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

/* **THE `wl_buffer` IMPLEMENTATION FOR A dma-buf, AND IT IS DELIBERATELY THE SAME ONE libwayland GIVES `wl_shm`.**
 * The only request a `wl_buffer` has is `destroy`; everything else about a buffer — size, format, what a compositor
 * does with it — is learned by who made it. `aws_dmabuf_buffer_from_resource` recognises a buffer by this
 * implementation pointer, which is the same test `wl_shm_buffer_get` makes from the other side. */
static const struct wl_buffer_interface dmabuf_buffer_implementation = {
	dmabuf_buffer_destroy,
};

struct aws_dmabuf_buffer *aws_dmabuf_buffer_from_resource(struct wl_resource *buffer)
{
	/* **`wl_resource_instance_of` IS THE WHOLE TEST.** It answers yes only when the resource is a `wl_buffer`
	 * whose implementation is the one above — which is the same question `wl_shm_buffer_get` asks from the other
	 * side, and the reason a `wl_shm` buffer's libwayland user data is never mistaken for ours. */
	if (buffer == NULL ||
	    !wl_resource_instance_of(buffer, &wl_buffer_interface, &dmabuf_buffer_implementation)) {
		return NULL;
	}
	return wl_resource_get_user_data(buffer);
}

/* ---------------------------------------------------------------------------------------------------
 * buffer params
 * --------------------------------------------------------------------------------------------------- */

struct aws_dmabuf_params {
	struct aws_server *server;
	int plane_count;
	int used[AWS_DMABUF_MAX_PLANES];
	struct aws_dmabuf_plane planes[AWS_DMABUF_MAX_PLANES];
};

static void params_destroy(struct wl_resource *resource)
{
	struct aws_dmabuf_params *params = wl_resource_get_user_data(resource);
	int i;

	if (params == NULL) {
		return;
	}
	for (i = 0; i < AWS_DMABUF_MAX_PLANES; i++) {
		if (params->planes[i].fd >= 0) {
			close(params->planes[i].fd);
		}
	}
	free(params);
}

static void params_add(struct wl_client *client, struct wl_resource *resource, int32_t fd,
		       uint32_t plane_idx, uint32_t offset, uint32_t stride,
		       uint32_t modifier_hi, uint32_t modifier_lo)
{
	struct aws_dmabuf_params *params = wl_resource_get_user_data(resource);

	(void)client;
	/* **A REJECTED DESCRIPTOR IS STILL OURS TO CLOSE.** The fd arrives owned by this process, and a `return` without
	 * this is a descriptor leaked per malformed request — the kind of leak that only shows up as a client that
	 * cannot open its own files after a while. */
	if (plane_idx >= AWS_DMABUF_MAX_PLANES) {
		close(fd);
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
				       "plane index %u is out of range", plane_idx);
		return;
	}
	if (params->used[plane_idx]) {
		close(fd);
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
				       "plane %u was already set", plane_idx);
		return;
	}
	params->used[plane_idx] = 1;
	params->planes[plane_idx].fd = fd;
	params->planes[plane_idx].offset = offset;
	params->planes[plane_idx].stride = stride;
	params->planes[plane_idx].modifier = ((uint64_t)modifier_hi << 32) | (uint64_t)modifier_lo;
	if ((int)plane_idx + 1 > params->plane_count) {
		params->plane_count = (int)plane_idx + 1;
	}
}

/* **THE ONE PLACE A dma-buf BECOMES A `wl_buffer`.** It validates, hands the planes to the renderer, and builds the
 * resource; `error` is the protocol error a `create_immed` must raise when it cannot answer with an event. `id` is
 * zero for the deferred `create`, where the server chooses the id. */
static struct aws_dmabuf_buffer *dmabuf_make(struct aws_server *server, struct wl_client *client,
					     struct aws_dmabuf_params *params, int32_t width, int32_t height,
					     uint32_t format, uint32_t flags, uint32_t id, int *error)
{
	struct aws_dmabuf_buffer *buffer;
	int planes = aws_dmabuf_format_planes(format);
	int i;

	(void)flags;
	*error = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT;
	if (getenv("AWS_DMABUF_DEBUG") != NULL) {
		fprintf(stderr, "  DIAG dmabuf: create %dx%d format 0x%08x planes=%d modifier=0x%016llx\n",
			width, height, format, params->plane_count,
			(unsigned long long)params->planes[0].modifier);
	}
	if (width <= 0 || height <= 0) {
		*error = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS;
		return NULL;
	}
	if (planes <= 0 || params->plane_count != planes) {
		if (getenv("AWS_DMABUF_DEBUG") != NULL) {
			fprintf(stderr, "  DIAG dmabuf: rejected — format wants %d plane(s), params has %d\n",
				planes, params->plane_count);
		}
		*error = (planes <= 0) ? ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT
				       : ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE;
		return NULL;
	}
	if (!dmabuf_format_supported(server, format, params->planes[0].modifier)) {
		if (getenv("AWS_DMABUF_DEBUG") != NULL) {
			fprintf(stderr, "  DIAG dmabuf: rejected — format/modifier pair not advertised\n");
		}
		return NULL;
	}
	/* **AND EVERY PLANE CARRIES THE SAME MODIFIER.** Version 5 of the protocol made this an explicit rule; a
	 * single-plane format makes it vacuous today, and enforcing it here is what keeps the check in the code that
	 * would have to answer for a two-plane format tomorrow. */
	for (i = 1; i < params->plane_count; i++) {
		if (params->planes[i].modifier != params->planes[0].modifier) {
			return NULL;
		}
	}

	buffer = calloc(1, sizeof *buffer);
	if (buffer == NULL) {
		wl_client_post_no_memory(client);
		*error = 0;	/* the client has been told; do not raise a second, unrelated error */
		return NULL;
	}
	buffer->server = server;
	buffer->width = width;
	buffer->height = height;
	buffer->format = format;
	buffer->plane_count = params->plane_count;
	for (i = 0; i < params->plane_count; i++) {
		buffer->planes[i] = params->planes[i];
		/* OWNERSHIP MOVES TO THE BUFFER: the params object no longer closes these. */
		params->planes[i].fd = -1;
		params->used[i] = 0;
	}
	params->plane_count = 0;

	if (server->renderer == NULL || server->renderer->dmabuf_import == NULL ||
	    server->renderer->dmabuf_import(server, buffer) != 0) {
		if (getenv("AWS_DMABUF_DEBUG") != NULL) {
			fprintf(stderr, "  DIAG dmabuf: rejected — the renderer could not import it\n");
		}
		buffer_close_planes(buffer);
		free(buffer);
		return NULL;
	}
	/* **THE IMPORT HOLDS ITS OWN REFERENCE, SO THE DESCRIPTORS GO NOW.** An EGLImage (or a DRM framebuffer) has
	 * taken what it needs from the kernel; keeping the fds open would be a descriptor leak with nothing behind it. */
	buffer_close_planes(buffer);

	buffer->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
	if (buffer->resource == NULL) {
		server->renderer->dmabuf_release(server, buffer);
		free(buffer);
		wl_client_post_no_memory(client);
		*error = 0;
		return NULL;
	}
	wl_resource_set_implementation(buffer->resource, &dmabuf_buffer_implementation, buffer,
				       dmabuf_buffer_destroyed);
	return buffer;
}

static void params_create(struct wl_client *client, struct wl_resource *resource,
			  int32_t width, int32_t height, uint32_t format, uint32_t flags)
{
	struct aws_dmabuf_params *params = wl_resource_get_user_data(resource);
	struct aws_dmabuf_buffer *buffer;
	int error = 0;

	buffer = dmabuf_make(params->server, client, params, width, height, format, flags, 0, &error);
	if (buffer == NULL) {
		if (error != 0) {
			zwp_linux_buffer_params_v1_send_failed(resource);
		}
		return;
	}
	zwp_linux_buffer_params_v1_send_created(resource, buffer->resource);
}

/* **`create_immed` HAS NO `failed` TO SEND, SO A REFUSAL IS A PROTOCOL ERROR.** The client asked for the buffer to
 * exist with an id it chose, and there is nothing to send on the params object that says "not this time" — so the
 * protocol's own vocabulary is the only honest answer, and the error names why. */
static void params_create_immed(struct wl_client *client, struct wl_resource *resource, uint32_t buffer_id,
				int32_t width, int32_t height, uint32_t format, uint32_t flags)
{
	struct aws_dmabuf_params *params = wl_resource_get_user_data(resource);
	struct aws_dmabuf_buffer *buffer;
	int error = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT;

	buffer = dmabuf_make(params->server, client, params, width, height, format, flags, buffer_id,
			     &error);
	if (buffer == NULL && error != 0) {
		wl_resource_post_error(resource, (uint32_t)error, "the dma-buf could not be imported");
	}
}

static void params_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zwp_linux_buffer_params_v1_interface params_implementation = {
	params_destroy_request,
	params_add,
	params_create,
	params_create_immed,
};

/* ---------------------------------------------------------------------------------------------------
 * the global
 * --------------------------------------------------------------------------------------------------- */

static void dmabuf_create_params(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct aws_server *server = wl_resource_get_user_data(resource);
	struct aws_dmabuf_params *params;
	struct wl_resource *params_resource;
	int i;

	params = calloc(1, sizeof *params);
	if (params == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	params->server = server;
	for (i = 0; i < AWS_DMABUF_MAX_PLANES; i++) {
		params->planes[i].fd = -1;
	}
	params_resource = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
					     wl_resource_get_version(resource), id);
	if (params_resource == NULL) {
		free(params);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(params_resource, &params_implementation, params, params_destroy);
}

static void dmabuf_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_implementation = {
	dmabuf_destroy,
	dmabuf_create_params,
};

/* **THE ADVERTISEMENT, IN THE ONE FORM EVERY CLIENT UNDERSTANDS.** `format` is the v1 event and `modifier` the v3
 * one; a client bound at v1 or v2 hears only the format, which is the correct, smaller promise. */
static void dmabuf_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct aws_server *server = data;
	struct wl_resource *resource;
	int i;

	resource = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &dmabuf_implementation, server, NULL);
	for (i = 0; i < server->dmabuf_format_count; i++) {
		uint64_t modifier = server->dmabuf_formats[i].modifier;

		zwp_linux_dmabuf_v1_send_format(resource, server->dmabuf_formats[i].format);
		if (wl_resource_get_version(resource) >= ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION) {
			zwp_linux_dmabuf_v1_send_modifier(resource, server->dmabuf_formats[i].format,
							  (uint32_t)(modifier >> 32), (uint32_t)modifier);
		}
	}
}

void aws_dmabuf_init(struct aws_server *server)
{
	struct aws_dmabuf_format formats[AWS_DMABUF_MAX_FORMATS];
	int count;
	int i, kept = 0;

	if (server->renderer == NULL || server->renderer->dmabuf_formats == NULL) {
		fprintf(stderr, "window-server: dmabuf: the %s renderer cannot import GPU buffers; "
				"zwp_linux_dmabuf_v1 is not advertised\n",
			server->renderer != NULL ? server->renderer->name : "(no)");
		return;
	}
	count = server->renderer->dmabuf_formats(server, formats, AWS_DMABUF_MAX_FORMATS);
	for (i = 0; i < count; i++) {
		/* Only the formats this compositor can actually composite — see the note at the top of the file. */
		if (aws_dmabuf_format_planes(formats[i].format) != 1) {
			continue;
		}
		formats[kept++] = formats[i];
	}
	if (kept == 0) {
		fprintf(stderr, "window-server: dmabuf: the renderer offered no importable single-plane "
				"format; zwp_linux_dmabuf_v1 is not advertised\n");
		return;
	}
	server->dmabuf_formats = calloc((size_t)kept, sizeof *formats);
	if (server->dmabuf_formats == NULL) {
		return;
	}
	memcpy(server->dmabuf_formats, formats, (size_t)kept * sizeof *formats);
	server->dmabuf_format_count = kept;
	if (getenv("AWS_DMABUF_DEBUG") != NULL) {
		for (i = 0; i < kept; i++) {
			fprintf(stderr, "  DIAG dmabuf: format 0x%08x modifier 0x%016llx\n",
				server->dmabuf_formats[i].format,
				(unsigned long long)server->dmabuf_formats[i].modifier);
		}
	}
	/* **VERSION 3 IS THE FLAT LIST; v4's FEEDBACK IS A MULTI-GPU ANSWER.** See the file's opening note. */
	wl_global_create(server->display, &zwp_linux_dmabuf_v1_interface, 3, server, dmabuf_bind);
	fprintf(stderr, "window-server: dmabuf: %d (format, modifier) pair(s) advertised\n", kept);
}
