/*
 * Camera: live preview of the rear (OV5640) or front (GC2145) camera, both on the
 * CSI parallel bus, and a shutter that saves the frame.
 *
 * The media pipeline is sensor -> sun6i-csi-bridge -> sun6i-csi-capture
 * (/dev/video0), all in UYVY at the sensor's frame size; the bridge takes one sensor at
 * a time, chosen by enabling its media link. Each frame is uploaded as is
 * into a GL texture, and a shader converts it to RGB and turns it to portrait into
 * the preview texture, which an lv_3dtexture shows scaled to the screen. On the
 * CPU, the conversion alone took more than half a core; a changing LVGL image
 * would also be copied, cleared and uploaded again by the draw unit every frame.
 *
 * Focus (rear camera; the front one is fixed focus): continuous autofocus by the
 * sensor's firmware (ov5640_af.bin, loaded by the driver); a tap on the preview
 * focuses once on that spot and keeps the lens there, a long press goes back to
 * continuous.
 *
 * Photos go to /var/lib/photos as JPEG, seen in the gallery. The rear camera
 * takes them at its full 2592x1944: the shutter switches the stream to raw Bayer
 * at that size (UYVY would need too fast a pixel clock for the parallel bus and
 * delivers no frames), skips a few frames while the exposure settles, debayers
 * one on the CPU and switches back to the preview. The device stays open, so the
 * sensor stays powered and keeps its focus. The front camera's photos are its
 * preview frames, already at its full size.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/media-bus-format.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "i18n.h"
#include "ui.h"

#if LV_USE_3DTEXTURE

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define PREVIEW_W 720 /* preview texture, portrait: the size it is shown at */
#define PREVIEW_H 960
#define BUFFERS 4
#define PHOTOS "/var/lib/photos"
#define JPEG_QUALITY 90
#define STILL_SKIP 2        /* raw frames dropped while the exposure settles */
#define STILL_TIMEOUT 3000  /* ms after the shutter: no photo, back to the preview */

/*
 * The sensors, by their subdevice name, the frame size they stream at
 * (landscape) and its media bus code, and the size photos are taken at in raw
 * BGGR (still_w 0: from the preview frames). Both are mounted rotated: sensor x
 * runs along the preview's height and sensor y along its width, dir_x and dir_y
 * say in which direction (+1: the same as GL coordinates, y up).
 */
static const struct sensor {
	const char *name;
	int width, height;
	uint32_t code;
	int dir_x, dir_y;
	bool focus;
	int still_w, still_h;
} sensors[] = {
	/* rear: 1280x720 at 30 fps, cropped to the preview at one sensor pixel per
	 * screen pixel (larger sizes in UYVY deliver no frames); photos at the
	 * full 2592x1944 (15 fps, the driver's default for that size) */
	{ "ov5640", 1280, 720, MEDIA_BUS_FMT_UYVY8_2X8, -1, -1, true, 2592, 1944 },
	/* front: its full 1600x1200 at 16 fps, scaled down; the mainline driver
	 * (16 bit bus codes) with the vendor's settings, its own exposure */
	{ "gc2145", 1600, 1200, MEDIA_BUS_FMT_UYVY8_1X16, 1, -1, false, 0, 0 },
};

static struct camera {
	const struct sensor *sensor_info;
	int video;
	int sensor; /* the sensor's subdevice, for the focus controls */
	struct { void *data; size_t size; } buf[BUFFERS];
	unsigned buffers;
	bool streaming;
	int stride;         /* bytes per line of the frames streamed */
	int still_skip;     /* -1: preview, else raw frames still to drop */
	bool focus_continuous; /* continuous autofocus, not a tapped spot */
	uint32_t still_start; /* lv_tick_get() when the raw stream started */
	GLuint frame;       /* UYVY frame: width / 2 x height, one U Y V Y group per texel */
	int frame_w, frame_h; /* the size it is allocated at */
	GLuint parity;      /* width x 1: 0, 1, 0, 1..., which of a texel's two pixels */
	GLuint preview_tex; /* RGB, portrait: PREVIEW_W x PREVIEW_H */
	GLuint fbo, program;
	/* sensor pixel for preview pixel (x, y), GL coordinates:
	 * (map[0] + map[1] * y, map[2] + map[3] * x) */
	float map[4];
	lv_obj_t *preview, *status, *flash;
	lv_obj_t *ring; /* where a tap focuses */
	lv_timer_t *timer, *focus_timer;
	unsigned frames, focus_polls;
	char photo[64]; /* where the next frame goes as a photo, if set */
} cam = { .sensor_info = &sensors[0], .video = -1, .sensor = -1, .still_skip = -1 };

