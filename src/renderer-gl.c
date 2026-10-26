/*
 * renderer-gl.c — the same scene, drawn by the GPU through EGL and GLES2.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WHY THIS FILE EXISTS AND WHY IT IS TESTED NESTED FIRST. There are two independent halves to "acceleration": the
 * RENDERER (turning surfaces, shadows and the bar into triangles) and the DISPLAY PATH (how the finished frame
 * reaches the glass — a GBM surface and an atomic plane commit on DRM/KMS, or `eglSwapBuffers` on a window). The
 * renderer is the expensive, backend-agnostic half, and the nested X11 backend is where it can be watched: it needs
 * no DRM master, no KMS driver, no free virtual terminal and no logind seat, only an X server with a GPU behind it.
 * So this renderer grew up in the nested output, and the DRM backend's accelerated path is a GBM swapchain added
 * under it rather than a second renderer beside it — see `aws_renderer_gl_create` and `gl_present`.
 *
 * IT IS THE SAME PICTURE AS `renderer-cpu.c`, AND THAT IS TESTABLE. Both are walked by `aws_output_walk`, both take
 * the same shadow constants from `server.h`, and both answer `--screenshot`: this one calls `glReadPixels` into
 * `backend->pixels` before it swaps, so `inspect.py` can compare a CPU frame and a GL frame of the same scene. A
 * renderer that looked different would be a bug with a number attached, not a matter of taste.
 *
 * WHAT IT DOES NOT DO YET, recorded rather than left to be found: client buffers arrive through `wl_shm` or
 * `zwp_linux_dmabuf_v1`, and the shm ones are uploaded as textures every frame, so this makes COMPOSITION cheap and not
 * client painting. The AppKit still draws on the CPU.
 */

#include "server.h"

#include <pixman.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AWS_HAVE_GL

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#ifdef AWS_HAVE_GBM
#include <gbm.h>
#endif

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
	"uniform vec2 u_box;\n"
	"uniform float u_radius;\n"
	"uniform vec4 u_round;\n"
	"uniform float u_opaque;\n"
	"uniform float u_coverage;\n"
	"uniform vec4 u_ink;\n"
	"vec4 sample_box(vec2 uv) {\n"
	"    if (u_box.x <= 0.0) { return texture2D(u_tex, uv); }\n"
	"    return 0.25 * (texture2D(u_tex, uv + vec2(-u_box.x, -u_box.y)) +\n"
	"                   texture2D(u_tex, uv + vec2( u_box.x, -u_box.y)) +\n"
	"                   texture2D(u_tex, uv + vec2(-u_box.x,  u_box.y)) +\n"
	"                   texture2D(u_tex, uv + vec2( u_box.x,  u_box.y)));\n"
	"}\n"
	"void main() {\n"
	"    vec2 local = v_pos - u_rect.xy;\n"
	"    vec4 t = sample_box(local / u_uv_div);\n"
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

/* **THE CURSOR IS THE ONLY QUAD WITH NO ROUNDED RECTANGLE IN IT.** Everything else this renderer draws is a
 * surface or furniture with a corner rule; the pointer's shape is its own antialiased alpha, so the shader samples the
 * texture and nothing else. The channel swap is the same one the window shader makes: the image is 0xAARRGGBB in
 * memory, which is BGRA on the wire, so `.bgr` puts red back in `.r`. */
static const char *const cursor_source =
	"precision mediump float;\n"
	"varying vec2 v_pos;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec4 u_rect;\n"
	"uniform vec2 u_uv_div;\n"
	"void main() {\n"
	"    vec4 t = texture2D(u_tex, (v_pos - u_rect.xy) / u_uv_div);\n"
	"    if (t.a <= 0.0) { discard; }\n"
	"    gl_FragColor = vec4(t.bgr, t.a);\n"
	"}\n";

/* **AND THE FRAME ITSELF, COPIED OUT OF ITS OWN TEXTURE.** The scene is composed into an offscreen texture that
 * SURVIVES a swap (see `struct gl_state`), and this is the one quad that puts it on the glass. It samples without the
 * BGRA swap the client shaders make — by the time a pixel is in the frame texture it is already in the order the
 * screen wants — and forces alpha to one, because the frame is opaque even when a window on it is not. */
