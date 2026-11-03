/*
 * Camera: live preview of the rear camera (OV5640 on the CSI parallel bus) and a
 * shutter that saves the frame.
 *
 * The media pipeline is sensor -> sun6i-csi-bridge -> sun6i-csi-capture
 * (/dev/video0), all in UYVY at FRAME_W x FRAME_H. Each frame is uploaded as is
 * into a GL texture, and a shader converts it to RGB and turns it to portrait into
 * the preview texture, which an lv_3dtexture shows scaled to the screen. On the
 * CPU, the conversion alone took more than half a core; a changing LVGL image
 * would also be copied, cleared and uploaded again by the draw unit every frame.
 *
 * Photos go to /var/lib/photos as PPM (no image encoder is built yet).
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/media-bus-format.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ui.h"

#if LV_USE_3DTEXTURE

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define FRAME_W 640 /* sensor frame, landscape */
#define FRAME_H 480
#define BUFFERS 4
#define PHOTOS "/var/lib/photos"

static struct camera {
	int video;
	struct { void *data; size_t size; } buf[BUFFERS];
	unsigned buffers;
	bool streaming;
	GLuint frame;       /* UYVY frame: FRAME_W / 2 x FRAME_H, one U Y V Y group per texel */
	GLuint preview_tex; /* RGB, portrait: FRAME_H x FRAME_W */
	GLuint fbo, program;
	lv_obj_t *preview, *status, *flash;
	lv_timer_t *timer;
	unsigned frames;
} cam = { .video = -1 };

/* /dev node of the V4L2 device or subdevice named `name` (sysfs), or -1 */
static int open_by_name(const char *prefix, const char *name)
{
	DIR *dir = opendir("/sys/class/video4linux");
	struct dirent *ent;
	int fd = -1;

	while (dir && fd < 0 && (ent = readdir(dir))) {
		char path[300], buf[64] = "";
		if (strncmp(ent->d_name, prefix, strlen(prefix)) != 0)
			continue;
		snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", ent->d_name);
		FILE *f = fopen(path, "r");
		if (f) {
			if (!fgets(buf, sizeof(buf), f))
				buf[0] = '\0';
			fclose(f);
		}
		if (strncmp(buf, name, strlen(name)) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/%s", ent->d_name);
		fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	}
	if (dir)
		closedir(dir);
	return fd;
}

static bool subdev_set_format(const char *name, unsigned pad)
{
	int fd = open_by_name("v4l-subdev", name);
	if (fd < 0)
		return false;
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = pad,
		.format = {
			.width = FRAME_W, .height = FRAME_H,
			.code = MEDIA_BUS_FMT_UYVY8_2X8, .field = V4L2_FIELD_NONE,
		},
	};
	bool ok = ioctl(fd, VIDIOC_SUBDEV_S_FMT, &fmt) == 0;
	if (!ok)
		printf("camera: %s pad %u: %s\n", name, pad, strerror(errno));
	close(fd);
	return ok;
}

static void camera_stop(void)
{
	if (cam.streaming) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		ioctl(cam.video, VIDIOC_STREAMOFF, &type);
		cam.streaming = false;
	}
	for (unsigned i = 0; i < cam.buffers; i++)
		munmap(cam.buf[i].data, cam.buf[i].size);
	cam.buffers = 0;
	if (cam.video >= 0) {
		/* Frees the buffers, which powers the sensor down */
		close(cam.video);
		cam.video = -1;
	}
}

static const char *camera_start(void)
{
	if (!subdev_set_format("ov5640", 0) ||
	    !subdev_set_format("sun6i-csi-bridge", 0) ||
	    !subdev_set_format("sun6i-csi-bridge", 1))
		return "Camera not found";

	cam.video = open_by_name("video", "sun6i-csi-capture");
	if (cam.video < 0)
		return "Camera not found";

	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	fmt.fmt.pix.width = FRAME_W;
	fmt.fmt.pix.height = FRAME_H;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (ioctl(cam.video, VIDIOC_S_FMT, &fmt) < 0 ||
	    fmt.fmt.pix.width != FRAME_W || fmt.fmt.pix.height != FRAME_H)
		return "Unsupported camera format";

	struct v4l2_requestbuffers req = {
		.count = BUFFERS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
	};
	/* Cached buffers (V4L2_MEMORY_FLAG_NON_COHERENT, the first byte of reserved[]
	 * in these older headers): reading uncached frames is slow. Kernels that do
	 * not allow it ignore it. */
	((uint8_t *)req.reserved)[0] = 1;
	if (ioctl(cam.video, VIDIOC_REQBUFS, &req) < 0 || req.count < 2)
		return "Not enough memory for the camera";

	for (cam.buffers = 0; cam.buffers < req.count; cam.buffers++) {
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
			.index = cam.buffers,
		};
		if (ioctl(cam.video, VIDIOC_QUERYBUF, &b) < 0)
			return "Camera buffers failed";
		void *p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, cam.video, b.m.offset);
		if (p == MAP_FAILED)
			return "Camera buffers failed";
		cam.buf[cam.buffers].data = p;
		cam.buf[cam.buffers].size = b.length;
		if (ioctl(cam.video, VIDIOC_QBUF, &b) < 0)
			return "Camera buffers failed";
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(cam.video, VIDIOC_STREAMON, &type) < 0)
		return "Camera did not start";
	cam.streaming = true;
	return NULL;
}