/* /dev node of the V4L2 device or subdevice named `name` (sysfs), or -1 */
static int open_by_name(const char *prefix, const char *name)
{
	DIR *dir = opendir("/sys/class/video4linux");
	struct dirent *ent;
	int fd = -1;

	while (dir && fd < 0 && (ent = readdir(dir))) {
		char path[300], buf[64];
		if (strncmp(ent->d_name, prefix, strlen(prefix)) != 0)
			continue;
		snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", ent->d_name);
		if (!ui_read_line(path, buf, sizeof(buf)) || strncmp(buf, name, strlen(name)) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/%s", ent->d_name);
		fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	}
	if (dir)
		closedir(dir);
	return fd;
}

static bool subdev_set_format(const char *name, unsigned pad, int width, int height, uint32_t code)
{
	int fd = open_by_name("v4l-subdev", name);
	if (fd < 0)
		return false;
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = pad,
		.format = {
			.width = (uint32_t)width, .height = (uint32_t)height,
			.code = code, .field = V4L2_FIELD_NONE,
		},
	};
	bool ok = ioctl(fd, VIDIOC_SUBDEV_S_FMT, &fmt) == 0;
	if (!ok)
		printf("camera: %s pad %u: %s\n", name, pad, strerror(errno));
	close(fd);
	return ok;
}

/* Stops streaming and frees the buffers, the device stays open (and the sensor
 * powered for a moment: the driver suspends it after a second) */
static void stream_stop(void)
{
	if (cam.streaming) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		ioctl(cam.video, VIDIOC_STREAMOFF, &type);
		cam.streaming = false;
	}
	for (unsigned i = 0; i < cam.buffers; i++)
		munmap(cam.buf[i].data, cam.buf[i].size);
	cam.buffers = 0;
	struct v4l2_requestbuffers req = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
	if (cam.video >= 0)
		ioctl(cam.video, VIDIOC_REQBUFS, &req);
}

static void camera_stop(void)
{
	stream_stop();
	cam.still_skip = -1;
	cam.photo[0] = '\0';
	if (cam.sensor >= 0) {
		close(cam.sensor);
		cam.sensor = -1;
	}
	if (cam.video >= 0) {
		/* Frees the buffers, which powers the sensor down */
		close(cam.video);
		cam.video = -1;
	}
}

static bool sensor_set(uint32_t id, int32_t value)
{
	struct v4l2_control c = { .id = id, .value = value };
	return cam.sensor >= 0 && ioctl(cam.sensor, VIDIOC_S_CTRL, &c) == 0;
}

static int32_t sensor_get(uint32_t id)
{
	struct v4l2_control c = { .id = id };
	return cam.sensor >= 0 && ioctl(cam.sensor, VIDIOC_G_CTRL, &c) == 0 ? c.value : -1;
}

/* Links sensor `name` to the CSI bridge and unlinks the others: sensors have one
 * link each, to the bridge, and the bridge streams only with one enabled */
static bool media_select(const char *name)
{
	int fd = open("/dev/media0", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return false;

	struct media_link_desc wanted = { 0 };
	struct media_entity_desc ent = { .id = MEDIA_ENT_ID_FLAG_NEXT };
	while (ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &ent) == 0) {
		struct media_link_desc link;
		struct media_links_enum links = { .entity = ent.id, .links = &link };
		bool sensor = false;
		for (unsigned i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++)
			sensor |= strncmp(ent.name, sensors[i].name, strlen(sensors[i].name)) == 0;

		if (sensor && ent.links == 1 && ioctl(fd, MEDIA_IOC_ENUM_LINKS, &links) == 0) {
			if (strncmp(ent.name, name, strlen(name)) == 0) {
				wanted = link;
			} else if (link.flags & MEDIA_LNK_FL_ENABLED) {
				link.flags &= ~MEDIA_LNK_FL_ENABLED;
				ioctl(fd, MEDIA_IOC_SETUP_LINK, &link);
			}
		}
		ent.id |= MEDIA_ENT_ID_FLAG_NEXT;
	}

	wanted.flags |= MEDIA_LNK_FL_ENABLED;
	bool ok = wanted.source.entity && ioctl(fd, MEDIA_IOC_SETUP_LINK, &wanted) == 0;
	close(fd);
	return ok;
}

