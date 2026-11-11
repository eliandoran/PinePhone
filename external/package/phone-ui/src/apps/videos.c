/*
 * Videos: the files in /var/lib/videos (made at startup and shared with a computer over
 * MTP, folders.c), by name, and a player. The picture is decoded by system/video.c
 * (GStreamer, the video engine) into NV12 frames, whose planes the GPU reads as they
 * are (dma-bufs imported as textures; uploaded where that fails) and a shader turns
 * into RGB, in a texture of the size shown, which an lv_3dtexture draws (as camera.c's
 * preview). The player turns with the phone, full screen sideways (its controls over
 * the picture, shown by a tap); the screen stays on while it plays; covered (locked, a
 * call), it pauses.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "i18n.h"
#include "system/video.h"
#include "ui.h"

#if LV_USE_3DTEXTURE
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/eglext.h>
#endif

#define VIDEOS "/var/lib/videos"
#define ROW_H 112
#define TICK_MS 10
#define CONTROLS_H 128
#define HIDE_MS 3000 /* sideways, the controls after a tap while it plays */
#define COLOR_ICON 0xff9f0a
/* DRM's formats of the planes imported (drm_fourcc.h, not in every build) */
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FORMAT_R8 FOURCC('R', '8', ' ', ' ')
#define FORMAT_GR88 FOURCC('G', 'R', '8', '8')

static const char *const extensions[] = { "mp4", "m4v", "mov", "mkv", "webm", "avi" }; /* NON-NLS */

struct entry {
	char *name; /* the file's */
	unsigned long long size;
};

static struct {
	lv_obj_t *scr, *list;
	struct entry *entries;
	unsigned count;
} lib;

static struct {
	lv_obj_t *scr, *header, *frame, *picture, *status;
	lv_obj_t *controls, *back, *play, *slider, *elapsed, *total;
	lv_timer_t *timer;
	struct video *video;
	enum video_state state;
	int width, height;   /* the video's, 0 while unknown */
	uint32_t second;     /* shown in elapsed, UINT32_MAX: none yet */
	uint32_t duration;   /* ms, as the slider's range was set */
	uint32_t touched;    /* lv_tick when the controls were last shown sideways */
	bool sideways, laid_out;
#if LV_USE_3DTEXTURE
	GLuint program, y_tex, uv_tex, fbo;
	unsigned out;        /* the picture's texture (ui_texture_create()) */
	int out_w, out_h;
	int tex_w, tex_h;    /* the planes' textures uploaded: the frame's stride and height */
	bool dmabuf;         /* the planes imported (dma-bufs): false once that failed */
#endif
} pl;

/* The list */

static bool is_video(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot || dot == name)
		return false;
	for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++)
		if (strcasecmp(dot + 1, extensions[i]) == 0)
			return true;
	return false;
}

static int entry_cmp(const void *a, const void *b)
{
	return strcasecmp(((const struct entry *)a)->name, ((const struct entry *)b)->name);
}

static void library_free(void)
{
	for (unsigned i = 0; i < lib.count; i++)
		free(lib.entries[i].name);
	free(lib.entries);
	lib.entries = NULL;
	lib.count = 0;
}

static void library_load(void)
{
	library_free();
	DIR *d = opendir(VIDEOS);
	if (!d)
		return;
	unsigned cap = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.' || !is_video(de->d_name))
			continue;
		char path[512];
		struct stat st;
		snprintf(path, sizeof(path), VIDEOS "/%s", de->d_name);
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		if (lib.count == cap) {
			cap = cap ? cap * 2 : 16;
			lib.entries = realloc(lib.entries, cap * sizeof(*lib.entries));
		}
		lib.entries[lib.count++] = (struct entry){ strdup(de->d_name), (unsigned long long)st.st_size };
	}
	closedir(d);
	if (lib.count)
		qsort(lib.entries, lib.count, sizeof(*lib.entries), entry_cmp);
}

