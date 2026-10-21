/*
 * renderer-gl.c — the same scene, drawn by the GPU through EGL and GLES2.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
 *
 * WHY THIS FILE EXISTS AND WHY IT IS TESTED NESTED FIRST. There are two independent halves to "acceleration": the
 * RENDERER (turning surfaces, shadows and the bar into triangles) and the DISPLAY PATH (how the finished frame
 * reaches the glass — a GBM surface and an atomic plane commit on DRM/KMS, or `eglSwapBuffers` on a window). The
 * renderer is the expensive, backend-agnostic half, and the nested X11 backend is where it can be watched: it needs
 * no DRM master, no KMS driver, no free virtual terminal and no logind seat, only an X server with a GPU behind it.
 * So this renderer grew up in the nested output, and a DRM backend's accelerated path will be a swapchain added
 * under it rather than a second renderer beside it.
 *
 * IT IS THE SAME PICTURE AS `renderer-cpu.c`, AND THAT IS TESTABLE. Both are walked by `aws_output_walk`, both take
 * the same shadow constants from `server.h`, and both answer `--screenshot`: this one calls `glReadPixels` into
 * `backend->pixels` before it swaps, so `inspect.py` can compare a CPU frame and a GL frame of the same scene. A
 * renderer that looked different would be a bug with a number attached, not a matter of taste.
 *
 * WHAT IT DOES NOT DO YET, recorded rather than left to be found: client buffers are uploaded as textures every
 * frame (`wl_shm` only — there is no dmabuf import here yet), so this makes COMPOSITION cheap and not client
 * painting. The AppKit still draws with the CPU; making its buffers GPU-visible is `zwp_linux_dmabuf_v1` and is a
 * separate piece of work on both sides of the wire.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AWS_HAVE_GL

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

/* ---------------------------------------------------------------------------------------------------
 * shaders
 * --------------------------------------------------------------------------------------------------- */

/* ONE VERTEX SHADER, BECAUSE EVERY QUAD IS A QUAD. Positions arrive in the output's own pixels with the origin at
 * the TOP LEFT — the coordinates every other file in this server thinks in, and the ones a screenshot is written
 * in — and are mapped to clip space here. A renderer that made its callers think in NDC would be a renderer whose
 * callers could not be compared to the CPU one. */
static const char *const vertex_source =
	"attribute vec2 a_pos;\n"
	"uniform vec2 u_size;\n"
	"varying vec2 v_pos;\n"
	"void main() {\n"
	"    v_pos = a_pos;\n"
	"    gl_Position = vec4(a_pos.x / u_size.x * 2.0 - 1.0,\n"
	"                       1.0 - a_pos.y / u_size.y * 2.0, 0.0, 1.0);\n"
	"}\n";

static const char *const solid_source =
	"precision mediump float;\n"
	"uniform vec4 u_color;\n"
	"void main() { gl_FragColor = u_color; }\n";

/* **THE WINDOW'S CORNER AND ITS ALPHA, IN ONE FRAGMENT SHADER.** `u_round` is one flag per corner — the same four
 * decisions `shape_coverage` makes in the CPU renderer (a corner flush with the output is square, a menu's top
 * corners belong to the bar) — and `u_radius` is the same six points. The coverage is `clamp(0.5 - d)` on the
 * signed distance, which is the CPU's "fully inside half a point in, fully outside half a point out" written once
 * instead of per pixel.
 *
 * AND THE CHANNELS ARE SWAPPED HERE RATHER THAN IN THE UPLOAD. A `wl_shm` XRGB8888/ARGB8888 buffer is BGRA in
 * memory; uploading it as GL_RGBA puts B in `.r`, so the shader reads `.bgr`. That keeps the texture format inside
 * core GLES2, with no BGRA extension and no per-frame byte swizzle on the CPU. */
static const char *const texture_source =
	"precision mediump float;\n"
	"varying vec2 v_pos;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec4 u_rect;\n"
	"uniform vec2 u_uv_div;\n"
	"uniform float u_radius;\n"
	"uniform vec4 u_round;\n"
	"uniform float u_opaque;\n"
	"uniform float u_coverage;\n"
	"uniform vec4 u_ink;\n"
	"void main() {\n"
	"    vec2 local = v_pos - u_rect.xy;\n"
	"    vec4 t = texture2D(u_tex, local / u_uv_div);\n"
	"    vec3 rgb;\n"
	"    float a;\n"
	"    if (u_coverage > 0.5) {\n"
	"        rgb = u_ink.rgb;\n"
	"        a = t.r;\n"
	"    } else {\n"
	"        rgb = vec3(t.b, t.g, t.r);\n"
	"        a = mix(t.a, 1.0, u_opaque);\n"
	"    }\n"
	"    vec2 c = local - u_rect.zw * 0.5;\n"
	"    float r;\n"
	"    if (c.x < 0.0) {\n"
	"        r = (c.y < 0.0) ? u_round.x : u_round.z;\n"
	"    } else {\n"
	"        r = (c.y < 0.0) ? u_round.y : u_round.w;\n"
	"    }\n"
	"    r *= u_radius;\n"
	"    vec2 hs = u_rect.zw * 0.5;\n"
	"    vec2 q = abs(c) - hs + vec2(r);\n"
	"    float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;\n"
	"    float cover = clamp(0.5 - d, 0.0, 1.0);\n"
	"    if (cover <= 0.0) { discard; }\n"
	"    gl_FragColor = vec4(rgb, a * cover);\n"
	"}\n";

/* **AND THE SHADOW IS THE SAME DISTANCE, WITH THE SAME FALLOFF, MEASURED FROM THE SAME PLACE.** `renderer-cpu.c`
 * computes it from the pixel's INDEX (`row`, `column`) and its integer half-size, not from its centre — so the
 * shader floors `v_pos` back to that index before measuring. That half-pixel is the difference between a shadow
 * that starts one row under the menu bar and one that starts on it: MEASURED, comparing a CPU frame and a GL frame
 * of the same scene, the centre-sampled version painted a dark line at row 24 where the CPU painted nothing. The
 * falloff below is `t = 1 - distance / DEPTH; alpha = SHADOW_ALPHA * t * t` verbatim. */
static const char *const shadow_source =
	"precision mediump float;\n"
	"varying vec2 v_pos;\n"
	"uniform vec4 u_rect;\n"
	"uniform float u_radius;\n"
	"uniform float u_alpha;\n"
	"uniform float u_depth;\n"
	"void main() {\n"
	"    vec2 pixel = floor(v_pos);\n"
	"    vec2 half_size = floor(u_rect.zw * 0.5);\n"
	"    vec2 center = u_rect.xy + half_size;\n"
	"    vec2 inner = half_size - vec2(u_radius);\n"
	"    vec2 d2 = max(abs(pixel - center) - inner, vec2(0.0));\n"
	"    float d = floor(sqrt(dot(d2, d2))) - u_radius;\n"
	"    float t = 1.0 - d / u_depth;\n"
	"    if (d <= 0.0 || t <= 0.0) { discard; }\n"
	"    gl_FragColor = vec4(0.0, 0.0, 0.0, u_alpha * t * t);\n"
	"}\n";