/* The sensor's frame scaled to cover the preview, centred (cut on one side) */
static void map_compute(const struct sensor *s)
{
	float k = (float)s->width / PREVIEW_H;
	if ((float)s->height / PREVIEW_W < k)
		k = (float)s->height / PREVIEW_W;
	float ox = (s->width - k * PREVIEW_H) / 2, oy = (s->height - k * PREVIEW_W) / 2;
	cam.map[0] = s->dir_x > 0 ? ox : s->width - ox;
	cam.map[1] = s->dir_x * k;
	cam.map[2] = s->dir_y > 0 ? oy : s->height - oy;
	cam.map[3] = s->dir_y * k;
}

/* Streams the selected sensor at the preview size (UYVY) or the still size (raw) */
static const char *stream_start(bool still)
{
	const struct sensor *s = cam.sensor_info;
	int width = still ? s->still_w : s->width, height = still ? s->still_h : s->height;
	uint32_t code = still ? MEDIA_BUS_FMT_SBGGR8_1X8 : s->code;
	if (!subdev_set_format(s->name, 0, width, height, code) ||
	    !subdev_set_format("sun6i-csi-bridge", 0, width, height, code) ||
	    !subdev_set_format("sun6i-csi-bridge", 1, width, height, code))
		return N_("Camera not found");

	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	fmt.fmt.pix.width = (uint32_t)width;
	fmt.fmt.pix.height = (uint32_t)height;
	fmt.fmt.pix.pixelformat = still ? V4L2_PIX_FMT_SBGGR8 : V4L2_PIX_FMT_UYVY;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (ioctl(cam.video, VIDIOC_S_FMT, &fmt) < 0 ||
	    fmt.fmt.pix.width != (uint32_t)width || fmt.fmt.pix.height != (uint32_t)height)
		return N_("Unsupported camera format");
	cam.stride = (int)fmt.fmt.pix.bytesperline;

	struct v4l2_requestbuffers req = {
		.count = BUFFERS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
	};
	/* Cached buffers (V4L2_MEMORY_FLAG_NON_COHERENT, the first byte of reserved[]
	 * in these older headers): reading uncached frames is slow. Kernels that do
	 * not allow it ignore it. */
	((uint8_t *)req.reserved)[0] = 1;
	if (ioctl(cam.video, VIDIOC_REQBUFS, &req) < 0 || req.count < 2)
		return N_("Not enough memory for the camera");

	for (cam.buffers = 0; cam.buffers < req.count; cam.buffers++) {
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
			.index = cam.buffers,
		};
		if (ioctl(cam.video, VIDIOC_QUERYBUF, &b) < 0)
			return N_("Camera buffers failed");
		void *p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, cam.video, b.m.offset);
		if (p == MAP_FAILED)
			return N_("Camera buffers failed");
		cam.buf[cam.buffers].data = p;
		cam.buf[cam.buffers].size = b.length;
		if (ioctl(cam.video, VIDIOC_QBUF, &b) < 0)
			return N_("Camera buffers failed");
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(cam.video, VIDIOC_STREAMON, &type) < 0)
		return N_("Camera did not start");
	cam.streaming = true;
	return NULL;
}

static const char *camera_start(void)
{
	const char *name = cam.sensor_info->name;
	map_compute(cam.sensor_info);
	if (!media_select(name))
		return N_("Camera not found");
	cam.video = open_by_name("video", "sun6i-csi-capture");
	if (cam.video < 0)
		return N_("Camera not found");
	const char *error = stream_start(false);
	if (error)
		return error;

	/* Focus controls only work while streaming (the sensor is powered) */
	cam.sensor = open_by_name("v4l-subdev", name);
	cam.focus_continuous = true;
	if (cam.sensor_info->focus)
		sensor_set(V4L2_CID_FOCUS_AUTO, 1);
	return NULL;
}

