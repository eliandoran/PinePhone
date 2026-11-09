/*
 * The PinePhone's cameras for the UI tests (ui_test.h, "Cameras"): the media
 * pipeline camera.c drives, the rear OV5640 and the front GC2145 each linked to the
 * sun6i-csi-bridge, which /dev/video0 (sun6i-csi-capture) streams from. The nodes
 * are files in the sandbox's /dev and /sys; their ioctl() calls (UI tests link with
 * --wrap=ioctl, fake_wpa.c passes the rest here) are answered as the drivers do:
 * formats along the pipeline, links, controls, and frames of one color on the
 * tests' clock at the sensor's rate, written into the buffers phone-ui maps (the
 * file's pages, at each buffer's offset). Streaming fails, as the kernel's
 * pipeline check does, unless one sensor is linked and every pad has the format
 * the video node captures.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/media-bus-format.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ui_test.h"
#include "ui/qrcodegen.h"

#define BUFFERS 8
#define PAGE 4096

enum { BRIDGE, REAR, FRONT, NODES };

/* By entity: its subdevice, its name in sysfs and the media graph, its entity id */
static const struct node {
	const char *dev, *name;
	uint32_t id;
} nodes[NODES] = {
	[BRIDGE] = { "v4l-subdev0", "sun6i-csi-bridge", 1 },
	[REAR] = { "v4l-subdev1", "ov5640 2-004c", 2 },
	[FRONT] = { "v4l-subdev2", "gc2145 2-003c", 3 },
};
#define CAPTURE_ID 4

struct format {
	uint32_t width, height, code;
};

static struct {
	bool present[NODES];
	bool linked[NODES];
	struct format pad[NODES][2]; /* the bridge's two pads, a sensor's one */
	uint32_t scene;
	uint8_t qr[qrcodegen_BUFFER_LEN_MAX]; /* a QR code in front of it, if qr_shown */
	bool qr_shown;
	bool stall;
	int32_t focus_auto, focus_zone, focus_starts;
	int focus_busy;
	bool focus_reached;
	/* The video node */
	uint32_t width, height, pixelformat, stride;
	unsigned count;
	size_t size; /* of a buffer, page aligned */
	unsigned queue[BUFFERS], queued; /* in the order they were queued */
	bool streaming;
	uint64_t start_ms;
	unsigned delivered;
} cam = { .scene = 0x808080, .focus_auto = -1, .focus_zone = -1, .focus_reached = true };

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts); /* the tests' time (ui_test.c's) */
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void file_create(const char *path)
{
	ui_test_write(path, "");
}

void ui_test_camera(bool rear, bool front)
{
	cam.present[BRIDGE] = true;
	cam.present[REAR] = rear;
	cam.present[FRONT] = front;
	char path[128], name[64];
	for (int i = 0; i < NODES; i++) {
		if (!cam.present[i])
			continue;
		snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", nodes[i].dev);
		snprintf(name, sizeof(name), "%s\n", nodes[i].name);
		ui_test_write(path, name);
		snprintf(path, sizeof(path), "/dev/%s", nodes[i].dev);
		file_create(path);
	}
	ui_test_write("/sys/class/video4linux/video0/name", "sun6i-csi-capture\n");
	file_create("/dev/video0");
	file_create("/dev/media0");
	/* As the device tree leaves them: the rear camera linked */
	cam.linked[REAR] = rear;
}

void ui_test_camera_scene(uint32_t rgb)
{
	cam.scene = rgb;
}