/*
 * The conversion pass, drawn over the whole preview texture. Output pixel (x, y) in
 * portrait comes from sensor pixel (y, FRAME_H - 1 - x): the sensor is mounted
 * rotated. The output is in LVGL's texture byte order (B, G, R, A). No branches:
 * the Mali-400 runs both sides of every branch.
 */
static const char *vertex_source =
	"attribute vec2 a_pos;\n"
	"void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char *fragment_source =
	"precision mediump float;\n"
	"uniform sampler2D u_frame;\n"
	"const vec2 size = vec2(640.0, 480.0);\n"
	"void main()\n"
	"{\n"
	"    vec2 s = vec2(gl_FragCoord.y, size.y - gl_FragCoord.x);\n"
	"    float x = floor(s.x);\n"
	"    vec4 t = texture2D(u_frame, vec2((floor(x * 0.5) + 0.5) / (size.x * 0.5), s.y / size.y));\n"
	"    float l = 1.164 * (mix(t.g, t.a, mod(x, 2.0)) - 0.0627);\n"
	"    float u = t.r - 0.5, v = t.b - 0.5;\n"
	"    vec3 rgb = vec3(l + 1.793 * v, l - 0.213 * u - 0.533 * v, l + 2.112 * u);\n" /* BT.709 */
	"    gl_FragColor = vec4(clamp(rgb.bgr, 0.0, 1.0), 1.0);\n"
	"}\n";

/* GLES 2 has vertex array objects as an extension (LVGL uses them) */
static PFNGLBINDVERTEXARRAYOESPROC bind_vertex_array;

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
		printf("camera: shader: %s\n", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
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
		cam.program = glCreateProgram();
		glAttachShader(cam.program, vs);
		glAttachShader(cam.program, fs);
		glBindAttribLocation(cam.program, 0, "a_pos");
		glLinkProgram(cam.program);
		glGetProgramiv(cam.program, GL_LINK_STATUS, &ok);
	}
	if (vs)
		glDeleteShader(vs);
	if (fs)
		glDeleteShader(fs);
	if (!ok)
		return false;

	glGenTextures(1, &cam.frame);
	glBindTexture(GL_TEXTURE_2D, cam.frame);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FRAME_W / 2, FRAME_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	glGenTextures(1, &cam.preview_tex);
	glBindTexture(GL_TEXTURE_2D, cam.preview_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FRAME_H, FRAME_W, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &cam.fbo);
	return true;
}

static void gl_deinit(void)
{
	if (cam.fbo)
		glDeleteFramebuffers(1, &cam.fbo);
	if (cam.frame)
		glDeleteTextures(1, &cam.frame);
	if (cam.preview_tex)
		glDeleteTextures(1, &cam.preview_tex);
	if (cam.program)
		glDeleteProgram(cam.program);
	cam.fbo = cam.frame = cam.preview_tex = cam.program = 0;
}

/*
 * Runs in LVGL's thread, between frames, in its GL context: keep what LVGL relies
 * on across draws (framebuffer, viewport, program, array buffer), and draw with no
 * vertex array object bound so that LVGL's are not changed.
 */
static void gl_convert(const void *uyvy)
{
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	GLint fbo, program, buffer, viewport[4];
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
	glGetIntegerv(GL_VIEWPORT, viewport);

	glBindTexture(GL_TEXTURE_2D, cam.frame);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, FRAME_W / 2, FRAME_H, GL_RGBA, GL_UNSIGNED_BYTE, uyvy);

	glBindFramebuffer(GL_FRAMEBUFFER, cam.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cam.preview_tex, 0);
	glViewport(0, 0, FRAME_H, FRAME_W);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glUseProgram(cam.program);
	glUniform1i(glGetUniformLocation(cam.program, "u_frame"), 0);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	bind_vertex_array(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);

	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	glUseProgram((GLuint)program);
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)buffer);
}

static void frame_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
	struct v4l2_buffer newest = b;
	bool got = false;

	/* Show the newest frame, give back the older ones */
	while (ioctl(cam.video, VIDIOC_DQBUF, &b) == 0) {
		if (got)
			ioctl(cam.video, VIDIOC_QBUF, &newest);
		newest = b;
		got = true;
	}
	if (!got)
		return;
	gl_convert(cam.buf[newest.index].data);
	ioctl(cam.video, VIDIOC_QBUF, &newest);
	lv_obj_invalidate(cam.preview);

	if (cam.frames++ == 0)
		lv_obj_set_hidden(cam.status, true);
}