/*
 * The conversion pass, drawn over the whole preview texture. Output pixel (x, y) in
 * portrait comes from the sensor pixel given by u_map (camera.map). The output is
 * in LVGL's texture byte order (B, G, R, A). No branches: the Mali-400 runs both
 * sides of every branch.
 *
 * The Mali-400 computes fragments in 16-bit floats, which cannot tell pixel 1001
 * from 1002: the coordinates come from the vertex shader (32-bit) and go to the
 * texture units as they are, and which pixel of a U Y V Y texel to take is looked
 * up, not computed: in a texture as wide as the frame, 0 and 1 alternating, at the
 * same coordinates (with larger ones, 0 to 640, the lookup goes wrong too).
 *
 * When the frame is scaled down (u_box), each output pixel is the mean of a 2 x 2
 * block, a texel and the one below it: picking one sensor pixel out of several
 * keeps all of their noise.
 */
static const char *vertex_source =
	"attribute vec2 a_pos;\n"
	"uniform vec4 u_map;\n"
	"uniform vec2 u_size;\n" /* frame */
	"uniform vec2 u_out;\n"  /* output */
	"varying vec2 v_frame;\n"
	"varying vec2 v_below;\n"
	"void main()\n"
	"{\n"
	"    vec2 p = (a_pos * 0.5 + 0.5) * u_out;\n"
	"    vec2 s = u_map.xz + u_map.yw * p.yx;\n"
	"    v_frame = s / u_size;\n"
	"    v_below = v_frame + vec2(0.0, 1.0 / u_size.y);\n"
	"    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

static const char *fragment_source =
	"precision mediump float;\n"
	"uniform sampler2D u_frame;\n"
	"uniform sampler2D u_parity;\n"
	"uniform float u_box;\n"
	"varying vec2 v_frame;\n"
	"varying vec2 v_below;\n"
	"void main()\n"
	"{\n"
	"    vec4 t = texture2D(u_frame, v_frame);\n"
	"    vec4 b = texture2D(u_frame, v_below);\n"
	"    float l = mix(mix(t.g, t.a, texture2D(u_parity, v_frame).r),\n"
	"                  (t.g + t.a + b.g + b.a) * 0.25, u_box);\n"
	"    vec2 c = mix(t.rb, (t.rb + b.rb) * 0.5, u_box) - 0.502;\n"
	"    float u = c.x, v = c.y;\n"
	/* BT.601, full range: what both sensors send */
	"    vec3 rgb = vec3(l + 1.402 * v, l - 0.344 * u - 0.714 * v, l + 1.772 * u);\n"
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
	/* frame and parity: allocated for the sensor's size by gl_convert() */
	cam.frame_w = cam.frame_h = 0;

	glGenTextures(1, &cam.parity);
	glBindTexture(GL_TEXTURE_2D, cam.parity);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glGenTextures(1, &cam.preview_tex);
	glBindTexture(GL_TEXTURE_2D, cam.preview_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, PREVIEW_W, PREVIEW_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
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
	if (cam.parity)
		glDeleteTextures(1, &cam.parity);
	if (cam.preview_tex)
		glDeleteTextures(1, &cam.preview_tex);
	if (cam.program)
		glDeleteProgram(cam.program);
	cam.fbo = cam.frame = cam.parity = cam.preview_tex = cam.program = 0;
}

/*
 * Runs in LVGL's thread, between frames, in its GL context: keep what LVGL relies
 * on across draws (framebuffer, viewport, program, array buffer), and draw with no
 * vertex array object bound so that LVGL's are not changed.
 */