static const char *const blit_source =
	"precision mediump float;\n"
	"varying vec2 v_pos;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec4 u_rect;\n"
	"uniform vec2 u_uv_div;\n"
	"void main() {\n"
	"    vec2 uv = (v_pos - u_rect.xy) / u_uv_div;\n"
	"    gl_FragColor = vec4(texture2D(u_tex, vec2(uv.x, 1.0 - uv.y)).rgb, 1.0);\n"
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
	"uniform vec2 u_box;\n"
	"uniform float u_radius;\n"
	"uniform vec4 u_round;\n"
	"uniform float u_opaque;\n"
	"uniform float u_coverage;\n"
	"uniform vec4 u_ink;\n"
	"vec4 sample_box(vec2 uv) {\n"
	"    if (u_box.x <= 0.0) { return texture2D(u_tex, uv); }\n"
	"    return 0.25 * (texture2D(u_tex, uv + vec2(-u_box.x, -u_box.y)) +\n"
	"                   texture2D(u_tex, uv + vec2( u_box.x, -u_box.y)) +\n"
	"                   texture2D(u_tex, uv + vec2(-u_box.x,  u_box.y)) +\n"
	"                   texture2D(u_tex, uv + vec2( u_box.x,  u_box.y)));\n"
	"}\n"
	"void main() {\n"
	"    vec2 local = v_pos - u_rect.xy;\n"
	"    vec4 t = sample_box(local / u_uv_div);\n"
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
	GLint u_box;
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
	struct gl_program cursor;

	GLuint client_texture;		/* the surface being drawn, re-uploaded per draw */
	/* **THE POINTER'S OWN TEXTURE, UPLOADED WHEN THE IMAGE IS RE-MADE.** The pointer's raster is a fact about the
	 * output's scale and the session's theme, not about the frame, so it is uploaded once per image rather than sixty
	 * times a second. The image says which one it is (`aws_cursor.generation`), because a scale alone would miss a theme
	 * or size that changed under a running server at the same scale. */
	GLuint cursor_texture;
	unsigned cursor_generation;	/* the generation in `cursor_texture` */
	int cursor_uploaded;		/* whether `cursor_texture` holds an image at all */
	GLuint coverage_texture;	/* the System menu's mark, uploaded when the mark or its size changes */
	int coverage_ready;		/* whether `coverage_texture` holds anything yet */
	int coverage_pixels;		/* the uploaded grid's side, in device pixels */
	struct aws_system_icon *coverage_icon;	/* which mark is in the texture */
	unsigned char *scratch;		/* rows repacked when a shm stride is not w * 4 */
	size_t scratch_size;

	int width;
	int height;
	int own_pixels;			/* the readback buffer is ours to free */
	/* **THE FRAME LIVES IN ITS OWN TEXTURE, SO THE SWAP'S BEHAVIOUR DOES NOT DECIDE WHAT A FRAME IS.** A swap may
	 * destroy the back buffer (EGL's default; GBM's next scanout buffer is a different allocation every frame), so a
	 * damage box painted into the back buffer could not be trusted to sit on top of the last frame. The scene is
	 * therefore composed into `frame_texture` through a framebuffer object that PERSISTS across swaps — a damage box
	 * paints the box and leaves the rest of the texture as it was — and one full-viewport quad copies the texture to the
	 * back buffer before the swap. The copy is a GPU fill; the scene draws are what the box saves. */
	struct gl_program blit;
	GLuint frame_texture;
	GLuint frame_fbo;
	/* **AND THE FRAME'S BOX AS GL STATE, BECAUSE TWO THINGS IN A PASS BOTH WANT THE SCISSOR.** The frame's damage box
	 * is what keeps a partial frame partial; the shadow's row limit (nothing casts a shadow up across the bar or across a
	 * sheet's parent title bar) is what keeps a shadow off somebody else's pixels. They are not alternatives — a shadow in
	 * a partial frame must honour both — so the shadow's scissor is their INTERSECTION and the restore puts the frame's
	 * back rather than switching the test off. MEASURED as the bug this fixes: the shadow clip disabled the scissor
	 * mid-pass, and the surface drawn after it painted its whole quad — a poisoned buffer on screen outside the strip the
	 * client had actually damaged. */
	struct aws_box frame_box;
	int frame_box_active;

#ifdef AWS_HAVE_GBM
	/* **A GBM SURFACE IS THE DRM BACKEND'S "WINDOW".** With no window system, EGL is given the card's GBM device;
	 * the renderer makes a scanout-capable surface from it, renders into it, and on present hands the locked front
	 * buffer to the backend to put on the plane. The DEVICE belongs to the backend (it made it from the DRM fd),
	 * the SURFACE belongs to this renderer — and `server->backend.gbm_surface` is the loan that lets the backend
	 * release a buffer back once the flip that stopped scanning it has completed. */
	int gbm;
	struct gbm_device *gbm_device;
	struct gbm_surface *gbm_surface;
	uint32_t gbm_format;
#endif
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
	program->u_box = glGetUniformLocation(program->id, "u_box");
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

/* **A ROUNDED DEVICE POSITION, HALF AWAY FROM ZERO, WITHOUT PULLING IN LIBM.** The compositor's placement is
 * `round(logical * scale)`, the same rule `cpu_device_edge` applies, and the two renderers must agree on it. */
static float gl_round(double value)
{
	return (float)(value >= 0.0 ? (long)(value + 0.5) : (long)(value - 0.5));
}

/* **A DEVICE PIXEL'S FOOTPRINT, AS TEXTURE COORDINATES, OR ZERO WHEN IT IS ONE TEXEL.** `quad_w` is the drawn width in
 * device pixels and `texture_w` the buffer's width in texels, so a device pixel is `1/quad_w` of the uv span and a
 * texel is `1/texture_w`. When the texture is larger the draw is a MINIFICATION and the four taps below sit at the
 * box's quarter points, `0.5/quad_w - 0.5/texture_w` from the centre: exactly zero at 1:1, exactly half a texel at
 * 2:1 (the four texel centres a box average wants), and an averaged sample at 1.5:1 — which is a 2x client on a
 * 96-dpi output and the case that used to drop every third texel. */
static void gl_box_for(float texture_w, float texture_h, float quad_w, float quad_h,
		       float *box_x, float *box_y)
{
	*box_x = (quad_w > 0.0f && texture_w > quad_w) ? (0.5f / quad_w) - (0.5f / texture_w) : 0.0f;
	*box_y = (quad_h > 0.0f && texture_h > quad_h) ? (0.5f / quad_h) - (0.5f / texture_h) : 0.0f;
}