/* The name without its extension */
static const char *title_of(const char *name)
{
	char t[256];
	snprintf(t, sizeof(t), "%s", name);
	char *dot = strrchr(t, '.');
	if (dot && dot != t)
		*dot = '\0';
	return ui_intern(t);
}

/* The player */

static void time_format(char *buf, size_t size, uint32_t s)
{
	if (s >= 3600)
		snprintf(buf, size, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(buf, size, "%u:%02u", s / 60, s % 60);
}

static void time_set(lv_obj_t *label, uint32_t s)
{
	char t[16];
	time_format(t, sizeof(t), s);
	ui_set_text(label, t);
}

#if LV_USE_3DTEXTURE

/*
 * NV12 to RGB, drawn over the picture's texture. The planes are the decoder's buffers
 * imported as textures (dma-bufs, no copy: R8 and GR88, U in red and V in green), or
 * else uploaded (LUMINANCE and LUMINANCE_ALPHA, V in alpha) as wide as the frame's rows
 * (GLES 2 cannot skip the decoder's padding), u_crop the part shown; u_u and u_v pick
 * the chroma plane's channels. Coordinates come from the vertex shader at full
 * precision, as camera.c's (the Mali-400 computes fragments in 16-bit floats). The
 * output is in LVGL's texture byte order (B, G, R, A).
 */
static const char *vertex_source =
	"attribute vec2 a_pos;\n"
	"uniform vec2 u_crop;\n"
	"varying vec2 v_tc;\n"
	"void main()\n"
	"{\n"
	"    v_tc = (a_pos * 0.5 + 0.5) * u_crop;\n"
	"    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

static const char *fragment_source =
	"precision mediump float;\n"
	"uniform sampler2D u_y;\n"
	"uniform sampler2D u_uv;\n"
	"uniform mat3 u_matrix;\n"
	"uniform vec3 u_offset;\n"
	"uniform vec4 u_u;\n"
	"uniform vec4 u_v;\n"
	"varying vec2 v_tc;\n"
	"void main()\n"
	"{\n"
	"    vec4 c = texture2D(u_uv, v_tc);\n"
	"    vec3 yuv = vec3(texture2D(u_y, v_tc).r, dot(c, u_u), dot(c, u_v)) - u_offset;\n"
	"    gl_FragColor = vec4(clamp((u_matrix * yuv).bgr, 0.0, 1.0), 1.0);\n"
	"}\n";

/* GLES 2 has vertex array objects as an extension (LVGL uses them) */
static PFNGLBINDVERTEXARRAYOESPROC bind_vertex_array;
/* dma-buf import (EGL_EXT_image_dma_buf_import) */
static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture;

static GLuint shader(GLenum type, const char *source)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	glShaderSource(s, 1, &source, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512] = "";
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		printf("videos: shader: %s\n", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static GLuint plane_texture(void)
{
	GLuint t;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);
	return t;
}

static bool gl_init(void)
{
	bind_vertex_array = (PFNGLBINDVERTEXARRAYOESPROC)eglGetProcAddress("glBindVertexArrayOES");
	if (!bind_vertex_array)
		return false;
	GLuint vs = shader(GL_VERTEX_SHADER, vertex_source);
	GLuint fs = shader(GL_FRAGMENT_SHADER, fragment_source);
	GLint ok = 0;
	if (vs && fs) {
		pl.program = glCreateProgram();
		glAttachShader(pl.program, vs);
		glAttachShader(pl.program, fs);
		glBindAttribLocation(pl.program, 0, "a_pos");
		glLinkProgram(pl.program);
		glGetProgramiv(pl.program, GL_LINK_STATUS, &ok);
	}
	if (vs)
		glDeleteShader(vs);
	if (fs)
		glDeleteShader(fs);
	if (!ok) {
		if (pl.program)
			glDeleteProgram(pl.program);
		pl.program = 0;
		return false;
	}
	pl.y_tex = plane_texture();
	pl.uv_tex = plane_texture();
	glGenFramebuffers(1, &pl.fbo);
	EGLDisplay dpy = eglGetCurrentDisplay();
	const char *ext = dpy != EGL_NO_DISPLAY ? eglQueryString(dpy, EGL_EXTENSIONS) : NULL;
	create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	image_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	pl.dmabuf = ext && strstr(ext, "EGL_EXT_image_dma_buf_import") && create_image && destroy_image && image_texture;
	return true;
}

static void gl_deinit(void)
{
	if (pl.fbo)
		glDeleteFramebuffers(1, &pl.fbo);
	if (pl.y_tex)
		glDeleteTextures(1, &pl.y_tex);
	if (pl.uv_tex)
		glDeleteTextures(1, &pl.uv_tex);
	if (pl.program)
		glDeleteProgram(pl.program);
	ui_texture_delete(pl.out);
}

/* A plane of a dma-buf as the texture's content: false if the driver refuses it */
static bool plane_import(GLuint tex, int fd, size_t offset, int stride, int w, int h, uint32_t fourcc)
{
	EGLDisplay dpy = eglGetCurrentDisplay();
	const EGLint attrs[] = {
		EGL_WIDTH, w, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fourcc,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, stride, EGL_NONE,
	};
	EGLImageKHR image = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
	if (image == EGL_NO_IMAGE_KHR)
		return false;
	glBindTexture(GL_TEXTURE_2D, tex);
	image_texture(GL_TEXTURE_2D, image);
	bool ok = glGetError() == GL_NO_ERROR;
	destroy_image(dpy, image); /* the texture keeps the buffer */
	return ok;
}

static void plane_upload(GLuint tex, GLenum format, int w, int h, const uint8_t *px, bool resized)
{
	glBindTexture(GL_TEXTURE_2D, tex);
	if (resized)
		glTexImage2D(GL_TEXTURE_2D, 0, (GLint)format, w, h, 0, format, GL_UNSIGNED_BYTE, px);
	else
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, format, GL_UNSIGNED_BYTE, px);
}