bool ui_test_camera_qr(const char *text)
{
	uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
	cam.qr_shown = text && qrcodegen_encodeText(text, temp, cam.qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
	                                            qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
	return cam.qr_shown || !text;
}

void ui_test_camera_stall(bool stall)
{
	cam.stall = stall;
}

void ui_test_camera_focus(int busy_polls, bool reached)
{
	cam.focus_busy = busy_polls;
	cam.focus_reached = reached;
}

int32_t ui_test_camera_control(uint32_t id)
{
	switch (id) {
	case V4L2_CID_FOCUS_AUTO: return cam.focus_auto;
	case V4L2_CID_USER_BASE | 0x1001: return cam.focus_zone;
	case V4L2_CID_AUTO_FOCUS_START: return cam.focus_starts;
	}
	return -1;
}

const char *ui_test_camera_streaming(void)
{
	static char text[64];
	if (!cam.streaming)
		return NULL;
	const char *sensor = cam.linked[REAR] ? "ov5640" : "gc2145";
	snprintf(text, sizeof(text), "%s %ux%u %s", sensor, cam.width, cam.height,
	         cam.pixelformat == V4L2_PIX_FMT_UYVY ? "UYVY" : "BGGR");
	return text;
}

/* Whether phone-ui has the video node open (its file descriptors) */
bool ui_test_camera_open(void)
{
	char path[64], target[64];
	for (int fd = 0; fd < 1024; fd++) {
		snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
		ssize_t n = readlink(path, target, sizeof(target) - 1);
		if (n > 0 && (target[n] = '\0', strcmp(target, "/dev/video0") == 0))
			return true;
	}
	return false;
}

/* Which node fd is: an entity, CAPTURE_ID for the video node, 0 for the media
 * device, -1 for none of them */
static int node_of(int fd)
{
	char path[64], target[64];
	snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
	ssize_t n = readlink(path, target, sizeof(target) - 1);
	if (n <= 0)
		return -1;
	target[n] = '\0';
	if (strcmp(target, "/dev/video0") == 0)
		return CAPTURE_ID;
	if (strcmp(target, "/dev/media0") == 0)
		return 0;
	for (int i = 0; i < NODES; i++)
		if (cam.present[i] && strncmp(target, "/dev/", 5) == 0 && strcmp(target + 5, nodes[i].dev) == 0)
			return 100 + i;
	return -1;
}

static int media_ioctl(unsigned long request, void *arg)
{
	if (request == MEDIA_IOC_ENUM_ENTITIES) {
		struct media_entity_desc *e = arg;
		uint32_t after = e->id & ~MEDIA_ENT_ID_FLAG_NEXT;
		for (uint32_t id = after + 1; id <= CAPTURE_ID; id++) {
			if (id < CAPTURE_ID && !cam.present[id - 1])
				continue;
			memset(e, 0, sizeof(*e));
			e->id = id;
			if (id == CAPTURE_ID) {
				snprintf(e->name, sizeof(e->name), "sun6i-csi-capture");
				e->links = 0;
			} else {
				snprintf(e->name, sizeof(e->name), "%s", nodes[id - 1].name);
				e->links = id - 1 == BRIDGE ? 1 + cam.present[REAR] + cam.present[FRONT] : 1;
			}
			return 0;
		}
		errno = EINVAL;
		return -1;
	}
	if (request == MEDIA_IOC_ENUM_LINKS) {
		struct media_links_enum *l = arg;
		int i = (int)l->entity - 1;
		if (i != REAR && i != FRONT) {
			errno = EINVAL;
			return -1;
		}
		if (l->links) {
			memset(l->links, 0, sizeof(*l->links));
			l->links[0].source = (struct media_pad_desc){ .entity = nodes[i].id, .flags = MEDIA_PAD_FL_SOURCE };
			l->links[0].sink = (struct media_pad_desc){ .entity = nodes[BRIDGE].id, .flags = MEDIA_PAD_FL_SINK };
			l->links[0].flags = cam.linked[i] ? MEDIA_LNK_FL_ENABLED : 0;
		}
		return 0;
	}
	if (request == MEDIA_IOC_SETUP_LINK) {
		struct media_link_desc *link = arg;
		int i = (int)link->source.entity - 1;
		if ((i != REAR && i != FRONT) || !cam.present[i] || link->sink.entity != nodes[BRIDGE].id) {
			errno = EINVAL;
			return -1;
		}
		if (cam.streaming) {
			errno = EBUSY;
			return -1;
		}
		cam.linked[i] = link->flags & MEDIA_LNK_FL_ENABLED;
		return 0;
	}
	errno = ENOTTY;
	return -1;
}

static int subdev_ioctl(int i, unsigned long request, void *arg)
{
	if (request == VIDIOC_SUBDEV_S_FMT) {
		struct v4l2_subdev_format *f = arg;
		if (f->pad > (i == BRIDGE ? 1u : 0u)) {
			errno = EINVAL;
			return -1;
		}
		cam.pad[i][f->pad] = (struct format){ f->format.width, f->format.height, f->format.code };
		return 0;
	}
	if (request == VIDIOC_S_CTRL || request == VIDIOC_G_CTRL) {
		struct v4l2_control *c = arg;
		if (i != REAR) { /* the front camera has no focus */
			errno = EINVAL;
			return -1;
		}
		if (request == VIDIOC_G_CTRL && c->id == V4L2_CID_AUTO_FOCUS_STATUS) {
			if (cam.focus_busy > 0) {
				cam.focus_busy--;
				c->value = V4L2_AUTO_FOCUS_STATUS_BUSY;
			} else {
				c->value = cam.focus_reached ? V4L2_AUTO_FOCUS_STATUS_REACHED : V4L2_AUTO_FOCUS_STATUS_FAILED;
			}
			return 0;
		}
		if (request == VIDIOC_S_CTRL && c->id == V4L2_CID_FOCUS_AUTO) {
			cam.focus_auto = c->value;
			return 0;
		}
		if (request == VIDIOC_S_CTRL && c->id == (V4L2_CID_USER_BASE | 0x1001)) {
			cam.focus_zone = c->value;
			return 0;
		}
		if (request == VIDIOC_S_CTRL && c->id == V4L2_CID_AUTO_FOCUS_START) {
			cam.focus_starts++;
			return 0;
		}
		errno = EINVAL;
		return -1;
	}
	errno = ENOTTY;
	return -1;
}

/* The sensor streaming, if the pipeline is set up as the kernel checks it */
static int pipeline_sensor(void)
{
	int sensor = -1;
	for (int i = REAR; i <= FRONT; i++)
		if (cam.present[i] && cam.linked[i]) {
			if (sensor >= 0)
				return -1;
			sensor = i;
		}
	if (sensor < 0)
		return -1;
	uint32_t code = cam.pixelformat == V4L2_PIX_FMT_UYVY
	                ? (sensor == REAR ? MEDIA_BUS_FMT_UYVY8_2X8 : MEDIA_BUS_FMT_UYVY8_1X16)
	                : MEDIA_BUS_FMT_SBGGR8_1X8;
	const struct format *pads[] = { &cam.pad[sensor][0], &cam.pad[BRIDGE][0], &cam.pad[BRIDGE][1] };
	for (int i = 0; i < 3; i++)
		if (pads[i]->width != cam.width || pads[i]->height != cam.height || pads[i]->code != code)
			return -1;
	return sensor;
}

/* Frames a second: the drivers' rates at these sizes */
static unsigned fps(void)
{
	if (cam.linked[FRONT])
		return 16;
	return cam.width > 1280 ? 15 : 30;
}

/* The QR code on white with its quiet zone, half the frame's height, centered (UYVY) */
static void qr_draw(uint8_t *p)
{
	int n = qrcodegen_getSize(cam.qr), px = (int)cam.height / 2 / (n + 8);
	int size = px * (n + 8), x0 = ((int)cam.width - size) / 2, y0 = ((int)cam.height - size) / 2;
	x0 &= ~1; /* on a U Y V Y group */
	for (int y = 0; y < size; y++)
		for (int x = 0; x < size; x++) {
			uint8_t *pixel = p + (size_t)(y0 + y) * cam.stride + (size_t)(x0 + x) * 2;
			pixel[0] = 128; /* U or V: gray */
			pixel[1] = qrcodegen_getModule(cam.qr, x / px - 4, y / px - 4) ? 20 : 235;
		}
}

/* The scene in the stream's format: UYVY (BT.601, full range) or raw BGGR */
static void frame_fill(uint8_t *p)
{
	int r = (int)(cam.scene >> 16 & 0xff), g = (int)(cam.scene >> 8 & 0xff), b = (int)(cam.scene & 0xff);
	if (cam.pixelformat == V4L2_PIX_FMT_UYVY) {
		double y = 0.299 * r + 0.587 * g + 0.114 * b;
		double u = (b - y) / 1.772 + 128, v = (r - y) / 1.402 + 128;
		uint8_t group[4] = { (uint8_t)(u + 0.5), (uint8_t)(y + 0.5), (uint8_t)(v + 0.5), (uint8_t)(y + 0.5) };
		for (uint32_t row = 0; row < cam.height; row++)
			for (uint32_t x = 0; x < cam.width * 2; x += 4)
				memcpy(p + row * cam.stride + x, group, 4);
		if (cam.qr_shown)
			qr_draw(p);
	} else {
		for (uint32_t row = 0; row < cam.height; row++)
			for (uint32_t x = 0; x < cam.width; x++)
				p[row * cam.stride + x] = (uint8_t)(row & 1 ? (x & 1 ? r : g) : (x & 1 ? g : b));
	}
}

static int video_ioctl(int fd, unsigned long request, void *arg)
{
	switch (request) {
	case VIDIOC_S_FMT: {
		struct v4l2_format *f = arg;
		if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || cam.count) {
			errno = cam.count ? EBUSY : EINVAL;
			return -1;
		}
		if (f->fmt.pix.pixelformat != V4L2_PIX_FMT_SBGGR8)
			f->fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
		cam.pixelformat = f->fmt.pix.pixelformat;
		cam.width = f->fmt.pix.width;
		cam.height = f->fmt.pix.height;
		cam.stride = cam.width * (cam.pixelformat == V4L2_PIX_FMT_UYVY ? 2 : 1);
		f->fmt.pix.bytesperline = cam.stride;
		f->fmt.pix.sizeimage = cam.stride * cam.height;
		return 0;
	}
	case VIDIOC_REQBUFS: {
		struct v4l2_requestbuffers *r = arg;
		if (cam.streaming) {
			errno = EBUSY;
			return -1;
		}
		cam.count = r->count > BUFFERS ? BUFFERS : r->count;
		r->count = cam.count;
		cam.queued = 0;
		cam.size = ((size_t)cam.stride * cam.height + PAGE - 1) / PAGE * PAGE;
		if (ftruncate(fd, (off_t)(cam.size * cam.count)) != 0)
			return -1;
		return 0;
	}
	case VIDIOC_QUERYBUF:
	case VIDIOC_QBUF: {
		struct v4l2_buffer *b = arg;
		if (b->index >= cam.count) {
			errno = EINVAL;
			return -1;
		}
		if (request == VIDIOC_QUERYBUF) {
			b->length = (uint32_t)cam.size;
			b->m.offset = (uint32_t)(b->index * cam.size);
			return 0;
		}
		for (unsigned i = 0; i < cam.queued; i++)
			if (cam.queue[i] == b->index) {
				errno = EINVAL;
				return -1;
			}
		cam.queue[cam.queued++] = b->index;
		return 0;
	}
	case VIDIOC_DQBUF: {
		struct v4l2_buffer *b = arg;
		unsigned due = cam.streaming ? (unsigned)((now_ms() - cam.start_ms) * fps() / 1000) : 0;
		if (!cam.streaming || cam.stall || !cam.queued || cam.delivered >= due) {
			errno = EAGAIN;
			return -1;
		}
		unsigned index = cam.queue[0];
		memmove(cam.queue, cam.queue + 1, --cam.queued * sizeof(cam.queue[0]));
		uint8_t *frame = malloc(cam.size);
		frame_fill(frame);
		ssize_t n = pwrite(fd, frame, (size_t)cam.stride * cam.height, (off_t)(index * cam.size));
		free(frame);
		if (n < 0)
			return -1;
		cam.delivered++;
		b->index = index;
		b->bytesused = cam.stride * cam.height;
		b->flags = V4L2_BUF_FLAG_DONE;
		b->sequence = cam.delivered - 1;
		return 0;
	}
	case VIDIOC_STREAMON:
		if (!cam.count || pipeline_sensor() < 0) {
			errno = EPIPE;
			return -1;
		}
		cam.streaming = true;
		cam.start_ms = now_ms();
		cam.delivered = 0;
		return 0;
	case VIDIOC_STREAMOFF:
		cam.streaming = false;
		cam.queued = 0;
		return 0;
	}
	errno = ENOTTY;
	return -1;
}

/* ioctl() on the camera's nodes: true if fd is one, with its result in *ret */
bool fake_camera_ioctl(int fd, unsigned long request, void *arg, int *ret)
{
	int node = node_of(fd);
	if (node < 0)
		return false;
	if (node == 0)
		*ret = media_ioctl(request, arg);
	else if (node == CAPTURE_ID)
		*ret = video_ioctl(fd, request, arg);
	else
		*ret = subdev_ioctl(node - 100, request, arg);
	return true;
}