static void gl_convert(const void *uyvy, GLuint target, int out_w, int out_h, const float *map)
{
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	const struct sensor *s = cam.sensor_info;
	GLint fbo, program, buffer, viewport[4], active;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
	glGetIntegerv(GL_VIEWPORT, viewport);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, cam.parity);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, cam.frame);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	if (!uyvy) {
		/* the frame uploaded last */
	} else if (cam.frame_w != s->width || cam.frame_h != s->height) {
		cam.frame_w = s->width;
		cam.frame_h = s->height;
		uint8_t parity[s->width];
		for (int i = 0; i < s->width; i++)
			parity[i] = i & 1 ? 255 : 0;
		glActiveTexture(GL_TEXTURE1);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, s->width, 1, 0,
		             GL_LUMINANCE, GL_UNSIGNED_BYTE, parity);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glActiveTexture(GL_TEXTURE0);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s->width / 2, s->height, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, uyvy);
	} else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s->width / 2, s->height,
		                GL_RGBA, GL_UNSIGNED_BYTE, uyvy);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, cam.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
	glViewport(0, 0, out_w, out_h);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glUseProgram(cam.program);
	glUniform1i(glGetUniformLocation(cam.program, "u_frame"), 0);
	glUniform1i(glGetUniformLocation(cam.program, "u_parity"), 1);
	glUniform4fv(glGetUniformLocation(cam.program, "u_map"), 1, map);
	glUniform2f(glGetUniformLocation(cam.program, "u_size"), (float)s->width, (float)s->height);
	glUniform2f(glGetUniformLocation(cam.program, "u_out"), (float)out_w, (float)out_h);
	/* Scaled down by more than a bit: average 2 x 2 sensor pixels, not pick one */
	glUniform1f(glGetUniformLocation(cam.program, "u_box"), fabsf(map[1]) > 1.2f ? 1.0f : 0.0f);

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
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	glUseProgram((GLuint)program);
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)buffer);
}

/* w x h pixels as read back from the texture (LVGL's layout, red and blue
 * swapped), GL's bottom row first */
static bool jpeg_write(const char *path, const uint8_t *px, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror("camera: photo");
		return false;
	}
	bool ok = ui_jpeg_write(f, px + (size_t)(h - 1) * w * 4, w, h, -w * 4, JPEG_QUALITY, false);
	if (fclose(f) != 0) {
		perror("camera: photo");
		ok = false;
	}
	if (ok)
		printf("camera: saved %s (%dx%d)\n", path, w, h);
	return ok;
}

/*
 * A photo: the frame uploaded last, converted again at the sensor's full size
 * (portrait, nothing cut), read back and written top row first (GL's first row is
 * the bottom one).
 */
static void photo_save(const char *path)
{
	const struct sensor *s = cam.sensor_info;
	int w = s->height, h = s->width;
	float map[4] = {
		s->dir_x > 0 ? 0 : s->width, s->dir_x, s->dir_y > 0 ? 0 : s->height, s->dir_y,
	};

	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	gl_convert(NULL, tex, w, h, map);

	uint8_t *px = malloc((size_t)w * h * 4);
	GLint fbo;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, cam.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
	glDeleteTextures(1, &tex);

	if (px && !jpeg_write(path, px, w, h))
		unlink(path);
	free(px);
}

/*
 * A photo from a raw BGGR frame at the still size: bilinear demosaicing straight
 * into the portrait picture (each output row is a sensor column). The sensor's
 * raw output already has its white balance, lens correction and a gamma curve:
 * nothing else to do. Neighbours past an edge are taken mirrored, which keeps
 * their colour.
 */