/*
 * The frame into the picture's texture. Runs in LVGL's thread, between frames, in its
 * GL context: keeps what LVGL relies on across draws (framebuffer, viewport, program,
 * array buffer), draws with no vertex array object bound so that LVGL's are not changed.
 */
static void gl_convert(const struct video_frame *f)
{
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	/* YUV to RGB: columns for Y, U, V (GLSL's order) */
	static const float bt601[9] = { 1, 1, 1, 0, -0.344136f, 1.772f, 1.402f, -0.714136f, 0 };
	static const float bt709[9] = { 1, 1, 1, 0, -0.187324f, 1.8556f, 1.5748f, -0.468124f, 0 };
	if (!pl.out || !pl.program)
		return;

	GLint fbo, program, buffer, viewport[4], active, align;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
	glGetIntegerv(GL_VIEWPORT, viewport);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);

	bool imported = false;
	if (pl.dmabuf && f->fd[0] >= 0 && f->fd[1] >= 0) {
		glGetError(); /* what came before */
		glActiveTexture(GL_TEXTURE1);
		imported = plane_import(pl.uv_tex, f->fd[1], f->offset[1], f->stride, (f->width + 1) / 2,
		                        (f->height + 1) / 2, FORMAT_GR88);
		glActiveTexture(GL_TEXTURE0);
		imported = imported && plane_import(pl.y_tex, f->fd[0], f->offset[0], f->stride, f->width,
		                                     f->height, FORMAT_R8);
		if (!imported) {
			printf("videos: the decoder's buffers cannot be imported, uploaded instead\n");
			pl.dmabuf = false;
		}
		pl.tex_w = pl.tex_h = 0; /* the textures are the images now: uploads define them again */
	}
	if (!imported) {
		bool resized = f->stride != pl.tex_w || f->height != pl.tex_h;
		pl.tex_w = f->stride;
		pl.tex_h = f->height;
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glActiveTexture(GL_TEXTURE1);
		plane_upload(pl.uv_tex, GL_LUMINANCE_ALPHA, f->stride / 2, (f->height + 1) / 2, f->uv, resized);
		glActiveTexture(GL_TEXTURE0);
		plane_upload(pl.y_tex, GL_LUMINANCE, f->stride, f->height, f->y, resized);
	}

	float matrix[9];
	memcpy(matrix, f->bt709 ? bt709 : bt601, sizeof(matrix));
	float scale = f->full_range ? 1.0f : 255.0f / 219.0f, chroma = f->full_range ? 1.0f : 255.0f / 224.0f;
	for (int i = 0; i < 9; i++)
		matrix[i] *= i < 3 ? scale : chroma;

	glBindFramebuffer(GL_FRAMEBUFFER, pl.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pl.out, 0);
	glViewport(0, 0, pl.out_w, pl.out_h);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glUseProgram(pl.program);
	glUniform1i(glGetUniformLocation(pl.program, "u_y"), 0);
	glUniform1i(glGetUniformLocation(pl.program, "u_uv"), 1);
	glUniformMatrix3fv(glGetUniformLocation(pl.program, "u_matrix"), 1, GL_FALSE, matrix);
	glUniform3f(glGetUniformLocation(pl.program, "u_offset"), f->full_range ? 0.0f : 16.0f / 255,
	            128.0f / 255, 128.0f / 255);
	glUniform2f(glGetUniformLocation(pl.program, "u_crop"), imported ? 1.0f : (float)f->width / (float)f->stride, 1.0f);
	glUniform4f(glGetUniformLocation(pl.program, "u_u"), 1, 0, 0, 0);
	glUniform4f(glGetUniformLocation(pl.program, "u_v"), 0, imported ? 1.0f : 0.0f, 0, imported ? 0.0f : 1.0f);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	bind_vertex_array(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);

	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture((GLenum)active);
	glPixelStorei(GL_UNPACK_ALIGNMENT, align);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	glUseProgram((GLuint)program);
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)buffer);
}