static void gl_textured(struct gl_state *gl, struct gl_program *program, GLenum target, GLuint texture,
			float x, float y, float w, float h, float texture_w, float texture_h,
			float box_x, float box_y,
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
	glUniform2f(program->u_box, box_x, box_y);
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
 * from a uniform — the same division the CPU renderer makes between `aws_system_icon_coverage_at` and
 * `AWS_SYSTEM_MENU_GLYPH_COLOUR`.
 *
 * **AND IT IS UPLOADED AT THE SIZE IT IS DRAWN, WHICH IS WHAT MAKES IT CRISP.** The frame is drawn every frame and the
 * mark almost never changes, so the upload is keyed on (mark, size): a magnified 16×16 texture against a 21-pixel box
 * was the GL half of the mark being blurry while the bar's text was not, and the sampler's `GL_NEAREST` magnification
 * only made it blocky instead. With the grid at the box's own size the sampler is 1:1 and the texture is uploaded
 * once per scale rather than once per frame. */
static void upload_coverage(struct gl_state *gl, struct aws_system_icon *icon, int pixels)
{
	const uint8_t *coverage;

	if (icon == NULL || icon->paths == NULL || icon->path_count == 0) {
		icon = aws_system_icon_by_id(AWS_SYSTEM_MENU_ICON_FALLBACK);
	}
	if (gl->coverage_ready && gl->coverage_icon == icon && gl->coverage_pixels == pixels) {
		return;
	}
	coverage = aws_system_icon_coverage_at(icon, pixels);
	if (coverage == NULL) {
		return;
	}
	glBindTexture(GL_TEXTURE_2D, gl->coverage_texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, pixels, pixels, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, coverage);
	gl->coverage_icon = icon;
	gl->coverage_pixels = pixels;
	gl->coverage_ready = 1;
}

/* ---------------------------------------------------------------------------------------------------
 * the scene
 * --------------------------------------------------------------------------------------------------- */

/* **NOTHING BEHIND THE MENU BAR CASTS A SHADOW ONTO IT**, which is the CPU renderer's rule and now one scissor
 * rectangle instead of a per-row test: the shadow may only land in the rows BELOW `clip_top`, which is a row count in
 * DEVICE rows measured from the top of the screen — the scissor is device state, and `AWS_MENU_BAR_HEIGHT` is points.
 * Using the point count here left `round(24 * scale) - 24` rows of the bar (8 at 96 dpi) exposed, which is exactly
 * where a menu's top shadow painted a dark band under the strip.
 *
 * **AND THE SHEET IS THE SECOND USER OF THE SAME SENTENCE.** A sheet's top edge is its parent's title bar's bottom
 * edge, so its shadow must stop there too: the caller passes its own top edge when it has one, and what gets clipped
 * is a shadow ON a title bar that is supposed to be a seam rather than a ledge. */
/* **THE ONE SCISSOR THIS RENDERER EVER SETS, BUILT FROM THE TWO THINGS THAT LIMIT IT.** The frame's box is where the
 * renderer may paint at all; `clip_top` (top-down device rows) is the lowest row a SHADOW may use. Zero for `clip_top`
 * means "no row limit", which is also how the frame's box is restored after a shadow — so there is no separate
 * "disable" path to get wrong, and a caller cannot switch the frame's clip off by accident. An empty intersection is an
 * empty box rather than a disabled test: nothing may be painted, which is what it means. */
static void scissor_set(struct gl_state *gl, float clip_top)
{
	int x0 = 0, y0 = 0, x1 = gl->width, y1 = gl->height;

	if (gl->frame_box_active) {
		x0 = gl->frame_box.x;
		y0 = gl->height - (gl->frame_box.y + gl->frame_box.height);
		x1 = x0 + gl->frame_box.width;
		y1 = y0 + gl->frame_box.height;
	}
	{
		int below = gl->height - (int)(clip_top + 0.5f);

		if (below < 0) {
			below = 0;
		}
		if (below < y1) {
			y1 = below;
		}
	}
	if (x1 < x0) {
		x1 = x0;
	}
	if (y1 < y0) {
		y1 = y0;
	}
	glEnable(GL_SCISSOR_TEST);
	glScissor(x0, y0, x1 - x0, y1 - y0);
}

static void shadow_clip(struct gl_state *gl, int enable, float clip_top)
{
	scissor_set(gl, enable ? clip_top : 0.0f);
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
	/* **MINIFICATION IS LINEAR SO THE BOX TAPS CAN INTERPOLATE; MAGNIFICATION STAYS NEAREST.** See `gl_box_for`:
	 * the four taps of a minified draw have to land between texels, and a nearest sample would just repeat the same
	 * texel four times. At 1:1 the sample sits on a texel centre and linear returns it exactly, and a magnified
	 * surface — a 1x client on a 2x output, the System menu's mark — keeps its crisp double, which is what the
	 * screenshots assert. */
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
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

	{
		/* **A FRACTIONAL SCALE IS A REAL SCALE.** 1.5 device pixels per point is a 1080p panel at a comfortable
		 * size, and truncating it to 1 here would draw the bar two-thirds of its device height — the picture
		 * disagreeing with the framebuffer rather than merely being soft.
		 *
		 * **AND THE STRIP IS A WHOLE NUMBER OF DEVICE ROWS, BECAUSE ITS SHADOW IS MEASURED FROM ITS EDGE.** The
		 * shadow shader uses the rect's integer half-size, so a 31.97-row strip had its last row counted as outside
		 * itself: the bar's shadow painted over row 31 everywhere the application's menu window was not, and stopped
		 * dead at that window's edge — a 1px step along the bottom of the bar. `cpu_device_edge` already rounds the
		 * strip, so this is also the GL/CPU parity the two renderers promise. */
		float s = (float)aws_output_scale(server);
		float bar_h = gl_round((double)AWS_MENU_BAR_HEIGHT * (double)s);
		/* **AND THE MARK'S BOX IS ROUNDED THE SAME WAY, AND ITS GRID IS THE BOX'S SIZE.** The two numbers are the same
		 * arithmetic the CPU renderer applies, so a screenshot from either one puts the mark in the same pixels: a
		 * 16-point mark at 96 dpi is 21 device pixels at (13,5), not a 16-pixel image stretched to 21. */
		float glyph = gl_round((double)AWS_SYSTEM_MENU_GLYPH_WIDTH * (double)s);
		int grid = (glyph < (float)AWS_SYSTEM_ICON_MAX_PIXELS) ? (int)glyph
								      : AWS_SYSTEM_ICON_MAX_PIXELS;
		float glyph_left = gl_round((double)AWS_SYSTEM_MENU_LEFT_MARGIN * (double)s);
		float glyph_top = gl_round((double)AWS_SYSTEM_MENU_GLYPH_TOP * (double)s);

		gl_solid(gl, 0.0f, 0.0f, (float)gl->width, bar_h, AWS_MENU_BAR_COLOUR);
	upload_coverage(gl, server->system_menu_icon, grid);
	/* **THE MARK'S QUAD AND ITS TEXTURE ARE THE SAME SIZE, SO THE SAMPLER NEVER MAGNIFIES.** A mark is a 16-point
	 * design and a 21-pixel box needs a 21-pixel raster, which the icon keeps for that size; the uv divisor is the
	 * grid and not the box so a mark asked for at the ceiling is still mapped across its box. */
	gl_textured(gl, &gl->texture, GL_TEXTURE_2D, gl->coverage_texture,
		    glyph_left, glyph_top, glyph, glyph,
		    (float)grid, (float)grid,
		    0.0f, 0.0f,
		    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1, 0, AWS_SYSTEM_MENU_GLYPH_COLOUR);
	/* **AND THE BAR'S OWN SHADOW, WHICH FALLS BELOW THE STRIP.** Radius zero: the bar's corners are square, and a
	 * shadow with a radius under a square bar painted a smudge INSIDE its own corners. MEASURED in the CPU
	 * renderer, and this is the same number. */
	shadow_clip(gl, 1, bar_h);
	gl_shadow(gl, 0.0f, 0.0f, (float)gl->width, bar_h, 0.0f);
	}
	shadow_clip(gl, 0, 0.0f);
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
	float quad_x, quad_y, quad_w, quad_h;
	int from_shm = 0;
	int width, height, stride = 0;
	int buffer_width, buffer_height;
	int draw_w = 0, draw_h = 0;	/* a device-grid draw's extent, after the frame clip */
	int transparent;
	int in_bar;
	int hangs_from_bar;
	/* **AND A SHEET HANGS FROM ITS PARENT'S TITLE BAR, WHICH IS A SEAM THE GEOMETRY CANNOT NAME.** The bar below is a
	 * number both sides have; a sheet's parent is a fact only its application knows, so `set_sheet_parent` is what puts
	 * this on the surface. The two are one question at the corner — see `shape_coverage` — and this is the GL copy of
	 * that rule. */
	int squares_top;
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
	/* **THE QUAD IS THE BUFFER DIVIDED BY THE CLIENT'S SCALE, AND `gl_box_for` IS WHAT MAKES THE SAMPLER THE
	 * DOWNSCALE.** A 2x client's 1280x800 buffer is a 640x400 window; the texture is the whole 1280x800 and the quad
	 * is 640x400, so the four device pixels of every logical pixel are averaged into one — the same box the CPU
	 * renderer's `scaled_block_pixel` computes, which is what keeps `--renderer gl` and `--renderer cpu` from
	 * disagreeing at a fractional scale. See `aws_surface`'s `buffer_scale`. */
	buffer_width = width;
	buffer_height = height;
	{
		double bscale = surface->buffer_scale > 0.0 ? surface->buffer_scale : 1.0;

		if (bscale != 1.0) {
			/* ROUND, because the logical space is whole pixels and the buffer is what the client chose on the
			 * device grid: a 0.805x client's 18-pixel buffer still describes a 23-point window. */
			width = (int)((double)width / bscale + 0.5);
			height = (int)((double)height / bscale + 0.5);
		}
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
	/* **AND WHEN THE CLIENT DREW AT THE OUTPUT'S OWN SCALE, THE QUAD IS THE BUFFER — 1:1, NO SAMPLER.** This is the
	 * resolution-independent path: the client rasterised at `round(points * scale)` device pixels and declared that
	 * fraction (`agora_window.set_buffer_scale_fixed`), so the compositor's job is a copy, not a resample. The
	 * comparison is exact because both numbers were quantised to 1/256 (see `aws_scale_quantized`); the origin is
	 * rounded, which is the only rounding the placement needs and is what keeps adjacent frames tiling. */
	{
		double out_scale = aws_output_scale(server);
		double bscale = surface->buffer_scale > 0.0 ? surface->buffer_scale : 1.0;
		int device_grid = (bscale > 0.0) && (bscale == out_scale);

		if (device_grid) {
			/* **THE CLIPPED EXTENT IS THE BUFFER'S, UNLESS THE FRAME IS SMALLER THAN IT.** A status item's
			 * buffer is 320x200 while its frame is 64x24, and the frame is what the server was told the window
			 * is — so the drawn region is the clipped logical size on the device grid, and the texture is
			 * sampled 1:1 from its top-left corner. No clip and no resample in the ordinary case, because
			 * `round(frame * scale)` is exactly the buffer the client made. */
			int span_w = (int)gl_round((double)width * out_scale);
			int span_h = (int)gl_round((double)height * out_scale);

			if (span_w > buffer_width) {
				span_w = buffer_width;
			}
			if (span_h > buffer_height) {
				span_h = buffer_height;
			}
			draw_w = span_w;
			draw_h = span_h;
			quad_x = gl_round((double)surface->x * out_scale);
			quad_y = gl_round((double)surface->y * out_scale);
			quad_w = (float)span_w;
			quad_h = (float)span_h;
			uv_w = (float)span_w;
			uv_h = (float)span_h;
		} else {
			/* **AND THE QUAD IS PLACED IN DEVICE PIXELS.** `width`/`height` are logical here, so everything below
			 * multiplies by the output scale on the way to the shader — the corners, the radius and the rect. The uv
			 * divisor is the quad's own extent, so it is the SCALED one: the fraction the shader computes from it
			 * must still be 1.0. */
			uv_w = (float)width;
			uv_h = (float)height;
			quad_x = (float)surface->x * (float)out_scale;
			quad_y = (float)surface->y * (float)out_scale;
			quad_w = (float)width * (float)out_scale;
			quad_h = (float)height * (float)out_scale;
		}
		radius *= (float)out_scale;
	}

	/* **THE FOUR CORNER DECISIONS, WHICH ARE `shape_coverage`'S AND NOT A SECOND OPINION.** A corner is rounded
	 * only where the window is free of the output's own edges; a menu hanging from the bar keeps its top corners
	 * square; a surface entirely inside the bar is a piece of the bar and is square all round. The comparison is
	 * LOGICAL on both sides — `gl->width` is the framebuffer in device pixels, and comparing a logical window edge
	 * with it would round the corners of a window that reaches the screen edge on any scaled output. */
	in_bar = surface->y >= 0 && surface->y + surface->frame_height <= AWS_MENU_BAR_HEIGHT;	/* logical, both sides */
	hangs_from_bar = surface->y == AWS_MENU_BAR_HEIGHT;
	squares_top = hangs_from_bar || surface->sheet_parent != NULL;
	radius = (float)AWS_SHADOW_RADIUS;
	if (radius > (float)width / 2.0f) {
		radius = (float)width / 2.0f;
	}
	if (radius > (float)height / 2.0f) {
		radius = (float)height / 2.0f;
	}
	tl = (!in_bar && !squares_top && surface->x > 0 && surface->y > 0) ? 1.0f : 0.0f;
	tr = (!in_bar && !squares_top && surface->x + width < aws_output_width(server) && surface->y > 0) ? 1.0f : 0.0f;
	bl = (!in_bar && surface->x > 0 && surface->y + height < aws_output_height(server)) ? 1.0f : 0.0f;
	br = (!in_bar && surface->x + width < aws_output_width(server) &&
	      surface->y + height < aws_output_height(server)) ? 1.0f : 0.0f;

	/* THE SHADOW FIRST, because it is a shadow on the things behind. */
	if (!in_bar) {
		float s = (float)aws_output_scale(server);
		/* The bar's height in DEVICE rows, which is what the scissor is measured in — not the 24 points. **AND A
		 * SHEET'S SEAM IS THE SECOND FLOOR**, for the reason `shadow_clip` gives: the sheet's top edge is tucked
		 * `sheet_seam` points into its parent's title bar, so no shadow may be painted on the rows above the bottom of
		 * that tuck — they are the bar's. */
		float bar_h = gl_round((double)AWS_MENU_BAR_HEIGHT * (double)s);
		float clip_top = bar_h;

		if (surface->sheet_parent != NULL) {
			float seam = gl_round((double)(surface->y + surface->sheet_seam) * (double)s);

			if (seam > clip_top) {
				clip_top = seam;
			}
		}
		shadow_clip(gl, 1, clip_top);
		gl_shadow(gl, (float)surface->x * s, (float)surface->y * s, (float)surface->frame_width * s,
			  (float)surface->frame_height * s, (float)AWS_SHADOW_RADIUS * s);
		shadow_clip(gl, 0, 0.0f);
	}

	if (from_shm) {
		/* **THE UPLOAD IS THE VISIBLE REGION IN BUFFER PIXELS.** `width`/`height` are logical by now, so a scaled
		 * client's texture is its buffer's top-left `width x scale` by `height x scale` — which is the region the
		 * quad will be filled from, and the same clipping rule the CPU renderer applies when the frame is smaller
		 * than the buffer. A device-grid client uploaded its whole buffer, because its quad is that buffer. */
		double bscale = surface->buffer_scale > 0.0 ? surface->buffer_scale : 1.0;
		int device_grid = (bscale == aws_output_scale(server));
		int texture_w = device_grid ? draw_w : (int)((double)width * bscale + 0.5);
		int texture_h = device_grid ? draw_h : (int)((double)height * bscale + 0.5);
		float box_x, box_y;

		upload_client_buffer(gl, shm, texture_w, texture_h, stride);
		gl_box_for((float)texture_w, (float)texture_h, quad_w, quad_h, &box_x, &box_y);
		texture = gl->client_texture;
		gl_textured(gl, &gl->texture, GL_TEXTURE_2D, texture, quad_x, quad_y,
			    quad_w, quad_h, quad_w, quad_h, box_x, box_y, radius, tl, tr, bl, br, 0,
			    !transparent, 0);
	} else {
		struct gl_import *import = dmabuf->render;
		float box_x, box_y;

		/* THE dma-buf IS THE WHOLE BUFFER, so its own size is the texture's and the quad is the clipped frame:
		 * a buffer larger than its window is squeezed here exactly as the shm path squeezes it above. */
		gl_box_for((float)dmabuf->width, (float)dmabuf->height, quad_w, quad_h, &box_x, &box_y);
		gl_textured(gl, import->external ? &gl->texture_external : &gl->texture,
			    import->external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, texture,
			    quad_x, quad_y, quad_w, quad_h, quad_w, quad_h, box_x, box_y,
			    radius, tl, tr, bl, br, 0, !transparent, 0);
	}
}

static void gl_emit(struct aws_server *server, struct aws_surface *surface, uint32_t time,
		    const struct aws_box *box)
{
	if (surface == NULL) {
		gl_draw_menu_bar(server);
		return;
	}
	/* **THE SCISSOR CLIPS EVERY DRAW, SO AN EMIT OUTSIDE THE BOX COSTS ITS CALLBACK AND NOTHING ELSE.** Trap 1 of
	 * DAMAGE-PLAN.md §6 is the reason this is a return AFTER `aws_surface_frame_done`: a client whose callback is
	 * skipped along with its pixels never draws again. */
	if (box != NULL) {
		struct aws_box footprint;
		double scale = server->backend.scale > 0.0 ? server->backend.scale : 1.0;
		int depth = (int)(AWS_SHADOW_DEPTH * scale + 0.5);

		footprint.x = (int)((double)surface->x * scale + 0.5) - depth;
		footprint.y = (int)((double)surface->y * scale + 0.5) - depth;
		footprint.width = (int)((double)surface->frame_width * scale + 0.5) + depth * 2;
		footprint.height = (int)((double)surface->frame_height * scale + 0.5) + depth * 2;
		if (!aws_box_intersects(box, &footprint)) {
			aws_surface_frame_done(surface, time);
			return;
		}
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

/* **THE FRAME'S OWN TEXTURE AND THE FRAMEBUFFER THAT DRAWS INTO IT.** This is what makes a damage box mean anything on
 * a path whose swaps destroy the back buffer: the composition lives here and survives every swap, so a box paints the box
 * and leaves the rest of the texture exactly as the last frame left it. GLES2 requires framebuffer objects, so this
 * cannot fail on a conforming driver; a driver that does fail it gets a message and the CPU renderer rather than a
 * picture with holes in it. Re-made when the output changes size, which nothing does today. */
static int gl_frame_target_create(struct gl_state *gl)
{
	GLenum status;

	if (gl->frame_texture != 0) {
		glDeleteTextures(1, &gl->frame_texture);
		gl->frame_texture = 0;
	}
	if (gl->frame_fbo != 0) {
		glDeleteFramebuffers(1, &gl->frame_fbo);
		gl->frame_fbo = 0;
	}
	glGenTextures(1, &gl->frame_texture);
	glBindTexture(GL_TEXTURE_2D, gl->frame_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gl->width, gl->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	/* **NEAREST, BECAUSE THE COPY IS 1:1 AND A FILTER WOULD SOFTEN TEXT THAT THE CLIENT SENT SHARP.** */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenFramebuffers(1, &gl->frame_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, gl->frame_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl->frame_texture, 0);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	gl->width = gl->width > 0 ? gl->width : 1;
	gl->height = gl->height > 0 ? gl->height : 1;
	return status == GL_FRAMEBUFFER_COMPLETE ? 0 : -1;
}

/* **AND THE ONE QUAD THAT PUTS THE FRAME ON THE GLASS.** Full viewport, no blending, no scissor: the back buffer may
 * contain anything at all after a swap, and the frame texture is the only complete picture. */
static void gl_blit_frame(struct gl_state *gl)
{
	float v[8];

	quad_vertices(v, 0.0f, 0.0f, (float)gl->width, (float)gl->height);
	glUseProgram(gl->blit.id);
	glUniform2f(gl->blit.u_size, (float)gl->width, (float)gl->height);
	glUniform4f(gl->blit.u_rect, 0.0f, 0.0f, (float)gl->width, (float)gl->height);
	glUniform2f(gl->blit.u_uv_div, (float)gl->width, (float)gl->height);
	glUniform1i(gl->blit.u_tex, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, gl->frame_texture);
	glEnableVertexAttribArray((GLuint)gl->blit.a_pos);
	glVertexAttribPointer((GLuint)gl->blit.a_pos, 2, GL_FLOAT, GL_FALSE, 0, v);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* **ONE RECTANGLE OF A FRAME, AS A WHOLE PASS INTO THE FRAME TEXTURE.** GL's origin is the bottom left while the box's
 * is the top left: the conversion is here, once, and nowhere else — `y_gl = height - (box.y + box.height)`, the row the
 * box's LAST device row starts at. `box` NULL is the whole output. */
static void gl_draw_rect(struct aws_server *server, const struct aws_box *box)
{
	struct gl_state *gl = server->renderer->data;
	float r, g, b;

	rgb_of(AWS_BACKGROUND, &r, &g, &b);
	glViewport(0, 0, gl->width, gl->height);
	if (box != NULL) {
		gl->frame_box = *box;
		gl->frame_box_active = 1;
	} else {
		gl->frame_box_active = 0;
	}
	scissor_set(gl, 0.0f);
	glClearColor(r, g, b, 1.0f);
	/* **THE CLEAR HONOURS THE SCISSOR, WHICH IS WHY THERE IS NO SECOND PATH FOR IT.** `glClear` is clipped by the
	 * scissor test, so "clear the background inside the box" is the same call as "clear the whole output" with a box
	 * around it — one expression that cannot drift from the walk it belongs to. */
	glClear(GL_COLOR_BUFFER_BIT);
	aws_output_walk(server, gl_emit, box);
	/* **THE POINTER IS OVER EVERYTHING** — the same last act as the CPU renderer's, with the placement and the
	 * clipping done by `aws_cursor_image`'s own arithmetic rather than by a second copy of it here. */
	{
		const struct aws_cursor *cursor = aws_cursor_image(server);

		if (cursor != NULL) {
			float x = gl_round((double)server->pointer_x * server->backend.scale) -
				(float)cursor->hotspot_x;
			float y = gl_round((double)server->pointer_y * server->backend.scale) -
				(float)cursor->hotspot_y;

			if (!gl->cursor_uploaded || gl->cursor_generation != cursor->generation) {
				glBindTexture(GL_TEXTURE_2D, gl->cursor_texture);
				glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cursor->width, cursor->height, 0, GL_RGBA,
					     GL_UNSIGNED_BYTE, cursor->pixels);
				gl->cursor_generation = cursor->generation;
				gl->cursor_uploaded = 1;
			}
			gl_textured(gl, &gl->cursor, GL_TEXTURE_2D, gl->cursor_texture, x, y,
				    (float)cursor->width, (float)cursor->height,
				    (float)cursor->width, (float)cursor->height,
				    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 0);
		}
	}
}

/* **AND THE FRAME: EVERY RECTANGLE PAINTED INTO THE TEXTURE, THEN ONE COPY TO THE GLASS.** A region's rectangles are
 * disjoint, so each is its own pass of the same scene — the picture is identical in every pass, and the gap between two
 * strips is never touched. The copy is full-viewport because the back buffer's previous contents are the swap's business
 * and not something this renderer may assume; the texture is where the frame lives. */
static void gl_draw(struct aws_server *server, const pixman_region32_t *damage)
{
	struct gl_state *gl = server->renderer->data;
	pixman_box32_t *rects;
	int count = 0;
	int i;

	/* **AN OUTPUT THAT CHANGED SIZE RE-MAKES THE TEXTURE.** Nothing in this server resizes an output today (see the X11
	 * backend's ConfigureNotify), but a frame texture that kept the old size would be a frame that is half a picture on
	 * the day something does. */
	if (gl->width != server->backend.width || gl->height != server->backend.height) {
		if (gl_frame_target_create(gl) != 0) {
			return;
		}
	}
	glBindFramebuffer(GL_FRAMEBUFFER, gl->frame_fbo);
	glViewport(0, 0, gl->width, gl->height);
	if (damage == NULL) {
		gl_draw_rect(server, NULL);
	} else {
		/* pixman's query is not const-correct: it hands back the region's own rectangles, and this is the one place
		 * the renderer needs them. */
		rects = pixman_region32_rectangles((pixman_region32_t *)damage, &count);
		for (i = 0; i < count; i++) {
			struct aws_box box;

			box.x = rects[i].x1;
			box.y = rects[i].y1;
			box.width = rects[i].x2 - rects[i].x1;
			box.height = rects[i].y2 - rects[i].y1;
			gl_draw_rect(server, &box);
		}
	}
	/* **THE SCISSOR GOES OFF BEFORE THE COPY AND BEFORE ANY SWAP**, or the copy itself would be clipped to the last
	 * rectangle and the rest of the screen would keep whatever the swap left there. */
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, gl->width, gl->height);
	/* **AND THE COPY NEEDS NO STATE OF ITS OWN, WHICH IS A LESSON THIS PAID FOR.** It used to switch blending off and
	 * leave it off — and blending is enabled once, at startup, so from the second frame every surface was drawn OPAQUE:
	 * the desktop's wallpaper vanished under the transparent windows drawn over it and the menu bar went black except
	 * where an application had painted its title. The blit writes alpha 1, and `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` with
	 * alpha 1 is an exact copy, so the state is left exactly as the scene left it. */
	gl_blit_frame(gl);
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

	if (gl == NULL || gl->surface == EGL_NO_SURFACE) {
		return;
	}
	if (!eglSwapBuffers(gl->display, gl->surface)) {
		/* Nothing was handed to the surface, so there is no front buffer to lock; the frame is dropped and the
		 * next render pass tries again from the same place. */
		return;
	}
#ifdef AWS_HAVE_GBM
	/* **THE SWAP IS NOT THE PRESENT ON A SCANNER.** EGL has drawn into the next GBM buffer; locking the front one
	 * and naming it to the KMS plane is what puts it on the glass. The backend owns the flip and therefore the
	 * buffer's lifetime — it releases it when the flip that replaced it completes — so the only failure this side
	 * has to handle is the backend refusing it, and then the buffer comes straight back to the surface. */
	if (gl->gbm && gl->gbm_surface != NULL && server->backend.gbm_present != NULL) {
		struct gbm_bo *bo = gbm_surface_lock_front_buffer(gl->gbm_surface);

		if (bo != NULL && server->backend.gbm_present(server, bo) != 0) {
			gbm_surface_release_buffer(gl->gbm_surface, bo);
		}
	}
#endif
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
#ifdef AWS_HAVE_GBM
		/* **AND THE GBM SURFACE GOES BEFORE THE DEVICE THAT MADE IT.** The backend destroys the device, and the
		 * loan is withdrawn first so a flip event racing the teardown cannot release into a freed surface. */
		if (gl->gbm_surface != NULL) {
			server->backend.gbm_surface = NULL;
			gbm_surface_destroy(gl->gbm_surface);
			gl->gbm_surface = NULL;
		}
#endif
		if (gl->context != EGL_NO_CONTEXT) {
			eglDestroyContext(gl->display, gl->context);
		}
		eglTerminate(gl->display);
	}
	free(gl->scratch);
	/* **AND THE FRAME'S TEXTURE AND FBO GO BEFORE THE CONTEXT DOES.** They are GL objects like any other: deleting them
	 * after `eglDestroyContext` would be deleting names in a context that no longer exists. */
	if (gl->frame_texture != 0 && gl->display != EGL_NO_DISPLAY && gl->context != EGL_NO_CONTEXT) {
		glDeleteTextures(1, &gl->frame_texture);
		glDeleteFramebuffers(1, &gl->frame_fbo);
	}
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

#ifdef AWS_HAVE_GBM
/* **THE GBM CONFIG IS CHOSEN BY ITS PIXEL FORMAT, NOT BY A VISUAL ID.** There is no window, so the thing that has to
 * agree between EGL and the surface is `EGL_NATIVE_VISUAL_ID` — which on the GBM platform is a DRM fourcc — and the
 * scanout format this compositor knows how to scan: `XRGB8888`, the same format every client sends and the one
 * `drm_set_plane` is handed. A config whose format differs would build a surface the plane cannot sample. */
static int choose_gbm_config(struct gl_state *gl)
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
		EGLint native = 0, renderable = 0, surface = 0, caveat = 0;

		eglGetConfigAttrib(gl->display, configs[index], EGL_NATIVE_VISUAL_ID, &native);
		eglGetConfigAttrib(gl->display, configs[index], EGL_RENDERABLE_TYPE, &renderable);
		eglGetConfigAttrib(gl->display, configs[index], EGL_SURFACE_TYPE, &surface);
		eglGetConfigAttrib(gl->display, configs[index], EGL_CONFIG_CAVEAT, &caveat);
		if ((uint32_t)native == (uint32_t)GBM_FORMAT_XRGB8888 && (surface & EGL_WINDOW_BIT) &&
		    (renderable & EGL_OPENGL_ES2_BIT) && caveat != EGL_SLOW_CONFIG) {
			gl->config = configs[index];
			gl->gbm_format = GBM_FORMAT_XRGB8888;
			free(configs);
			return 0;
		}
	}
	free(configs);
	return -1;
}
#endif /* AWS_HAVE_GBM */

static int make_texture(struct gl_state *gl, GLuint *texture)
{
	glGenTextures(1, texture);
	if (*texture == 0) {
		return -1;
	}
	glBindTexture(GL_TEXTURE_2D, *texture);
	/* MINIFICATION IS LINEAR FOR THE BOX FILTER, MAGNIFICATION NEAREST FOR AN EXACT 1:1 AND A CRISP DOUBLE — the
	 * same pair the dma-buf import uses, so both buffer paths sample identically. */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
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
	int use_gbm = 0;

#ifdef AWS_HAVE_GBM
	use_gbm = server->backend.gbm_device != NULL;
#endif
	if (!use_gbm && (server->backend.native_display == NULL || server->backend.native_window == 0)) {
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
#ifdef AWS_HAVE_GBM
	gl->gbm = use_gbm;
	gl->gbm_device = server->backend.gbm_device;
#endif
	if (use_gbm) {
#ifdef AWS_HAVE_GBM
		/* **THE DISPLAY IS THE CARD, NOT A WINDOW SYSTEM.** `EGL_PLATFORM_GBM_KHR` is the platform a KMS
		 * compositor uses; the EXT entry point is how EGL 1.4 exposed it, and the core one is the 1.5 spelling.
		 * A driver with neither cannot host this renderer, and saying so lets `auto` fall to the CPU. */
		PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform =
			(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

		if (get_platform != NULL) {
			gl->display = get_platform(EGL_PLATFORM_GBM_KHR, gl->gbm_device, NULL);
		} else {
			gl->display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gl->gbm_device, NULL);
		}
		if (gl->display == EGL_NO_DISPLAY) {
			fprintf(stderr, "window-server: gl: eglGetPlatformDisplay(GBM) failed (0x%x)\n",
				(unsigned)eglGetError());
			goto fail;
		}
#endif
	} else {
		gl->display = eglGetDisplay((EGLNativeDisplayType)server->backend.native_display);
		if (gl->display == EGL_NO_DISPLAY) {
			fprintf(stderr, "window-server: gl: eglGetDisplay failed (0x%x)\n",
				(unsigned)eglGetError());
			goto fail;
		}
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
	if (use_gbm) {
#ifdef AWS_HAVE_GBM
		if (choose_gbm_config(gl) != 0) {
			fprintf(stderr, "window-server: gl: no XRGB8888 EGL config for a GBM scanout on %s\n",
				gbm_device_get_backend_name(gl->gbm_device));
			goto fail;
		}
		if (!gbm_device_is_format_supported(gl->gbm_device, gl->gbm_format,
						    GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING)) {
			fprintf(stderr,
				"window-server: gl: %s cannot make a scanout XRGB8888 buffer through GBM; the CPU renderer will be used\n",
				gbm_device_get_backend_name(gl->gbm_device));
			goto fail;
		}
		gl->gbm_surface = gbm_surface_create(gl->gbm_device, (uint32_t)gl->width, (uint32_t)gl->height,
						     gl->gbm_format,
						     GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
		if (gl->gbm_surface == NULL) {
			fprintf(stderr, "window-server: gl: gbm_surface_create(%dx%d) failed on %s: %s\n",
				gl->width, gl->height, gbm_device_get_backend_name(gl->gbm_device),
				strerror(errno));
			goto fail;
		}
		/* THE LOAN THAT LETS THE BACKEND RELEASE A BUFFER AFTER A FLIP. Cleared before the surface is destroyed. */
		server->backend.gbm_surface = gl->gbm_surface;
#endif
	} else if (choose_config(gl, server->backend.native_visual) != 0) {
		fprintf(stderr, "window-server: gl: no EGL config matches the window's visual (0x%lx)\n",
			server->backend.native_visual);
		goto fail;
	}
	gl->context = eglCreateContext(gl->display, gl->config, EGL_NO_CONTEXT, context_attributes);
	if (gl->context == EGL_NO_CONTEXT) {
		fprintf(stderr, "window-server: gl: eglCreateContext failed (0x%x)\n", (unsigned)eglGetError());
		goto fail;
	}
	if (use_gbm) {
		PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_platform_window =
			(PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");

		if (create_platform_window != NULL) {
			gl->surface = create_platform_window(gl->display, gl->config,
							     (void *)gl->gbm_surface, NULL);
		} else {
			gl->surface = eglCreateWindowSurface(gl->display, gl->config,
							     (EGLNativeWindowType)gl->gbm_surface, NULL);
		}
	} else {
		gl->surface = eglCreateWindowSurface(gl->display, gl->config,
						     (EGLNativeWindowType)server->backend.native_window, NULL);
	}
	if (gl->surface == EGL_NO_SURFACE) {
		fprintf(stderr, "window-server: gl: eglCreateWindowSurface failed (0x%x)\n",
			(unsigned)eglGetError());
		goto fail;
	}
	if (!eglMakeCurrent(gl->display, gl->surface, gl->surface, gl->context)) {
		fprintf(stderr, "window-server: gl: eglMakeCurrent failed (0x%x)\n", (unsigned)eglGetError());
		goto fail;
	}
	/* **THE SWAP INTERVAL IS THE WINDOW'S PACING, NOT THE SCANNER'S.** On a GBM surface the throttle is the page
	 * flip (`drm_busy`), and asking EGL to wait for a vblank as well would spend two vblanks per frame on a driver
	 * that implements the wait. The X11 surface has no flip event of ours to pace it, so it keeps interval 1. */
	eglSwapInterval(gl->display, use_gbm ? 0 : 1);
	/* **AND THE FRAME'S OWN TEXTURE, WHICH IS WHAT MAKES THE DAMAGE BOX MEANINGFUL ON THIS PATH.** A swap may destroy
	 * the back buffer — EGL's default for a window, and every GBM scanout buffer by construction — so painting only the
	 * damaged box into it would leave holes wherever the previous frame was. The scene is composed into a texture that
	 * survives every swap, and one full-viewport quad copies it out. GLES2 requires framebuffer objects, so a driver
	 * that cannot make one cannot run this renderer at all, and saying so here is better than a partial picture later. */
	if (gl_frame_target_create(gl) != 0) {
		fprintf(stderr, "window-server: gl: no framebuffer object for the frame texture\n");
		goto fail;
	}
	(void)use_gbm;

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
	    link_program(&gl->shadow, shadow_source) != 0 ||
	    link_program(&gl->blit, blit_source) != 0 ||
	    link_program(&gl->cursor, cursor_source) != 0) {
		goto fail;
	}
	if (gl->has_external && link_program(&gl->texture_external, texture_external_source) != 0) {
		goto fail;
	}
	if (make_texture(gl, &gl->client_texture) != 0 || make_texture(gl, &gl->coverage_texture) != 0 ||
	    make_texture(gl, &gl->cursor_texture) != 0) {
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
#ifdef AWS_HAVE_GBM
		if (gl->gbm_surface != NULL) {
			server->backend.gbm_surface = NULL;
			gbm_surface_destroy(gl->gbm_surface);
			gl->gbm_surface = NULL;
		}
#endif
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