static void raw_photo_save(const char *path, const uint8_t *raw)
{
	const struct sensor *s = cam.sensor_info;
	int sw = s->still_w, sh = s->still_h, w = sh, h = sw;
	uint8_t *px = malloc((size_t)w * h * 4);
	if (!px)
		return;
	for (int y = 0; y < h; y++) {
		int sx = s->dir_x > 0 ? sw - 1 - y : y;
		int l = sx > 0 ? sx - 1 : 1, r = sx < sw - 1 ? sx + 1 : sw - 2;
		uint8_t *out = px + (size_t)y * w * 4;
		for (int x = 0; x < w; x++, out += 4) {
			int sy = s->dir_y > 0 ? x : sh - 1 - x;
			const uint8_t *m = raw + (size_t)sy * cam.stride;
			const uint8_t *u = raw + (size_t)(sy > 0 ? sy - 1 : 1) * cam.stride;
			const uint8_t *d = raw + (size_t)(sy < sh - 1 ? sy + 1 : sh - 2) * cam.stride;
			int c = m[sx], cross = (u[sx] + d[sx] + m[l] + m[r] + 2) >> 2;
			int diag = (u[l] + u[r] + d[l] + d[r] + 2) >> 2;
			int hor = (m[l] + m[r] + 1) >> 1, ver = (u[sx] + d[sx] + 1) >> 1;
			switch ((sy & 1) << 1 | (sx & 1)) { /* B, G, R */
			case 0: out[0] = c; out[1] = cross; out[2] = diag; break; /* blue */
			case 1: out[0] = hor; out[1] = c; out[2] = ver; break;    /* green, blue row */
			case 2: out[0] = ver; out[1] = c; out[2] = hor; break;    /* green, red row */
			default: out[0] = diag; out[1] = cross; out[2] = c;       /* red */
			}
		}
	}

	FILE *f = fopen(path, "wb");
	bool ok = f && ui_jpeg_write(f, px, w, h, (long)w * 4, JPEG_QUALITY, false);
	if (f && fclose(f) != 0)
		ok = false;
	free(px);
	if (ok) {
		printf("camera: saved %s (%dx%d)\n", path, w, h);
	} else {
		perror("camera: photo");
		unlink(path);
	}
}

/* Back from the raw stream to the preview, and to continuous autofocus if it
 * was on */
static void still_end(void)
{
	cam.still_skip = -1;
	stream_stop();
	const char *error = stream_start(false);
	if (error) {
		printf("camera: %s\n", error);
		lv_label_set_text_static(cam.status, _(error));
		lv_obj_set_hidden(cam.status, false);
	} else if (cam.sensor_info->focus && cam.focus_continuous) {
		sensor_set(V4L2_CID_FOCUS_AUTO, 1);
	}
}

/* Raw frames: drop the first ones (exposure), save the next, back to the preview */
static void still_frame(void)
{
	struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
	while (ioctl(cam.video, VIDIOC_DQBUF, &b) == 0) {
		if (cam.still_skip > 0 || (b.flags & V4L2_BUF_FLAG_ERROR)) {
			cam.still_skip -= cam.still_skip > 0;
			ioctl(cam.video, VIDIOC_QBUF, &b);
			continue;
		}
		uint32_t t = lv_tick_get();
		printf("camera: raw frame after %u ms\n", (unsigned)(t - cam.still_start));
		raw_photo_save(cam.photo, cam.buf[b.index].data);
		printf("camera: photo in %u ms\n", (unsigned)lv_tick_elaps(t));
		cam.photo[0] = '\0';
		still_end();
		return;
	}
	if (lv_tick_elaps(cam.still_start) > STILL_TIMEOUT) {
		printf("camera: no raw frame, photo not taken\n");
		cam.photo[0] = '\0';
		still_end();
	}
}

static void frame_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	if (cam.still_skip >= 0) {
		still_frame();
		return;
	}

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
	gl_convert(cam.buf[newest.index].data, cam.preview_tex, PREVIEW_W, PREVIEW_H, cam.map);
	if (cam.photo[0]) {
		photo_save(cam.photo);
		cam.photo[0] = '\0';
	}
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

static void shutter_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!cam.frames || cam.photo[0])
		return;

	/* Taken from the next frame (frame_cb), or from the raw stream at the still
	 * size (still_frame) */
	mkdir(PHOTOS, 0755);
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(cam.photo, sizeof(cam.photo), PHOTOS "/IMG_%Y%m%d_%H%M%S.jpg", &tm);
	ui_vibrate(UI_VIBRATE_TAP);
	if (cam.sensor_info->still_w) {
		/* Manual focus keeps the lens where it is, also across the stream
		 * restarts; continuous autofocus would start over in the new mode */
		if (cam.sensor_info->focus)
			sensor_set(V4L2_CID_FOCUS_AUTO, 0);
		stream_stop();
		const char *error = stream_start(true);
		if (error) {
			printf("camera: still: %s\n", error);
			cam.photo[0] = '\0';
			still_end();
			return;
		}
		cam.still_skip = STILL_SKIP;
		cam.still_start = lv_tick_get();
	}

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