#endif

/* The picture as large as the frame lets it, keeping its shape; its texture that size */
static void picture_layout(void)
{
	if (!pl.picture || !pl.width || !pl.height)
		return;
	lv_obj_update_layout(pl.scr);
	int32_t fw = lv_obj_get_content_width(pl.frame), fh = lv_obj_get_content_height(pl.frame);
	if (fw <= 0 || fh <= 0)
		return;
	int32_t w = fw, h = (int32_t)((int64_t)fw * pl.height / pl.width);
	if (h > fh) {
		h = fh;
		w = (int32_t)((int64_t)fh * pl.width / pl.height);
	}
	w = LV_MAX(w, 2);
	h = LV_MAX(h, 2);
	lv_obj_set_size(pl.picture, w, h);
	lv_obj_center(pl.picture);
#if LV_USE_3DTEXTURE
	if (w != pl.out_w || h != pl.out_h) {
		ui_texture_delete(pl.out);
		pl.out = ui_texture_create(NULL, w, h);
		pl.out_w = w;
		pl.out_h = h;
		lv_3dtexture_set_src(pl.picture, pl.out);
		pl.tex_w = pl.tex_h = 0; /* uploaded again into the new one */
	}
#endif
	lv_obj_set_hidden(pl.picture, false);
}

static void controls_show(bool show)
{
	if (!pl.sideways)
		show = true;
	lv_obj_set_hidden(pl.controls, !show);
	pl.touched = lv_tick_get();
}

/* Upright: the header, the picture, the controls under it. Sideways (full screen):
 * the picture alone, the controls over its bottom with a way back. */