/* **AND THE SAME SHADER FOR AN IMPORT THAT MAY ONLY BE `GL_TEXTURE_EXTERNAL_OES`.** Some drivers — NVIDIA's among
 * them — report a dma-buf modifier as `external_only`, which means the EGLImage can be bound to the external target
 * and NOT to `GL_TEXTURE_2D`: MEASURED, the ordinary target answers `GL_INVALID_OPERATION`. The two differences from
 * the shader above are the extension, the sampler type, and the fact that an external image is already in RGB order
 * (the driver converts on sample), so the BGRA swap must not be applied twice. */
static const char *const texture_external_source =
	"#extension GL_OES_EGL_image_external : require\n"
	"precision mediump float;\n"
	"varying vec2 v_pos;\n"
	"uniform samplerExternalOES u_tex;\n"
	"uniform vec4 u_rect;\n"
	"uniform vec2 u_uv_div;\n"
	"uniform float u_radius;\n"
	"uniform vec4 u_round;\n"
	"uniform float u_opaque;\n"
	"uniform float u_coverage;\n"
	"uniform vec4 u_ink;\n"
	"void main() {\n"
	"    vec2 local = v_pos - u_rect.xy;\n"
	"    vec4 t = texture2D(u_tex, local / u_uv_div);\n"
	"    vec3 rgb;\n"
	"    float a;\n"
	"    if (u_coverage > 0.5) {\n"
	"        rgb = u_ink.rgb;\n"
	"        a = t.r;\n"
	"    } else {\n"
	"        rgb = t.rgb;\n"
	"        a = mix(t.a, 1.0, u_opaque);\n"
	"    }\n"
	"    vec2 c = local - u_rect.zw * 0.5;\n"
	"    float r;\n"
	"    if (c.x < 0.0) {\n"
	"        r = (c.y < 0.0) ? u_round.x : u_round.z;\n"
	"    } else {\n"
	"        r = (c.y < 0.0) ? u_round.y : u_round.w;\n"
	"    }\n"
	"    r *= u_radius;\n"
	"    vec2 hs = u_rect.zw * 0.5;\n"
	"    vec2 q = abs(c) - hs + vec2(r);\n"
	"    float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;\n"
	"    float cover = clamp(0.5 - d, 0.0, 1.0);\n"
	"    if (cover <= 0.0) { discard; }\n"
	"    gl_FragColor = vec4(rgb, a * cover);\n"
	"}\n";

/* ---------------------------------------------------------------------------------------------------
 * state
 * --------------------------------------------------------------------------------------------------- */

struct gl_program {
	GLuint id;
	GLint a_pos;
	GLint u_size;
	GLint u_color;
	GLint u_tex;
	GLint u_rect;
	GLint u_uv_div;
	GLint u_radius;
	GLint u_round;
	GLint u_opaque;
	GLint u_coverage;
	GLint u_ink;
	GLint u_alpha;
	GLint u_depth;
};

struct gl_state {
	EGLDisplay display;
	EGLSurface surface;
	EGLContext context;
	EGLConfig config;

	/* **AND THE dma-buf IMPORT, WHEN THE DRIVER HAS IT.** The three entry points below are what turns a client's
	 * file descriptors into a texture; the two queries are what makes the advertisement honest by asking the driver
	 * which formats and modifiers it can actually sample rather than naming a list here. */
	int dmabuf;
	/* **AND WHETHER AN IMPORTED IMAGE MAY HAVE TO BE SAMPLED AS `GL_TEXTURE_EXTERNAL_OES`.** A driver that reports a
	 * modifier as `external_only` — NVIDIA does, for LINEAR — can bind the image to nothing else, and the shader
	 * that samples it is a different one with a different sampler type. */
	int has_external;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;
	PFNEGLQUERYDMABUFFORMATSEXTPROC query_formats;
	PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_modifiers;

	struct gl_program solid;
	struct gl_program texture;
	struct gl_program texture_external;
	struct gl_program shadow;

	GLuint client_texture;		/* the surface being drawn, re-uploaded per draw */
	GLuint coverage_texture;	/* the System menu's mark, re-uploaded per bar draw */
	unsigned char *scratch;		/* rows repacked when a shm stride is not w * 4 */
	size_t scratch_size;

	int width;
	int height;
	int own_pixels;			/* the readback buffer is ours to free */
};

/* ---------------------------------------------------------------------------------------------------
 * shader plumbing
 * --------------------------------------------------------------------------------------------------- */