/* Focus ring: white while focusing, then green (focused) or red, then hidden */
static void ring_hide_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	lv_obj_set_hidden(cam.ring, true);
	cam.focus_timer = NULL;
}

static void focus_poll_cb(lv_timer_t *timer)
{
	int32_t status = sensor_get(V4L2_CID_AUTO_FOCUS_STATUS);
	if (status == V4L2_AUTO_FOCUS_STATUS_BUSY && ++cam.focus_polls < 30)
		return;

	bool reached = status == V4L2_AUTO_FOCUS_STATUS_REACHED;
	lv_obj_set_style_border_color(cam.ring, lv_color_hex(reached ? 0x34c759 : 0xff453a), 0);
	lv_timer_set_cb(timer, ring_hide_cb);
	lv_timer_set_period(timer, 800);
	lv_timer_set_repeat_count(timer, 1);
	lv_timer_reset(timer);
}

/* Tap: focus once on that spot (the sensor's focus zone), then keep the lens there */
static void focus_tap_cb(lv_event_t *e)
{
	lv_obj_t *frame = lv_event_get_target_obj(e);
	lv_point_t p;
	lv_area_t area;
	lv_indev_get_point(lv_indev_active(), &p);
	lv_obj_get_coords(frame, &area);
	int32_t x = p.x - area.x1, y = p.y - area.y1;
	int32_t w = lv_area_get_width(&area), h = lv_area_get_height(&area);
	if (cam.sensor < 0 || !cam.sensor_info->focus || x < 0 || y < 0 || x >= w || y >= h)
		return;

	/* Screen to sensor: through the conversion pass's map (GL y is up, and the
	 * preview is shown flipped) */
	float px = (float)x * PREVIEW_W / w, py = (float)(h - y) * PREVIEW_H / h;
	uint32_t sx = (uint32_t)(cam.map[0] + cam.map[1] * py);
	uint32_t sy = (uint32_t)(cam.map[2] + cam.map[3] * px);
	cam.focus_continuous = false;
	sensor_set(V4L2_CID_FOCUS_AUTO, 0);
	sensor_set(V4L2_CID_USER_BASE | 0x1001, (int32_t)(sx << 16 | (sy ? sy : 1))); /* focus zone */
	sensor_set(V4L2_CID_AUTO_FOCUS_START, 1);

	lv_obj_set_pos(cam.ring, x - lv_obj_get_width(cam.ring) / 2, y - lv_obj_get_height(cam.ring) / 2);
	lv_obj_set_style_border_color(cam.ring, lv_color_white(), 0);
	lv_obj_set_hidden(cam.ring, false);
	if (cam.focus_timer)
		lv_timer_delete(cam.focus_timer);
	cam.focus_polls = 0;
	cam.focus_timer = lv_timer_create(focus_poll_cb, 100, NULL);
}

/* Long press: back to continuous autofocus over the default zones */
static void focus_reset_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (cam.sensor < 0 || !cam.sensor_info->focus)
		return;
	if (cam.focus_timer)
		lv_timer_delete(cam.focus_timer);
	cam.focus_timer = NULL;
	lv_obj_set_hidden(cam.ring, true);
	cam.focus_continuous = true;
	sensor_set(V4L2_CID_USER_BASE | 0x1001, 0);
	sensor_set(V4L2_CID_FOCUS_AUTO, 1);
	ui_vibrate(UI_VIBRATE_TAP);
}

/* Starts the selected camera, or shows why it did not */
static void camera_restart(void)
{
	camera_stop();
	cam.frames = 0;
	const char *error = camera_start();
	if (error) {
		printf("camera: %s\n", error);
		camera_stop();
	}
	lv_label_set_text_static(cam.status, error ? _(error) : _("Starting camera..."));
	lv_obj_set_hidden(cam.status, false);
}

/* Rear <-> front */
static void switch_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (cam.focus_timer)
		lv_timer_delete(cam.focus_timer);
	cam.focus_timer = NULL;
	lv_obj_set_hidden(cam.ring, true);
	cam.sensor_info = cam.sensor_info == &sensors[0] ? &sensors[1] : &sensors[0];
	camera_restart();
}