static void layout(void)
{
	bool sideways = ui_screen_fullscreen(pl.scr) && ui_sideways();
	if (sideways != pl.sideways || !pl.laid_out) {
		pl.laid_out = true;
		pl.sideways = sideways;
		if (pl.header)
			lv_obj_set_hidden(pl.header, sideways);
		/* The room left for the status bar: none sideways, where it is hidden (nav.c) */
		lv_obj_set_style_pad_top(lv_obj_get_parent(lv_obj_get_parent(pl.frame)), sideways ? 0 : UI_STATUS_H, 0);
		lv_obj_set_hidden(pl.back, !sideways);
		if (sideways) {
			lv_obj_set_floating(pl.controls, true);
			lv_obj_align(pl.controls, LV_ALIGN_BOTTOM_MID, 0, 0);
			lv_obj_set_style_bg_opa(pl.controls, LV_OPA_60, 0);
		} else {
			lv_obj_set_floating(pl.controls, false);
			lv_obj_set_style_bg_opa(pl.controls, LV_OPA_COVER, 0);
		}
		controls_show(true);
	}
	picture_layout();
}

static void state_show(void)
{
	bool playing = pl.state == VIDEO_PLAYING || pl.state == VIDEO_LOADING;
	lv_label_set_text_static(lv_obj_get_child(pl.play, 0), playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void position_show(void)
{
	uint32_t duration = video_duration(pl.video);
	if (duration != pl.duration) {
		pl.duration = duration;
		lv_slider_set_range(pl.slider, 0, (int32_t)LV_MAX(duration / 1000, 1));
		time_set(pl.total, duration / 1000);
	}
	if (lv_obj_has_state(pl.slider, LV_STATE_PRESSED))
		return; /* the labels follow the finger */
	uint32_t s = video_position(pl.video) / 1000;
	if (s != pl.second) {
		pl.second = s;
		time_set(pl.elapsed, s);
		lv_slider_set_value(pl.slider, (int32_t)s, LV_ANIM_OFF);
	}
}

static void failed(void)
{
	printf("videos: %s\n", video_error(pl.video));
	lv_label_set_text_static(pl.status, _("Can't play this video"));
	lv_obj_set_hidden(pl.status, false);
	lv_obj_set_hidden(pl.controls, true);
	if (pl.picture)
		lv_obj_set_hidden(pl.picture, true);
	lv_timer_pause(pl.timer);
}

static void play_toggle(void);

static void tick_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	enum video_state state = video_poll(pl.video);
	if (state == VIDEO_FAILED) {
		pl.state = state;
		failed();
		return;
	}
	struct video_frame f;
	if (video_frame(pl.video, &f)) {
		if (f.width != pl.width || f.height != pl.height) {
			pl.width = f.width;
			pl.height = f.height;
			picture_layout();
		}
#if LV_USE_3DTEXTURE
		gl_convert(&f);
		if (pl.picture)
			lv_obj_invalidate(pl.picture);
#endif
		lv_obj_set_hidden(pl.status, true);
	}
	if (state != pl.state) {
		pl.state = state;
		state_show();
	}
	/* Locked (the lock screen is over it, not a screen of its own): paused, left so */
	if ((state == VIDEO_PLAYING || state == VIDEO_LOADING) && ui_locked()) {
		play_toggle();
		state = pl.state;
	}
	position_show();
	if (state == VIDEO_PLAYING) {
		lv_display_trigger_activity(lv_display_get_default()); /* the screen stays on (power.c) */
		if (pl.sideways && !lv_obj_is_hidden(pl.controls) &&
		    !lv_obj_has_state(pl.slider, LV_STATE_PRESSED) && lv_tick_elaps(pl.touched) > HIDE_MS)
			controls_show(false);
	}
}

static void play_toggle(void)
{
	bool play = pl.state != VIDEO_PLAYING && pl.state != VIDEO_LOADING;
	video_play(pl.video, play);
	pl.state = video_poll(pl.video);
	state_show();
	pl.second = UINT32_MAX;
	position_show();
}

static void play_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	play_toggle();
	controls_show(true);
}

/* Upright a tap plays or pauses; sideways it shows the controls, or hides them */
static void frame_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (pl.state == VIDEO_FAILED)
		return;
	if (pl.sideways)
		controls_show(lv_obj_is_hidden(pl.controls));
	else
		play_toggle();
}

