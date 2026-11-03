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
 * Photos go to /var/lib/photos as JPEG, seen in the gallery.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/media-bus-format.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <jpeglib.h>

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

/*
 * The sensors, by their subdevice name, and the frame size they stream at
 * (landscape). Both are mounted rotated: sensor x runs along the preview's
 * height and sensor y along its width, dir_x and dir_y say in which direction
 * (+1: the same as GL coordinates, y up). app_ae: the exposure is set by the
 * app (ae_update()), not by the sensor.
 */
static const struct sensor {
	const char *name;
	int width, height;
	int dir_x, dir_y;
	bool focus, app_ae;
} sensors[] = {
	/* rear: 1280x720 at 30 fps, cropped to the preview at one sensor pixel per
	 * screen pixel (1024x768 and larger sizes deliver no frames yet) */
	{ "ov5640", 1280, 720, -1, -1, true, false },
	/* front: its full 1600x1200, at 15 fps as at any size, scaled down. Its own
	 * auto exposure stops at 40 ms with anti-flicker and then runs every gain to
	 * the top, whatever the picture: 16x and a white face under room light */
	{ "gc2145", 1600, 1200, 1, -1, false, true },
};

/* App exposure: the mean luma it aims at (centre weighted), the gain it stops at */
#define AE_TARGET 105
#define AE_GAIN_MAX 8.0f

static struct camera {
	const struct sensor *sensor_info;
	int video;
	int sensor; /* the sensor's subdevice, for the focus controls */
	struct { void *data; size_t size; } buf[BUFFERS];
	unsigned buffers;
	bool streaming;
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
	/* app exposure: exposure time x gain in rows at unity gain, what is set */
	float ae_value;
	int ae_rows_max, ae_rows, ae_again, ae_dgain;
} cam = { .sensor_info = &sensors[0], .video = -1, .sensor = -1, .ae_value = 1000 };

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
			.width = (uint32_t)cam.sensor_info->width,
			.height = (uint32_t)cam.sensor_info->height,
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

/*
 * App exposure (sensors with app_ae): the sensor in manual exposure, and
 * cam.ae_value, the exposure time times the gain, steered from the frames. The
 * time goes first, up to the whole frame, then the global gain (the one the
 * vendor code drives), then the digital one: gain is what makes the noise.
 */
static void ae_apply(void)
{
	float value = cam.ae_value;
	if (value > cam.ae_rows_max * AE_GAIN_MAX)
		value = cam.ae_value = cam.ae_rows_max * AE_GAIN_MAX;
	if (value < 1)
		value = cam.ae_value = 1;

	int rows = value < cam.ae_rows_max ? (int)value : cam.ae_rows_max;
	float gain = value / rows;
	int again = (int)(64 * gain); /* unity 64, up to 4x */
	if (again > 255)
		again = 255;
	int dgain = (int)(32 * gain * 64 / again); /* unity 32 */
	if (dgain > 255)
		dgain = 255;

	if (rows != cam.ae_rows)
		sensor_set(V4L2_CID_EXPOSURE, rows);
	if (again != cam.ae_again)
		sensor_set(V4L2_CID_ANALOGUE_GAIN, again);
	if (dgain != cam.ae_dgain)
		sensor_set(V4L2_CID_DIGITAL_GAIN, dgain);
	cam.ae_rows = rows;
	cam.ae_again = again;
	cam.ae_dgain = dgain;
}

/* From a frame: its mean luma over a grid of samples, the middle counting
 * twice; moved towards AE_TARGET, a bit more than proportionally since the
 * output is gamma encoded (half the light is not half the luma) */