static void screen_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (cam.timer)
		lv_timer_delete(cam.timer);
	cam.timer = NULL;
	if (cam.focus_timer)
		lv_timer_delete(cam.focus_timer);
	cam.focus_timer = NULL;
	camera_stop();
	gl_deinit();
}

void camera_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(_("Camera"), NULL, &body);
	lv_obj_add_event_cb(scr, screen_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	/* Preview: the portrait frame scaled to the screen width */
	int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
	lv_obj_t *frame = lv_obj_create(body);
	lv_obj_remove_style_all(frame);
	lv_obj_set_size(frame, w, w * PREVIEW_H / PREVIEW_W);
	lv_obj_set_style_bg_color(frame, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);

	cam.status = lv_label_create(frame);
	lv_label_set_text_static(cam.status, _("Starting camera..."));
	lv_obj_set_style_text_color(cam.status, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_center(cam.status);

	/* Shutter: white disc in a ring of the text color, with a gap of the
	 * background's */
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
	lv_obj_set_style_border_color(shutter, lv_color_hex(UI_COLOR_SHADE), 0);
	lv_obj_set_style_border_width(shutter, 6, 0);
	lv_obj_set_style_outline_color(shutter, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_outline_width(shutter, 6, 0);
	lv_obj_set_style_outline_pad(shutter, 0, 0);
	lv_obj_set_style_shadow_width(shutter, 0, 0);
	lv_obj_add_event_cb(shutter, shutter_cb, LV_EVENT_CLICKED, NULL);

	/* Rear / front, right of the shutter */
	lv_obj_t *flip = lv_button_create(controls);
	lv_obj_set_size(flip, 96, 96);
	lv_obj_align(flip, LV_ALIGN_CENTER, 200, 0);
	lv_obj_set_style_radius(flip, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(flip, lv_color_hex(UI_COLOR_BUTTON), 0);
	lv_obj_set_style_bg_color(flip, lv_color_hex(UI_COLOR_BUTTON_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_style_shadow_width(flip, 0, 0);
	lv_obj_add_event_cb(flip, switch_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *icon = lv_label_create(flip);
	lv_label_set_text_static(icon, LV_SYMBOL_REFRESH);
	lv_obj_set_style_text_font(icon, &ui_font_32, 0);
	lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_center(icon);

	if (!gl_init()) {
		printf("camera: no GPU for the preview\n");
		lv_label_set_text_static(cam.status, _("No GPU for the preview"));
		ui_push(scr);
		return;
	}

	cam.preview = lv_3dtexture_create(frame);
	lv_obj_set_size(cam.preview, LV_PCT(100), LV_PCT(100));
	lv_3dtexture_set_src(cam.preview, cam.preview_tex);
	lv_3dtexture_set_flip(cam.preview, false, true);
	lv_obj_move_to_index(cam.preview, 0);

	cam.flash = ui_box(frame);
	lv_obj_set_size(cam.flash, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_color(cam.flash, lv_color_white(), 0);
	lv_obj_set_hidden(cam.flash, true);

	cam.ring = ui_box(frame);
	lv_obj_set_size(cam.ring, 120, 120);
	lv_obj_set_style_radius(cam.ring, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(cam.ring, 4, 0);
	lv_obj_set_hidden(cam.ring, true);

	/* Taps go to the frame, under the preview */
	lv_obj_set_clickable(cam.preview, false);
	lv_obj_set_clickable(frame, true);
	lv_obj_set_scrollable(frame, false);
	lv_obj_add_event_cb(frame, focus_tap_cb, LV_EVENT_SHORT_CLICKED, NULL);
	lv_obj_add_event_cb(frame, focus_reset_cb, LV_EVENT_LONG_PRESSED, NULL);

	camera_restart();
	cam.timer = lv_timer_create(frame_cb, 10, NULL);
	ui_push(scr);
}

#else

void camera_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(_("Camera"), NULL, &body);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *label = lv_label_create(body);
	lv_label_set_text_static(label, _("The camera needs the GPU renderer"));
	ui_push(scr);
}

#endif