static void seek_cb(lv_event_t *e)
{
	uint32_t s = (uint32_t)lv_slider_get_value(pl.slider);
	controls_show(true);
	if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
		video_seek(pl.video, s * 1000);
		pl.second = UINT32_MAX;
		position_show();
		return;
	}
	time_set(pl.elapsed, s);
}

static void back_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_pop();
}

static void size_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	layout();
}

/* Covered by another screen (a call): paused, left so */
static void cover_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (pl.state == VIDEO_PLAYING || pl.state == VIDEO_LOADING)
		play_toggle();
}

static void player_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (pl.timer)
		lv_timer_delete(pl.timer);
	video_close(pl.video);
#if LV_USE_3DTEXTURE
	gl_deinit();
#endif
	memset(&pl, 0, sizeof(pl));
}

static lv_obj_t *time_label(lv_obj_t *parent)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_style_text_font(label, UI_FONT_CAPTION, 0);
	lv_obj_set_style_text_color(label, lv_color_white(), 0);
	lv_label_set_text_static(label, "0:00");
	return label;
}

static void player_open(const char *name)
{
	char path[512];
	snprintf(path, sizeof(path), VIDEOS "/%s", name);

	lv_obj_t *body;
	pl.scr = ui_app_screen_create(title_of(name), &pl.header, &body);
	lv_obj_add_event_cb(pl.scr, player_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_add_event_cb(pl.scr, cover_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
	lv_obj_set_scrollable(body, false);

	pl.frame = ui_box(body);
	lv_obj_set_width(pl.frame, LV_PCT(100));
	lv_obj_set_flex_grow(pl.frame, 1);
	lv_obj_set_style_bg_color(pl.frame, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(pl.frame, LV_OPA_COVER, 0);
	lv_obj_set_clickable(pl.frame, true);
	lv_obj_set_scrollable(pl.frame, false);
	lv_obj_add_event_cb(pl.frame, frame_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(pl.frame, size_cb, LV_EVENT_SIZE_CHANGED, NULL);

	pl.status = lv_label_create(pl.frame);
	lv_label_set_text_static(pl.status, _("Loading..."));
	lv_obj_set_style_text_color(pl.status, lv_color_hex(0x8e8e93), 0);
	lv_obj_center(pl.status);

	/* Controls: back (sideways), play, the time played, the bar, the length */
	pl.controls = ui_box(body);
	lv_obj_set_size(pl.controls, LV_PCT(100), CONTROLS_H);
	lv_obj_set_style_bg_color(pl.controls, lv_color_black(), 0);
	lv_obj_set_flex_flow(pl.controls, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(pl.controls, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(pl.controls, 24, 0);
	lv_obj_set_style_pad_column(pl.controls, 24, 0);
	lv_obj_set_clickable(pl.controls, true); /* taps on it do not reach the picture */
	pl.back = ui_button(pl.controls, 88, 88, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, LV_SYMBOL_LEFT,
	                    back_cb, NULL);
	pl.play = ui_button(pl.controls, 88, 88, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED, LV_SYMBOL_PAUSE,
	                    play_cb, NULL);
	lv_obj_set_style_text_font(lv_obj_get_child(pl.play, 0), &ui_font_32, 0);
	pl.elapsed = time_label(pl.controls);
	pl.slider = lv_slider_create(pl.controls);
	lv_obj_set_flex_grow(pl.slider, 1);
	lv_obj_set_height(pl.slider, 8);
	lv_obj_set_ext_click_area(pl.slider, 40);
	lv_obj_set_style_bg_color(pl.slider, lv_color_hex(0x48484a), LV_PART_MAIN);
	lv_obj_set_style_bg_color(pl.slider, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(pl.slider, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_shadow_width(pl.slider, 0, LV_PART_KNOB);
	lv_obj_add_event_cb(pl.slider, seek_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(pl.slider, seek_cb, LV_EVENT_RELEASED, NULL);
	pl.total = time_label(pl.controls);

	pl.second = UINT32_MAX;
	pl.duration = UINT32_MAX;
	pl.video = video_open(path, ui_sound_device());
	pl.state = VIDEO_LOADING;
	pl.timer = lv_timer_create(tick_cb, TICK_MS, NULL);

#if LV_USE_3DTEXTURE
	if (gl_init()) {
		pl.picture = lv_3dtexture_create(pl.frame);
		lv_obj_set_clickable(pl.picture, false);
		lv_obj_set_hidden(pl.picture, true);
	} else {
		printf("videos: no GPU for the picture\n");
	}
#endif
	ui_screen_turns(pl.scr, true);
	layout();
	if (!pl.video) {
		lv_timer_pause(pl.timer);
		lv_label_set_text_static(pl.status, _("Can't play this video"));
		lv_obj_set_hidden(pl.controls, true);
	}
	ui_push(pl.scr);
}

/* The list's screen */

static void row_cb(lv_event_t *e)
{
	unsigned i = (unsigned)(uintptr_t)lv_event_get_user_data(e);
	if (i < lib.count && !pl.scr)
		player_open(lib.entries[i].name);
}

static void row_create(lv_obj_t *card, unsigned i, bool divider)
{
	const struct entry *en = &lib.entries[i];
	lv_obj_t *row = ui_row_create(card, ROW_H, divider, row_cb, (void *)(uintptr_t)i);

	lv_obj_t *thumb = ui_box(row);
	lv_obj_set_size(thumb, 72, 72);
	lv_obj_set_style_radius(thumb, 12, 0);
	lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(thumb, lv_color_hex(COLOR_ICON), 0);
	lv_obj_t *icon = lv_label_create(thumb);
	lv_label_set_text_static(icon, LV_SYMBOL_VIDEO);
	lv_obj_set_style_text_font(icon, &ui_font_26, 0);
	lv_obj_set_style_text_color(icon, lv_color_white(), 0);
	lv_obj_center(icon);

	lv_obj_t *text = ui_row_text(row);
	lv_obj_t *title = ui_one_line_label(text, title_of(en->name), UI_FONT_TITLE);
	lv_obj_set_width(title, LV_PCT(100));
	char size[32];
	ui_size_format(size, sizeof(size), en->size);
	lv_obj_t *line = ui_one_line_label(text, ui_intern(size), UI_FONT_CAPTION);
	lv_obj_set_style_text_color(line, lv_color_hex(UI_COLOR_DIM), 0);
}

static void list_fill(void)
{
	lv_obj_clean(lib.list);
	if (!lib.count) {
		ui_empty_label(lib.list, _("No videos"));
		char text[96];
		snprintf(text, sizeof(text), _("Copy videos to %s"), VIDEOS);
		lv_obj_t *hint = ui_dim_label(lib.list, ui_intern(text), UI_FONT_CAPTION);
		lv_obj_set_width(hint, LV_PCT(100));
		lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_margin_top(hint, 16, 0);
		return;
	}
	lv_obj_t *card = ui_card_create(lib.list);
	for (unsigned i = 0; i < lib.count; i++)
		row_create(card, i, i + 1 < lib.count);
}

static void pull_cb(void *user_data)
{
	LV_UNUSED(user_data);
	library_load();
	list_fill();
	ui_pull_refresh_done(lib.list);
}

static void lib_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	library_free();
	memset(&lib, 0, sizeof(lib));
}

void videos_open(void)
{
	library_load();
	lv_obj_t *body;
	lib.scr = ui_app_screen_create(_("Videos"), NULL, &body);
	lv_obj_add_event_cb(lib.scr, lib_delete_cb, LV_EVENT_DELETE, NULL);
	lib.list = body;
	lv_obj_set_style_pad_hor(body, 16, 0);
	lv_obj_set_style_pad_bottom(body, 32, 0);
	lv_obj_set_style_pad_row(body, 8, 0);
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
	ui_pull_refresh(body, pull_cb, NULL);
	list_fill();
	ui_push(lib.scr);
}