static void ae_update(const uint8_t *uyvy)
{
	const struct sensor *s = cam.sensor_info;
	unsigned sum = 0, weight = 0;
	for (int gy = 0; gy < 24; gy++) {
		for (int gx = 0; gx < 32; gx++) {
			int x = s->width / 10 + (2 * gx + 1) * s->width * 8 / 10 / 64;
			int y = s->height / 10 + (2 * gy + 1) * s->height * 8 / 10 / 48;
			unsigned w = gx >= 8 && gx < 24 && gy >= 6 && gy < 18 ? 2 : 1;
			sum += w * uyvy[((size_t)y * s->width + (x & ~1)) * 2 + 1];
			weight += w;
		}
	}
	float mean = (float)sum / weight;
	float ratio = AE_TARGET / (mean < 1 ? 1 : mean);
	if (ratio > 0.92f && ratio < 1.08f)
		return;
	float step = powf(ratio, 1.5f);
	cam.ae_value *= step < 0.25f ? 0.25f : step > 4 ? 4 : step;
	ae_apply();
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

static const char *camera_start(void)
{
	const char *name = cam.sensor_info->name;
	map_compute(cam.sensor_info);
	if (!media_select(name) || !subdev_set_format(name, 0) ||
	    !subdev_set_format("sun6i-csi-bridge", 0) ||
	    !subdev_set_format("sun6i-csi-bridge", 1))
		return "Camera not found";

	cam.video = open_by_name("video", "sun6i-csi-capture");
	if (cam.video < 0)
		return "Camera not found";

	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	fmt.fmt.pix.width = (uint32_t)cam.sensor_info->width;
	fmt.fmt.pix.height = (uint32_t)cam.sensor_info->height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (ioctl(cam.video, VIDIOC_S_FMT, &fmt) < 0 ||
	    fmt.fmt.pix.width != (uint32_t)cam.sensor_info->width ||
	    fmt.fmt.pix.height != (uint32_t)cam.sensor_info->height)
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

	cam.sensor = open_by_name("v4l-subdev", name);
	if (cam.sensor_info->app_ae) {
		/* No anti-flicker (taken when the mode is programmed, at stream on):
		 * the exposure can then take the whole frame. LED light does not
		 * flicker at the mains rate. */
		sensor_set(V4L2_CID_POWER_LINE_FREQUENCY, V4L2_CID_POWER_LINE_FREQUENCY_DISABLED);
		sensor_set(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
		/* Its edge enhancement doubles the noise at the default strength (4)
		 * and draws it out into dashes along the rows; the preview is scaled
		 * down and does not need it */
		sensor_set(V4L2_CID_SHARPNESS, 0);
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(cam.video, VIDIOC_STREAMON, &type) < 0)
		return "Camera did not start";
	cam.streaming = true;

	/* Focus and exposure controls only work while streaming (the sensor is
	 * powered) */
	if (cam.sensor_info->focus)
		sensor_set(V4L2_CID_FOCUS_AUTO, 1);
	if (cam.sensor_info->app_ae) {
		struct v4l2_queryctrl q = { .id = V4L2_CID_EXPOSURE };
		cam.ae_rows_max = cam.sensor >= 0 && ioctl(cam.sensor, VIDIOC_QUERYCTRL, &q) == 0 ?
		                  q.maximum : 1000;
		cam.ae_rows = cam.ae_again = cam.ae_dgain = 0;
		ae_apply();
	}
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

struct jpeg_error {
	struct jpeg_error_mgr mgr;
	jmp_buf jmp;
};

/* libjpeg's default exits the program (a full disk, for one) */
static void jpeg_error_exit(j_common_ptr cinfo)
{
	char text[JMSG_LENGTH_MAX];
	cinfo->err->format_message(cinfo, text);
	printf("camera: photo: %s\n", text);
	longjmp(((struct jpeg_error *)cinfo->err)->jmp, 1);
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
	struct jpeg_compress_struct cinfo;
	struct jpeg_error err;
	cinfo.err = jpeg_std_error(&err.mgr);
	err.mgr.error_exit = jpeg_error_exit;
	if (setjmp(err.jmp)) {
		jpeg_destroy_compress(&cinfo);
		fclose(f);
		return false;
	}
	jpeg_create_compress(&cinfo);
	jpeg_stdio_dest(&cinfo, f);
	cinfo.image_width = w;
	cinfo.image_height = h;
	cinfo.in_color_space = JCS_EXT_BGRX;
	cinfo.input_components = 4;
	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, JPEG_QUALITY, TRUE);
	jpeg_start_compress(&cinfo, TRUE);
	while (cinfo.next_scanline < cinfo.image_height) {
		JSAMPROW row = (JSAMPROW)px + (size_t)(h - 1 - cinfo.next_scanline) * w * 4;
		jpeg_write_scanlines(&cinfo, &row, 1);
	}
	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);
	if (fclose(f) != 0) {
		perror("camera: photo");
		return false;
	}
	printf("camera: saved %s (%dx%d)\n", path, w, h);
	return true;
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
	gl_convert(cam.buf[newest.index].data, cam.preview_tex, PREVIEW_W, PREVIEW_H, cam.map);
	if (cam.photo[0]) {
		photo_save(cam.photo);
		cam.photo[0] = '\0';
	}
	/* Every third frame: a new exposure takes a frame or two to show */
	if (cam.sensor_info->app_ae && cam.frames % 3 == 0)
		ae_update(cam.buf[newest.index].data);
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
	if (!cam.frames)
		return;

	/* Taken from the next frame (frame_cb) */
	mkdir(PHOTOS, 0755);
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(cam.photo, sizeof(cam.photo), PHOTOS "/IMG_%Y%m%d_%H%M%S.jpg", &tm);
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
	sensor_set(V4L2_CID_USER_BASE | 0x1001, 0);
	sensor_set(V4L2_CID_FOCUS_AUTO, 1);
	ui_vibrate(20);
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
	lv_label_set_text_static(cam.status, error ? error : "Starting camera...");
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
	lv_obj_t *scr = ui_app_screen_create("Camera", NULL, &body);
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
	lv_label_set_text_static(cam.status, "Starting camera...");
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
		lv_label_set_text_static(cam.status, "No GPU for the preview");
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
	lv_obj_set_clickable(cam.flash, false);

	cam.ring = lv_obj_create(frame);
	lv_obj_remove_style_all(cam.ring);
	lv_obj_set_size(cam.ring, 120, 120);
	lv_obj_set_style_radius(cam.ring, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(cam.ring, 4, 0);
	lv_obj_set_hidden(cam.ring, true);
	lv_obj_set_clickable(cam.ring, false);

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
	lv_obj_t *scr = ui_app_screen_create("Camera", NULL, &body);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *label = lv_label_create(body);
	lv_label_set_text_static(label, "The camera needs the GPU renderer");
	ui_push(scr);
}

#endif