static GLuint compile(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];

		glGetShaderInfoLog(shader, (GLsizei)sizeof log, NULL, log);
		fprintf(stderr, "window-server: gl shader: %s\n", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static int link_program(struct gl_program *program, const char *fragment_source)
{
	GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
	GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
	GLint ok = 0;

	if (vertex == 0 || fragment == 0) {
		return -1;
	}
	program->id = glCreateProgram();
	glAttachShader(program->id, vertex);
	glAttachShader(program->id, fragment);
	glLinkProgram(program->id);
	glGetProgramiv(program->id, GL_LINK_STATUS, &ok);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	if (!ok) {
		char log[1024];

		glGetProgramInfoLog(program->id, (GLsizei)sizeof log, NULL, log);
		fprintf(stderr, "window-server: gl link: %s\n", log);
		return -1;
	}
	program->a_pos = glGetAttribLocation(program->id, "a_pos");
	program->u_size = glGetUniformLocation(program->id, "u_size");
	program->u_color = glGetUniformLocation(program->id, "u_color");
	program->u_tex = glGetUniformLocation(program->id, "u_tex");
	program->u_rect = glGetUniformLocation(program->id, "u_rect");
	program->u_uv_div = glGetUniformLocation(program->id, "u_uv_div");
	program->u_radius = glGetUniformLocation(program->id, "u_radius");
	program->u_round = glGetUniformLocation(program->id, "u_round");
	program->u_opaque = glGetUniformLocation(program->id, "u_opaque");
	program->u_coverage = glGetUniformLocation(program->id, "u_coverage");
	program->u_ink = glGetUniformLocation(program->id, "u_ink");
	program->u_alpha = glGetUniformLocation(program->id, "u_alpha");
	program->u_depth = glGetUniformLocation(program->id, "u_depth");
	return 0;
}

/* ---------------------------------------------------------------------------------------------------
 * drawing one thing
 * --------------------------------------------------------------------------------------------------- */

/* EVERY QUAD IS A TRIANGLE STRIP THROUGH FOUR CORNERS, in the output's pixels. Client-side arrays rather than a
 * VBO: four vertices per draw is not a buffer worth a buffer object, and GLES2 supports them. */
static void quad_vertices(float *v, float x, float y, float w, float h)
{
	v[0] = x;	v[1] = y;
	v[2] = x + w;	v[3] = y;
	v[4] = x;	v[5] = y + h;
	v[6] = x + w;	v[7] = y + h;
}

static void rgb_of(uint32_t colour, float *r, float *g, float *b)
{
	*r = (float)((colour >> 16) & 0xFFu) / 255.0f;
	*g = (float)((colour >> 8) & 0xFFu) / 255.0f;
	*b = (float)(colour & 0xFFu) / 255.0f;
}

static void gl_solid(struct gl_state *gl, float x, float y, float w, float h, uint32_t colour)
{
	float v[8];
	float r, g, b;

	quad_vertices(v, x, y, w, h);
	rgb_of(colour, &r, &g, &b);
	glUseProgram(gl->solid.id);
	glUniform2f(gl->solid.u_size, (float)gl->width, (float)gl->height);
	glUniform4f(gl->solid.u_color, r, g, b, 1.0f);
	glEnableVertexAttribArray((GLuint)gl->solid.a_pos);
	glVertexAttribPointer((GLuint)gl->solid.a_pos, 2, GL_FLOAT, GL_FALSE, 0, v);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void gl_textured(struct gl_state *gl, struct gl_program *program, GLenum target, GLuint texture,
			float x, float y, float w, float h, float texture_w, float texture_h,
			float radius, float tl, float tr, float bl, float br, int coverage, int opaque,
			uint32_t ink)
{
	float v[8];
	float r, g, b;

	if (w <= 0.0f || h <= 0.0f) {
		return;
	}
	quad_vertices(v, x, y, w, h);
	rgb_of(ink, &r, &g, &b);
	glUseProgram(program->id);
	glUniform2f(program->u_size, (float)gl->width, (float)gl->height);
	glUniform4f(program->u_rect, x, y, w, h);
	glUniform2f(program->u_uv_div, texture_w, texture_h);
	glUniform1f(program->u_radius, radius);
	glUniform4f(program->u_round, tl, tr, bl, br);
	glUniform1f(program->u_opaque, opaque ? 1.0f : 0.0f);
	glUniform1f(program->u_coverage, coverage ? 1.0f : 0.0f);
	glUniform4f(program->u_ink, r, g, b, 1.0f);
	glUniform1i(program->u_tex, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(target, texture);
	glEnableVertexAttribArray((GLuint)program->a_pos);
	glVertexAttribPointer((GLuint)program->a_pos, 2, GL_FLOAT, GL_FALSE, 0, v);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void gl_shadow(struct gl_state *gl, float x, float y, float w, float h, float radius)
{
	float v[8];

	if (w <= 0.0f || h <= 0.0f) {
		return;
	}
	quad_vertices(v, x - (float)AWS_SHADOW_DEPTH, y - (float)AWS_SHADOW_DEPTH,
		      w + 2.0f * (float)AWS_SHADOW_DEPTH, h + 2.0f * (float)AWS_SHADOW_DEPTH);
	glUseProgram(gl->shadow.id);
	glUniform2f(gl->shadow.u_size, (float)gl->width, (float)gl->height);
	glUniform4f(gl->shadow.u_rect, x, y, w, h);
	glUniform1f(gl->shadow.u_radius, radius);
	glUniform1f(gl->shadow.u_alpha, (float)AWS_SHADOW_ALPHA / 255.0f);
	glUniform1f(gl->shadow.u_depth, (float)AWS_SHADOW_DEPTH);
	glEnableVertexAttribArray((GLuint)gl->shadow.a_pos);
	glVertexAttribPointer((GLuint)gl->shadow.a_pos, 2, GL_FLOAT, GL_FALSE, 0, v);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* ---------------------------------------------------------------------------------------------------
 * textures
 * --------------------------------------------------------------------------------------------------- */

/* **A CLIENT BUFFER UPLOADED AS ONE TEXTURE.** GLES2 has no `GL_UNPACK_ROW_LENGTH`, so a stride wider than the
 * row is repacked through a scratch buffer rather than read past the end of a row — the alternative was an
 * extension the renderer does not need to be portable. */
static void upload_client_buffer(struct gl_state *gl, struct wl_shm_buffer *shm,
				 int width, int height, int stride)
{
	const void *data;
	size_t row = (size_t)width * 4u;

	wl_shm_buffer_begin_access(shm);
	data = wl_shm_buffer_get_data(shm);
	if (data == NULL) {
		wl_shm_buffer_end_access(shm);
		return;
	}
	glBindTexture(GL_TEXTURE_2D, gl->client_texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	if ((size_t)stride == row) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	} else {
		size_t needed = row * (size_t)height;
		int y;

		if (needed > gl->scratch_size) {
			unsigned char *grown = realloc(gl->scratch, needed);

			if (grown == NULL) {
				wl_shm_buffer_end_access(shm);
				return;
			}
			gl->scratch = grown;
			gl->scratch_size = needed;
		}
		for (y = 0; y < height; y++) {
			memcpy(gl->scratch + (size_t)y * row,
			       (const unsigned char *)data + (size_t)y * (size_t)stride, row);
		}
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
			     gl->scratch);
	}
	wl_shm_buffer_end_access(shm);
}

/* THE SYSTEM MENU'S MARK, WHICH IS COVERAGE AND NOT COLOUR. `GL_LUMINANCE` is core GLES2 and copies the eight-bit
 * coverage into all three channels plus one alpha, so the shader reads the coverage out of `.r` and takes the ink
 * from a uniform — the same division the CPU renderer makes between `aws_system_icon_coverage` and
 * `AWS_SYSTEM_MENU_GLYPH_COLOUR`. */
static void upload_coverage(struct gl_state *gl, struct aws_system_icon *icon)
{
	const uint8_t *coverage;

	if (icon == NULL || icon->paths == NULL || icon->path_count == 0) {
		icon = aws_system_icon_by_id(AWS_SYSTEM_MENU_ICON_FALLBACK);
	}
	coverage = aws_system_icon_coverage(icon);
	if (coverage == NULL) {
		return;
	}
	glBindTexture(GL_TEXTURE_2D, gl->coverage_texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, AWS_SYSTEM_MENU_GLYPH_WIDTH,
		     AWS_SYSTEM_MENU_GLYPH_HEIGHT, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, coverage);
}

/* ---------------------------------------------------------------------------------------------------
 * the scene
 * --------------------------------------------------------------------------------------------------- */

/* **NOTHING BEHIND THE MENU BAR CASTS A SHADOW ONTO IT**, which is the CPU renderer's rule and now one scissor
 * rectangle instead of a per-row test: the shadow may only land in the rows BELOW the strip. */
static void shadow_clip(struct gl_state *gl, int enable)
{
	if (enable) {
		int below = gl->height - AWS_MENU_BAR_HEIGHT;

		if (below < 0) {
			below = 0;
		}
		glEnable(GL_SCISSOR_TEST);
		glScissor(0, 0, gl->width, below);
	} else {
		glDisable(GL_SCISSOR_TEST);
	}
}

/* ---------------------------------------------------------------------------------------------------
 * dma-buf import
 * --------------------------------------------------------------------------------------------------- */

/* **ONE IMPORTED BUFFER: AN EGLIMAGE AND THE TEXTURE THAT SAMPLES IT.** They are one object because they are one
 * lifetime — the texture is a view of the image, and destroying either alone is the kind of half-release that only
 * shows up as a driver warning an hour later. The renderer allocates this; `aws_dmabuf_buffer.render` is the only
 * pointer to it and the protocol code never looks inside. */
struct gl_import {
	EGLImageKHR image;
	GLuint texture;
	int external;		/* sampled as GL_TEXTURE_EXTERNAL_OES, not GL_TEXTURE_2D */
};

/* THE PLANE ATTRIBUTE NAMES ARE NOT CONTIGUOUS, hence four small tables rather than arithmetic on PLANE0. The
 * modifier pair exists from `EGL_EXT_image_dma_buf_import_modifiers`; without it EGL assumes linear, which is why
 * an implicit modifier is passed by simply not sending the pair. */
static const EGLint plane_fd_attr[4] = { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
					EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT };
static const EGLint plane_offset_attr[4] = { EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
					     EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT };
static const EGLint plane_pitch_attr[4] = { EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
					    EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT };
static const EGLint plane_mod_lo_attr[4] = { EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
					     EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT };
static const EGLint plane_mod_hi_attr[4] = { EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
					     EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT };

/* **WHAT THE DRIVER SAYS IT CAN SAMPLE, NOT WHAT THIS FILE WISHES IT COULD.** `eglQueryDmaBufFormatsEXT` answers with
 * the fourcc list the implementation will import, and `eglQueryDmaBufModifiersEXT` with the modifiers it accepts for
 * each. A compositor that advertised a format the driver refuses would turn every client window into a `failed`
 * event with no diagnosis; asking is what makes the advertisement true. */
static int gl_dmabuf_formats(struct aws_server *server, struct aws_dmabuf_format *out, int max)
{
	struct gl_state *gl = server->renderer->data;
	EGLint count = 0, index;
	EGLint *formats;
	int written = 0;

	if (gl == NULL || !gl->dmabuf || gl->query_formats == NULL || max <= 0) {
		return 0;
	}
	if (!gl->query_formats(gl->display, 0, NULL, &count) || count <= 0) {
		return 0;
	}
	formats = calloc((size_t)count, sizeof *formats);
	if (formats == NULL) {
		return 0;
	}
	if (!gl->query_formats(gl->display, count, formats, &count)) {
		free(formats);
		return 0;
	}
	for (index = 0; index < count && written < max; index++) {
		EGLint modifier_count = 0;

		if (gl->query_modifiers != NULL &&
		    gl->query_modifiers(gl->display, formats[index], 0, NULL, NULL, &modifier_count) &&
		    modifier_count > 0) {
			EGLuint64KHR *modifiers = calloc((size_t)modifier_count, sizeof *modifiers);
			EGLBoolean *external = calloc((size_t)modifier_count, sizeof *external);
			int m;

			if (modifiers == NULL || external == NULL) {
				free(modifiers);
				free(external);
				continue;
			}
			gl->query_modifiers(gl->display, formats[index], modifier_count, modifiers, external,
					    &modifier_count);
			for (m = 0; m < modifier_count && written < max; m++) {
				/* **AN `external_only` MODIFIER CANNOT BE SAMPLED AS `GL_TEXTURE_2D`, AND THIS RENDERER
				 * SAMPLES WITH IT.** The query answers, per modifier, whether the image may only be bound to
				 * `GL_TEXTURE_EXTERNAL_OES`; advertising one of those would be a buffer that imports and then
				 * refuses to become a texture — MEASURED, as `GL_INVALID_OPERATION` out of
				 * `glEGLImageTargetTexture2DOES`. */
				if (modifiers[m] == AWS_DRM_FORMAT_MOD_INVALID) {
					continue;
				}
				/* **AN `external_only` MODIFIER IS ADVERTISED ONLY IF THIS CONTEXT CAN SAMPLE IT.** The image
				 * can then be bound to `GL_TEXTURE_EXTERNAL_OES` only; a build whose GL lacks
				 * `GL_OES_EGL_image_external` must not offer it, because the client would allocate and the
				 * compositor could not draw. */
				if (external[m] && !gl->has_external) {
					continue;
				}
				if (getenv("AWS_DMABUF_DEBUG") != NULL) {
					fprintf(stderr, "  DIAG dmabuf: driver offers format 0x%08x modifier "
						"0x%016llx (external_only=%d)\n",
						(uint32_t)formats[index], (unsigned long long)modifiers[m],
						(int)external[m]);
				}
				out[written].format = (uint32_t)formats[index];
				out[written].modifier = modifiers[m];
				written++;
			}
			free(modifiers);
			free(external);
		} else if (written < max) {
			/* No modifier query: linear is the only thing EGL can be told without one. */
			out[written].format = (uint32_t)formats[index];
			out[written].modifier = AWS_DRM_FORMAT_MOD_LINEAR;
			written++;
		}
	}
	free(formats);
	return written;
}

/* **WHICH MODIFIERS ARE `external_only`, ASKED PER BUFFER.** The answer is a property of the (format, modifier)
 * pair, so it is looked up at import rather than carried through the protocol — the wire only carries the pair.
 * A driver without the modifier query gets "no", which is the linear assumption EGL makes anyway. */
static int modifier_external_only(struct gl_state *gl, uint32_t format, uint64_t modifier)
{
	EGLuint64KHR *modifiers;
	EGLBoolean *external;
	EGLint count = 0;
	int i, result = 0;

	if (gl->query_modifiers == NULL) {
		return 0;
	}
	if (!gl->query_modifiers(gl->display, (EGLint)format, 0, NULL, NULL, &count) || count <= 0) {
		return 0;
	}
	modifiers = calloc((size_t)count, sizeof *modifiers);
	external = calloc((size_t)count, sizeof *external);
	if (modifiers == NULL || external == NULL) {
		free(modifiers);
		free(external);
		return 0;
	}
	gl->query_modifiers(gl->display, (EGLint)format, count, modifiers, external, &count);
	for (i = 0; i < count; i++) {
		if (modifiers[i] == modifier) {
			result = external[i] ? 1 : 0;
			break;
		}
	}
	free(modifiers);
	free(external);
	return result;
}

/* **ONE IMPORT PER BUFFER, NOT ONE PER FRAME.** This is the whole point of the exercise: the EGLImage takes its own
 * reference to the client's memory, so the texture samples it where the client wrote it until the buffer is gone. */
static int gl_dmabuf_import(struct aws_server *server, struct aws_dmabuf_buffer *buffer)
{
	struct gl_state *gl = server->renderer->data;
	struct gl_import *import;
	EGLint attributes[64];
	uint64_t modifier;
	EGLImageKHR image;
	GLuint texture;
	GLenum target;
	int external;
	int n = 0, i;

	if (gl == NULL || !gl->dmabuf || gl->create_image == NULL || gl->image_target_texture == NULL) {
		return -1;
	}
	/* **THE ERROR QUEUE IS CLEARED FIRST, SO THE ANSWER BELOW IS THIS CALL'S OWN.** GL errors are sticky until read,
	 * and a compositor that draws before it imports would otherwise attribute an earlier mistake to the import. */
	while (glGetError() != GL_NO_ERROR) {
	}
	modifier = buffer->planes[0].modifier;
	/* **AND THE TARGET IS THE DRIVER'S ANSWER, NOT A PREFERENCE.** A modifier reported as `external_only` can be
	 * bound only to `GL_TEXTURE_EXTERNAL_OES`; binding it to `GL_TEXTURE_2D` is `GL_INVALID_OPERATION` with no hint
	 * as to why. MEASURED on NVIDIA, whose LINEAR XRGB8888 is external-only. */
	external = modifier_external_only(gl, buffer->format, modifier);
	if (external && !gl->has_external) {
		fprintf(stderr, "window-server: gl: this buffer needs GL_TEXTURE_EXTERNAL_OES and GL has no "
				"GL_OES_EGL_image_external\n");
		return -1;
	}
	target = external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
	attributes[n++] = EGL_LINUX_DRM_FOURCC_EXT;
	attributes[n++] = (EGLint)buffer->format;
	attributes[n++] = EGL_WIDTH;
	attributes[n++] = buffer->width;
	attributes[n++] = EGL_HEIGHT;
	attributes[n++] = buffer->height;
	for (i = 0; i < buffer->plane_count && i < 4; i++) {
		attributes[n++] = plane_fd_attr[i];
		attributes[n++] = buffer->planes[i].fd;
		attributes[n++] = plane_offset_attr[i];
		attributes[n++] = (EGLint)buffer->planes[i].offset;
		attributes[n++] = plane_pitch_attr[i];
		attributes[n++] = (EGLint)buffer->planes[i].stride;
		/* **AN IMPLICIT MODIFIER IS THE ABSENCE OF THE PAIR, NOT A VALUE.** `DRM_FORMAT_MOD_INVALID` asks EGL to
		 * work the layout out; sending it as a modifier asks it to import a buffer whose modifier is invalid. */
		if (modifier != AWS_DRM_FORMAT_MOD_INVALID) {
			attributes[n++] = plane_mod_lo_attr[i];
			attributes[n++] = (EGLint)(uint32_t)(modifier & 0xFFFFFFFFu);
			attributes[n++] = plane_mod_hi_attr[i];
			attributes[n++] = (EGLint)(uint32_t)(modifier >> 32);
		}
	}
	attributes[n++] = EGL_NONE;

	image = gl->create_image(gl->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
				 (EGLClientBuffer)NULL, attributes);
	if (image == EGL_NO_IMAGE_KHR) {
		fprintf(stderr, "window-server: gl: dma-buf import failed (EGL error 0x%x, format 0x%x)\n",
			(unsigned)eglGetError(), buffer->format);
		return -1;
	}
	glGenTextures(1, &texture);
	if (texture == 0) {
		fprintf(stderr, "window-server: gl: no texture for the dma-buf (0x%x)\n", (unsigned)glGetError());
		gl->destroy_image(gl->display, image);
		return -1;
	}
	glBindTexture(target, texture);
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl->image_target_texture(target, (GLeglImageOES)image);
	{
		GLenum gl_error = glGetError();

		if (gl_error != GL_NO_ERROR) {
			fprintf(stderr, "window-server: gl: EGLImage could not become a texture (0x%x)\n",
				(unsigned)gl_error);
			glDeleteTextures(1, &texture);
			gl->destroy_image(gl->display, image);
			return -1;
		}
	}
	import = calloc(1, sizeof *import);
	if (import == NULL) {
		glDeleteTextures(1, &texture);
		gl->destroy_image(gl->display, image);
		return -1;
	}
	import->image = image;
	import->texture = texture;
	import->external = external;
	buffer->render = import;
	return 0;
}

static void gl_dmabuf_release(struct aws_server *server, struct aws_dmabuf_buffer *buffer)
{
	struct gl_state *gl = server->renderer->data;
	struct gl_import *import = buffer->render;

	if (import == NULL) {
		return;
	}
	if (gl != NULL) {
		glDeleteTextures(1, &import->texture);
		if (import->image != EGL_NO_IMAGE_KHR && gl->destroy_image != NULL) {
			gl->destroy_image(gl->display, import->image);
		}
	}
	free(import);
	buffer->render = NULL;
}

static void gl_draw_menu_bar(struct aws_server *server)
{
	struct gl_state *gl = server->renderer->data;

	gl_solid(gl, 0.0f, 0.0f, (float)gl->width, (float)AWS_MENU_BAR_HEIGHT, AWS_MENU_BAR_COLOUR);
	upload_coverage(gl, server->system_menu_icon);
	gl_textured(gl, &gl->texture, GL_TEXTURE_2D, gl->coverage_texture, (float)AWS_SYSTEM_MENU_LEFT_MARGIN,
		    (float)AWS_SYSTEM_MENU_GLYPH_TOP, (float)AWS_SYSTEM_MENU_GLYPH_WIDTH,
		    (float)AWS_SYSTEM_MENU_GLYPH_HEIGHT, (float)AWS_SYSTEM_MENU_GLYPH_WIDTH,
		    (float)AWS_SYSTEM_MENU_GLYPH_HEIGHT, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1, 0,
		    AWS_SYSTEM_MENU_GLYPH_COLOUR);
	/* **AND THE BAR'S OWN SHADOW, WHICH FALLS BELOW THE STRIP.** Radius zero: the bar's corners are square, and a
	 * shadow with a radius under a square bar painted a smudge INSIDE its own corners. MEASURED in the CPU
	 * renderer, and this is the same number. */
	shadow_clip(gl, 1);
	gl_shadow(gl, 0.0f, 0.0f, (float)gl->width, (float)AWS_MENU_BAR_HEIGHT, 0.0f);
	shadow_clip(gl, 0);
}

static void gl_draw_surface(struct aws_server *server, struct aws_surface *surface)
{
	struct gl_state *gl = server->renderer->data;
	/* **TWO KINDS OF BUFFER, ONE QUAD.** A dma-buf was imported once and is already a texture; a `wl_shm` buffer is
	 * CPU memory and is uploaded here, every frame, which is the cost the other path removes. Everything after this
	 * resolution — the clip to the frame, the four corner decisions, the quad — is the same code for both, because
	 * the shape of a window is not a property of where its pixels live. */
	struct aws_dmabuf_buffer *dmabuf = aws_dmabuf_buffer_from_resource(surface->current_buffer);
	struct wl_shm_buffer *shm = NULL;
	GLuint texture = 0;
	float uv_w, uv_h;
	int from_shm = 0;
	int width, height, stride = 0;
	int transparent;
	int in_bar;
	int hangs_from_bar;
	float radius;
	float tl, tr, bl, br;

	if (dmabuf != NULL && dmabuf->render != NULL) {
		struct gl_import *import = dmabuf->render;

		width = dmabuf->width;
		height = dmabuf->height;
		texture = import->texture;
		transparent = aws_dmabuf_format_has_alpha(dmabuf->format);
	} else {
		shm = wl_shm_buffer_get(surface->current_buffer);
		if (shm == NULL) {
			return;
		}
		width = wl_shm_buffer_get_width(shm);
		height = wl_shm_buffer_get_height(shm);
		stride = wl_shm_buffer_get_stride(shm);
		transparent = wl_shm_buffer_get_format(shm) == WL_SHM_FORMAT_ARGB8888;
		from_shm = 1;
	}
	/* **AND THE FRAME IS THE WINDOW'S BOUNDS, SO A BIGGER BUFFER IS CLIPPED TO IT** — the CPU renderer's rule,
	 * and the reason a 320x200 status item does not run down the screen. */
	if (surface->frame_width > 0 && width > surface->frame_width) {
		width = surface->frame_width;
	}
	if (surface->frame_height > 0 && height > surface->frame_height) {
		height = surface->frame_height;
	}
	if (width <= 0 || height <= 0) {
		return;
	}
	/* The shm texture holds only the clamped region; the imported one is the whole buffer. */
	uv_w = from_shm ? (float)width : (float)dmabuf->width;
	uv_h = from_shm ? (float)height : (float)dmabuf->height;

	/* **THE FOUR CORNER DECISIONS, WHICH ARE `shape_coverage`'S AND NOT A SECOND OPINION.** A corner is rounded
	 * only where the window is free of the output's own edges; a menu hanging from the bar keeps its top corners
	 * square; a surface entirely inside the bar is a piece of the bar and is square all round. */
	in_bar = surface->y >= 0 && surface->y + surface->frame_height <= AWS_MENU_BAR_HEIGHT;
	hangs_from_bar = surface->y == AWS_MENU_BAR_HEIGHT;
	radius = (float)AWS_SHADOW_RADIUS;
	if (radius > (float)width / 2.0f) {
		radius = (float)width / 2.0f;
	}
	if (radius > (float)height / 2.0f) {
		radius = (float)height / 2.0f;
	}
	tl = (!in_bar && !hangs_from_bar && surface->x > 0 && surface->y > 0) ? 1.0f : 0.0f;
	tr = (!in_bar && !hangs_from_bar && surface->x + width < gl->width && surface->y > 0) ? 1.0f : 0.0f;
	bl = (!in_bar && surface->x > 0 && surface->y + height < gl->height) ? 1.0f : 0.0f;
	br = (!in_bar && surface->x + width < gl->width && surface->y + height < gl->height) ? 1.0f : 0.0f;

	/* THE SHADOW FIRST, because it is a shadow on the things behind. */
	if (!in_bar) {
		shadow_clip(gl, 1);
		gl_shadow(gl, (float)surface->x, (float)surface->y, (float)surface->frame_width,
			  (float)surface->frame_height, (float)AWS_SHADOW_RADIUS);
		shadow_clip(gl, 0);
	}

	if (from_shm) {
		upload_client_buffer(gl, shm, width, height, stride);
		texture = gl->client_texture;
		gl_textured(gl, &gl->texture, GL_TEXTURE_2D, texture, (float)surface->x, (float)surface->y,
			    (float)width, (float)height, uv_w, uv_h, radius, tl, tr, bl, br, 0,
			    !transparent, 0);
	} else {
		struct gl_import *import = dmabuf->render;

		gl_textured(gl, import->external ? &gl->texture_external : &gl->texture,
			    import->external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, texture,
			    (float)surface->x, (float)surface->y, (float)width, (float)height, uv_w, uv_h,
			    radius, tl, tr, bl, br, 0, !transparent, 0);
	}
}

static void gl_emit(struct aws_server *server, struct aws_surface *surface, uint32_t time)
{
	if (surface == NULL) {
		gl_draw_menu_bar(server);
		return;
	}
	gl_draw_surface(server, surface);
	aws_surface_frame_done(surface, time);
}

/* **THE SCREENSHOT IS A QUESTION ABOUT THE SCENE, NOT ABOUT THE RENDERER.** `--screenshot` is how every test in this
 * tree photographs a frame, so the GL path reads the frame back into `backend->pixels` before it swaps — RGBA bytes
 * from the bottom row up, packed into the top-down XRGB8888 the PPM writer and `inspect.py` already understand. */
static void gl_readback(struct aws_server *server)
{
	struct gl_state *gl = server->renderer->data;
	int width = gl->width;
	int height = gl->height;
	size_t count = (size_t)width * (size_t)height;
	unsigned char *pixels = malloc(count * 4u);
	int x, y;

	if (pixels == NULL) {
		return;
	}
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	for (y = 0; y < height; y++) {
		const unsigned char *source = pixels + (size_t)(height - 1 - y) * (size_t)width * 4u;
		uint32_t *destination = server->backend.pixels + (size_t)y * (size_t)width;

		for (x = 0; x < width; x++) {
			destination[x] = 0xFF000000u
				| ((uint32_t)source[x * 4 + 0] << 16)
				| ((uint32_t)source[x * 4 + 1] << 8)
				| (uint32_t)source[x * 4 + 2];
		}
	}
	free(pixels);
}

static void gl_draw(struct aws_server *server)
{
	struct gl_state *gl = server->renderer->data;
	float r, g, b;

	rgb_of(AWS_BACKGROUND, &r, &g, &b);
	glViewport(0, 0, gl->width, gl->height);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(r, g, b, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	aws_output_walk(server, gl_emit);
	/* **A SCREENSHOT'S BUFFER IS MADE WHEN IT IS FIRST WANTED, NOT WHEN THE CONTEXT IS.** `--screenshot` is parsed
	 * after the server (and so after this renderer) is created, so a buffer allocated at init time would be one
	 * nobody asked for; and the readback is a debug path, so three and a half megabytes are not worth holding for
	 * a run that never takes a photograph. */
	if (server->screenshot != NULL) {
		if (server->backend.pixels == NULL) {
			server->backend.pixels =
				calloc((size_t)gl->width * (size_t)gl->height, sizeof(uint32_t));
			gl->own_pixels = server->backend.pixels != NULL;
		}
		if (server->backend.pixels != NULL) {
			gl_readback(server);
		}
	}
}

/* ---------------------------------------------------------------------------------------------------
 * lifecycle
 * --------------------------------------------------------------------------------------------------- */

/* THE SWAP IS THE PRESENT. It replaces `backend.present` — the X11 backend's XShm/XPutImage path — because the
 * frame never becomes CPU pixels to put. `eglSwapInterval(1)` paces the swap to the display's refresh, which is the
 * same pacing the DRM backend gets from `busy` and a page flip. */
static void gl_present(struct aws_server *server)
{
	struct gl_state *gl = server->renderer->data;

	if (gl != NULL && gl->surface != EGL_NO_SURFACE) {
		eglSwapBuffers(gl->display, gl->surface);
	}
}

static void gl_stop(struct aws_server *server)
{
	struct gl_state *gl = server->renderer->data;

	if (gl == NULL) {
		return;
	}
	if (gl->display != EGL_NO_DISPLAY) {
		eglMakeCurrent(gl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (gl->surface != EGL_NO_SURFACE) {
			eglDestroySurface(gl->display, gl->surface);
		}
		if (gl->context != EGL_NO_CONTEXT) {
			eglDestroyContext(gl->display, gl->context);
		}
		eglTerminate(gl->display);
	}
	free(gl->scratch);
	/* **WHOEVER ALLOCATED THE PIXELS FREES THEM, AND HERE THAT IS US.** The readback buffer exists only because
	 * `--screenshot` asked for one; the X11 backend gave its own copy-path memory back when the renderer took the
	 * present, so `backend.pixels` is this struct's or nothing at all. */
	if (gl->own_pixels) {
		free(server->backend.pixels);
	}
	server->backend.pixels = NULL;
	server->backend.present = NULL;
	free(gl);
	server->renderer->data = NULL;
}

/* **IS AN EXTENSION IN THE LIST?** `strstr` would answer yes for `EGL_EXT_image_dma_buf_import_modifiers` when asked
 * about `EGL_EXT_image_dma_buf_import`, which is a different capability — so the list is walked by token. */
static int extension_present(const char *list, const char *name)
{
	size_t length = strlen(name);
	const char *p = list;

	while (p != NULL && *p != '\0') {
		const char *end = strchr(p, ' ');
		size_t token = end != NULL ? (size_t)(end - p) : strlen(p);

		if (token == length && strncmp(p, name, length) == 0) {
			return 1;
		}
		if (end == NULL) {
			break;
		}
		p = end + 1;
	}
	return 0;
}

/* **THE CONFIG HAS TO MATCH THE WINDOW'S VISUAL, AND THAT IS THE ONE REAL FOOTGUN OF EGL-ON-X11.** The X11 backend
 * creates its window with `DefaultVisual` before any renderer exists, so the EGL config is chosen by asking every
 * config for its `EGL_NATIVE_VISUAL_ID` and taking one that equals that visual — not by `eglChooseConfig` and
 * hoping. A mismatch is `EGL_BAD_MATCH` on `eglCreateWindowSurface`, and a renderer that half-initialised would be
 * worse than one that says which visual it could not find a config for. */
static int choose_config(struct gl_state *gl, unsigned long visual)
{
	EGLint count = 0, index;
	EGLConfig *configs;

	if (!eglGetConfigs(gl->display, NULL, 0, &count) || count <= 0) {
		return -1;
	}
	configs = calloc((size_t)count, sizeof *configs);
	if (configs == NULL) {
		return -1;
	}
	if (!eglGetConfigs(gl->display, configs, count, &count)) {
		free(configs);
		return -1;
	}
	for (index = 0; index < count; index++) {
		EGLint native = 0, renderable = 0, surface = 0;

		eglGetConfigAttrib(gl->display, configs[index], EGL_NATIVE_VISUAL_ID, &native);
		eglGetConfigAttrib(gl->display, configs[index], EGL_RENDERABLE_TYPE, &renderable);
		eglGetConfigAttrib(gl->display, configs[index], EGL_SURFACE_TYPE, &surface);
		if ((unsigned long)native == visual && (surface & EGL_WINDOW_BIT) &&
		    (renderable & EGL_OPENGL_ES2_BIT)) {
			gl->config = configs[index];
			free(configs);
			return 0;
		}
	}
	free(configs);
	return -1;
}

static int make_texture(struct gl_state *gl, GLuint *texture)
{
	glGenTextures(1, texture);
	if (*texture == 0) {
		return -1;
	}
	glBindTexture(GL_TEXTURE_2D, *texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return 0;
}

int aws_renderer_gl_create(struct aws_server *server)
{
	struct gl_state *gl;
	EGLint major = 0, minor = 0;
	EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	struct aws_renderer *renderer;

	if (server->backend.native_display == NULL || server->backend.native_window == 0) {
		/* NOT AN ERROR AND NOT A WORD: `auto` asks every backend, and the ones that cannot answer are simply
		 * the ones that cannot. A backend asked for by name that lacks it has already failed above. */
		return -1;
	}
	gl = calloc(1, sizeof *gl);
	if (gl == NULL) {
		return -1;
	}
	gl->width = server->backend.width;
	gl->height = server->backend.height;
	gl->surface = EGL_NO_SURFACE;
	gl->context = EGL_NO_CONTEXT;
	gl->display = eglGetDisplay((EGLNativeDisplayType)server->backend.native_display);
	if (gl->display == EGL_NO_DISPLAY) {
		fprintf(stderr, "window-server: gl: eglGetDisplay failed (0x%x)\n", (unsigned)eglGetError());
		goto fail;
	}
	if (!eglInitialize(gl->display, &major, &minor)) {
		fprintf(stderr, "window-server: gl: eglInitialize failed (0x%x)\n", (unsigned)eglGetError());
		gl->display = EGL_NO_DISPLAY;
		goto fail;
	}
	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		fprintf(stderr, "window-server: gl: eglBindAPI(OPENGL_ES) failed (0x%x)\n",
			(unsigned)eglGetError());
		goto fail;
	}
	if (choose_config(gl, server->backend.native_visual) != 0) {
		fprintf(stderr, "window-server: gl: no EGL config matches the window's visual (0x%lx)\n",
			server->backend.native_visual);
		goto fail;
	}
	gl->context = eglCreateContext(gl->display, gl->config, EGL_NO_CONTEXT, context_attributes);
	if (gl->context == EGL_NO_CONTEXT) {
		fprintf(stderr, "window-server: gl: eglCreateContext failed (0x%x)\n", (unsigned)eglGetError());
		goto fail;
	}
	gl->surface = eglCreateWindowSurface(gl->display, gl->config,
					     (EGLNativeWindowType)server->backend.native_window, NULL);
	if (gl->surface == EGL_NO_SURFACE) {
		fprintf(stderr, "window-server: gl: eglCreateWindowSurface failed (0x%x)\n",
			(unsigned)eglGetError());
		goto fail;
	}
	if (!eglMakeCurrent(gl->display, gl->surface, gl->surface, gl->context)) {
		fprintf(stderr, "window-server: gl: eglMakeCurrent failed (0x%x)\n", (unsigned)eglGetError());
		goto fail;
	}
	eglSwapInterval(gl->display, 1);

	/* **AND WHETHER THIS GL CAN SAMPLE AN EXTERNAL IMAGE.** Some drivers refuse `GL_TEXTURE_2D` for a dma-buf
	 * modifier and offer only `GL_TEXTURE_EXTERNAL_OES`; the shader for that is linked only when the extension is
	 * there, and the advertisement follows this flag. */
	{
		const char *gl_extensions = (const char *)glGetString(GL_EXTENSIONS);

		gl->has_external = gl_extensions != NULL &&
			extension_present(gl_extensions, "GL_OES_EGL_image_external");
	}

	/* **WHAT THIS DRIVER CAN IMPORT, ASKED RATHER THAN ASSUMED.** The imports are extension functions, so they are
	 * resolved by name; the two queries only exist when the modifiers extension does, and without them EGL assumes
	 * a linear layout — so the pair is loaded together. */
	{
		const char *extensions = eglQueryString(gl->display, EGL_EXTENSIONS);

		if (extensions != NULL && extension_present(extensions, "EGL_EXT_image_dma_buf_import")) {
			gl->create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
			gl->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
			gl->image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
				eglGetProcAddress("glEGLImageTargetTexture2DOES");
			if (extension_present(extensions, "EGL_EXT_image_dma_buf_import_modifiers")) {
				gl->query_formats = (PFNEGLQUERYDMABUFFORMATSEXTPROC)
					eglGetProcAddress("eglQueryDmaBufFormatsEXT");
				gl->query_modifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)
					eglGetProcAddress("eglQueryDmaBufModifiersEXT");
			}
			gl->dmabuf = gl->create_image != NULL && gl->destroy_image != NULL &&
				     gl->image_target_texture != NULL;
		}
		if (!gl->dmabuf) {
			fprintf(stderr, "window-server: gl: no dma-buf import on this EGL implementation; "
					"clients will use shared memory\n");
		}
	}

	if (link_program(&gl->solid, solid_source) != 0 ||
	    link_program(&gl->texture, texture_source) != 0 ||
	    link_program(&gl->shadow, shadow_source) != 0) {
		goto fail;
	}
	if (gl->has_external && link_program(&gl->texture_external, texture_external_source) != 0) {
		goto fail;
	}
	if (make_texture(gl, &gl->client_texture) != 0 || make_texture(gl, &gl->coverage_texture) != 0) {
		goto fail;
	}
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	renderer = calloc(1, sizeof *renderer);
	if (renderer == NULL) {
		goto fail;
	}
	renderer->name = "gl";
	renderer->draw = gl_draw;
	renderer->stop = gl_stop;
	renderer->data = gl;
	server->renderer = renderer;
	/* **AND THE dma-buf HOOKS, WHICH ARE WHAT MAKES THE PROTOCOL EXIST AT ALL.** `aws_dmabuf_init` asks this
	 * renderer for its (format, modifier) list and advertises nothing when the answer is empty, so a driver without
	 * the extension simply never offers GPU buffers. */
	if (gl->dmabuf) {
		renderer->dmabuf_formats = gl_dmabuf_formats;
		renderer->dmabuf_import = gl_dmabuf_import;
		renderer->dmabuf_release = gl_dmabuf_release;
	}

	/* **THE COPY PATH GIVES ITS MEMORY BACK FIRST, AND THE SWAP TAKES THE PRESENT SECOND.** The backend's XShm
	 * segment and XImage exist to put a CPU frame on the screen; with a GPU frame they are dead weight, and
	 * leaving them would be a present pointer that puts the OLD pixels over the new ones. */
	if (server->backend.release_present != NULL) {
		server->backend.release_present(server);
	}
	server->backend.present = gl_present;

	fprintf(stderr, "window-server: gl renderer: EGL %d.%d, %s\n", major, minor,
		glGetString(GL_RENDERER) != NULL ? (const char *)glGetString(GL_RENDERER) : "unknown");
	return 0;

fail:
	/* A HALF-MADE CONTEXT IS TORN DOWN IN THE ORDER IT WAS MADE, because the surface and the context both hold
	 * references to the display and destroying it under them leaks rather than crashes — which is worse. */
	if (gl->display != EGL_NO_DISPLAY) {
		eglMakeCurrent(gl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (gl->surface != EGL_NO_SURFACE) {
			eglDestroySurface(gl->display, gl->surface);
		}
		if (gl->context != EGL_NO_CONTEXT) {
			eglDestroyContext(gl->display, gl->context);
		}
		eglTerminate(gl->display);
	}
	free(gl->scratch);
	free(gl);
	return -1;
}

#else /* !AWS_HAVE_GL */

int aws_renderer_gl_create(struct aws_server *server)
{
	(void)server;
	/* BUILT OUT RATHER THAN FAILING MYSTERIOUSLY: a build without EGL/GLES2 has to say so where a caller would
	 * look for the reason, exactly as the x11 backend does when it was configured without libX11. */
	fprintf(stderr, "window-server: this build has no gl renderer (it was configured without EGL/GLES2)\n");
	return -1;
}

#endif /* AWS_HAVE_GL */