static void flash_done_cb(lv_anim_t *a)
{
	lv_obj_set_hidden(a->var, true);
}

static void flash_opa_cb(void *obj, int32_t v)
{
	lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0);
}

/* The preview as it is shown, read back from the GPU (B, G, R, A rows) */
static void save_photo(const char *path)
{
	uint8_t *px = malloc(FRAME_W * FRAME_H * 4);
	GLint fbo;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, cam.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cam.preview_tex, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, FRAME_H, FRAME_W, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);

	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "P6\n%d %d\n255\n", FRAME_H, FRAME_W);
		for (int i = 0; i < FRAME_W * FRAME_H; i++) {
			const uint8_t *p = px + i * 4;
			uint8_t rgb[3] = { p[2], p[1], p[0] };
			fwrite(rgb, 1, 3, f);
		}
		fclose(f);
		printf("camera: saved %s\n", path);
	}
	free(px);
}

static void shutter_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!cam.frames)
		return;

	mkdir(PHOTOS, 0755);
	char path[64];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(path, sizeof(path), PHOTOS "/IMG_%Y%m%d_%H%M%S.ppm", &tm);
	save_photo(path);
	ui_vibrate(30);

	/* White flash over the preview */
	lv_obj_set_hidden(cam.flash, false);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, cam.flash);
	lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
	lv_anim_set_duration(&a, 250);
	lv_anim_set_exec_cb(&a, flash_opa_cb);
	lv_anim_set_completed_cb(&a, flash_done_cb);
	lv_anim_start(&a);
}

static void screen_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (cam.timer)
		lv_timer_delete(cam.timer);
	cam.timer = NULL;
	camera_stop();
	gl_deinit();
}

void camera_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Camera", NULL, &body);
	lv_obj_add_event_cb(scr, screen_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	/* Preview: the portrait frame scaled to the screen width */
	int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
	lv_obj_t *frame = lv_obj_create(body);
	lv_obj_remove_style_all(frame);
	lv_obj_set_size(frame, w, w * FRAME_W / FRAME_H);
	lv_obj_set_style_bg_color(frame, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);

	cam.status = lv_label_create(frame);
	lv_label_set_text_static(cam.status, "Starting camera...");
	lv_obj_set_style_text_color(cam.status, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_center(cam.status);

	/* Shutter: white disc in a white ring */
	lv_obj_t *controls = lv_obj_create(body);
	lv_obj_remove_style_all(controls);
	lv_obj_set_width(controls, LV_PCT(100));
	lv_obj_set_flex_grow(controls, 1);

	lv_obj_t *shutter = lv_button_create(controls);
	lv_obj_set_size(shutter, 136, 136);
	lv_obj_center(shutter);
	lv_obj_set_style_radius(shutter, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(shutter, lv_color_white(), 0);
	lv_obj_set_style_bg_color(shutter, lv_color_hex(0xc8c8d0), LV_STATE_PRESSED);
	lv_obj_set_style_border_color(shutter, lv_color_black(), 0);
	lv_obj_set_style_border_width(shutter, 6, 0);
	lv_obj_set_style_outline_color(shutter, lv_color_white(), 0);
	lv_obj_set_style_outline_width(shutter, 6, 0);
	lv_obj_set_style_outline_pad(shutter, 0, 0);
	lv_obj_set_style_shadow_width(shutter, 0, 0);
	lv_obj_add_event_cb(shutter, shutter_cb, LV_EVENT_CLICKED, NULL);

	cam.frames = 0;
	const char *error = gl_init() ? camera_start() : "No GPU for the preview";
	if (error) {
		printf("camera: %s\n", error);
		lv_label_set_text_static(cam.status, error);
		camera_stop();
		ui_push(scr);
		return;
	}

	cam.preview = lv_3dtexture_create(frame);
	lv_obj_set_size(cam.preview, LV_PCT(100), LV_PCT(100));
	lv_3dtexture_set_src(cam.preview, cam.preview_tex);
	lv_3dtexture_set_flip(cam.preview, false, true);
	lv_obj_move_to_index(cam.preview, 0);

	cam.flash = lv_obj_create(frame);
	lv_obj_remove_style_all(cam.flash);
	lv_obj_set_size(cam.flash, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_color(cam.flash, lv_color_white(), 0);
	lv_obj_set_hidden(cam.flash, true);

	cam.timer = lv_timer_create(frame_cb, 10, NULL);
	ui_push(scr);
}

#else

void camera_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Camera", NULL, &body);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *label = lv_label_create(body);
	lv_label_set_text_static(label, "The camera needs the GPU renderer");
	ui_push(scr);
}

#endif
