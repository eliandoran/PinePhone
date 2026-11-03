/*
 * Hello: the smallest external app, and a template for drawing without phone-ui.
 * Fills the screen with a color through KMS (a dumb buffer, plain ioctls, no
 * libdrm); every tap shows the next color, the last one exits. Holding Power
 * ends it too (phone-ui does that).
 *
 * phone-ui has dropped DRM master before starting it: opening the card makes it
 * master, and closing it (exiting) gives the display back.
 */

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <linux/input.h>

static const uint32_t colors[] = { 0xff453a, 0xff9f0a, 0x30d158, 0x0a84ff, 0xbf5af2 };
#define NCOLORS (sizeof(colors) / sizeof(colors[0]))

static int drm_fd;

static void die(const char *what)
{
	perror(what);
	exit(EXIT_FAILURE);
}

static void drm_ioctl(unsigned long request, void *arg, const char *what)
{
	if (ioctl(drm_fd, request, arg) < 0)
		die(what);
}

/* The first touchscreen (multitouch X axis), -1 if none */
static int touch_open(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		char path[300];
		if (strncmp(e->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		unsigned long abs[ABS_CNT / (8 * sizeof(long)) + 1] = { 0 };
		if (fd >= 0 && ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) >= 0 &&
		    abs[ABS_MT_POSITION_X / (8 * sizeof(long))] >> (ABS_MT_POSITION_X % (8 * sizeof(long))) & 1) {
			closedir(d);
			return fd;
		}
		if (fd >= 0)
			close(fd);
	}
	if (d)
		closedir(d);
	return -1;
}

int main(void)
{
	drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm_fd < 0)
		die("/dev/dri/card0");

	/* The first connected connector, its preferred (first) mode, its CRTC */
	struct drm_mode_card_res res = { 0 };
	drm_ioctl(DRM_IOCTL_MODE_GETRESOURCES, &res, "resources");
	uint32_t *conn_ids = calloc(res.count_connectors, sizeof(uint32_t));
	uint32_t *crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
	uint32_t *enc_ids = calloc(res.count_encoders, sizeof(uint32_t));
	uint32_t *fb_ids = calloc(res.count_fbs + 1, sizeof(uint32_t));
	res.connector_id_ptr = (uintptr_t)conn_ids;
	res.crtc_id_ptr = (uintptr_t)crtc_ids;
	res.encoder_id_ptr = (uintptr_t)enc_ids;
	res.fb_id_ptr = (uintptr_t)fb_ids;
	drm_ioctl(DRM_IOCTL_MODE_GETRESOURCES, &res, "resources");

	struct drm_mode_get_connector conn = { 0 };
	struct drm_mode_modeinfo mode = { 0 };
	for (uint32_t i = 0; i < res.count_connectors && !mode.hdisplay; i++) {
		memset(&conn, 0, sizeof(conn));
		conn.connector_id = conn_ids[i];
		drm_ioctl(DRM_IOCTL_MODE_GETCONNECTOR, &conn, "connector");
		if (conn.connection != 1 || !conn.count_modes) /* 1: connected */
			continue;
		struct drm_mode_modeinfo *modes = calloc(conn.count_modes, sizeof(*modes));
		conn.modes_ptr = (uintptr_t)modes;
		conn.count_props = conn.count_encoders = 0;
		drm_ioctl(DRM_IOCTL_MODE_GETCONNECTOR, &conn, "connector");
		mode = modes[0];
		free(modes);
	}
	if (!mode.hdisplay) {
		fprintf(stderr, "hello: no connected display\n");
		return EXIT_FAILURE;
	}
	struct drm_mode_get_encoder enc = { .encoder_id = conn.encoder_id };
	uint32_t crtc_id = crtc_ids[0];
	if (enc.encoder_id && ioctl(drm_fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0 && enc.crtc_id)
		crtc_id = enc.crtc_id;

	/* A dumb buffer, mapped */
	struct drm_mode_create_dumb dumb = { .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32 };
	drm_ioctl(DRM_IOCTL_MODE_CREATE_DUMB, &dumb, "dumb buffer");
	struct drm_mode_fb_cmd fb = { .width = dumb.width, .height = dumb.height, .pitch = dumb.pitch,
	                              .bpp = 32, .depth = 24, .handle = dumb.handle };
	drm_ioctl(DRM_IOCTL_MODE_ADDFB, &fb, "framebuffer");
	struct drm_mode_map_dumb map = { .handle = dumb.handle };
	drm_ioctl(DRM_IOCTL_MODE_MAP_DUMB, &map, "map");
	uint32_t *pixels = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, map.offset);
	if (pixels == MAP_FAILED)
		die("mmap");

	int touch = touch_open();
	printf("hello: %ux%u, touchscreen %s\n", mode.hdisplay, mode.vdisplay, touch >= 0 ? "found" : "missing");

	for (unsigned c = 0; c < NCOLORS; c++) {
		/* The color, with a white bar growing at the bottom: taps left */
		for (uint32_t y = 0; y < dumb.height; y++)
			for (uint32_t x = 0; x < dumb.width; x++)
				pixels[y * dumb.pitch / 4 + x] =
					y > dumb.height - 120 && x < dumb.width * (c + 1) / NCOLORS ? 0xffffff : colors[c];
		struct drm_mode_crtc crtc = { .crtc_id = crtc_id, .fb_id = fb.fb_id,
		                              .set_connectors_ptr = (uintptr_t)&conn.connector_id,
		                              .count_connectors = 1, .mode = mode, .mode_valid = 1 };
		drm_ioctl(DRM_IOCTL_MODE_SETCRTC, &crtc, "set CRTC");

		/* Until a finger lifts */
		struct input_event ev;
		while (touch >= 0 && read(touch, &ev, sizeof(ev)) == sizeof(ev))
			if (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value == 0)
				break;
		if (touch < 0)
			sleep(2);
	}
	printf("hello: done\n");
	return EXIT_SUCCESS;
}
