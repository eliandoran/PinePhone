/*
 * The display's scanout for the UI tests (ui_test.h, "Screenshots"): libdrm's calls
 * that screenshot.c makes are wrapped (the Makefile's DRM_WRAPS) and answered for
 * /dev/dri/card0, a file in the sandbox's /dev (ui_test_board()): a CRTC showing a
 * linear XRGB8888 framebuffer of the display, its rows padded, at an offset in the
 * buffer. Mapping it maps the file, where the frame LVGL drew last (ui_test_pixels())
 * is written when phone-ui asks for the framebuffer. ui_test_scanout() makes it one
 * screenshot.c cannot read.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "ui_test.h"

#define CARD "/dev/dri/card0"
#define CRTC_OFF 41 /* a CRTC without a mode, then the one showing FB */
#define CRTC_ON 59
#define FB 77
#define HANDLE 5
#define PITCH_PAD 64
#define OFFSET 4096

static enum ui_test_scanout scanout;

void ui_test_scanout(enum ui_test_scanout s)
{
	scanout = s;
}

static bool is_card(int fd)
{
	char path[64], target[64];
	snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
	ssize_t n = readlink(path, target, sizeof(target) - 1);
	return n > 0 && (target[n] = '\0', strcmp(target, CARD) == 0);
}

UI_TEST_WRAP(drmModeGetResources);
drmModeResPtr __wrap_drmModeGetResources(int fd)
{
	if (!is_card(fd))
		return __real_drmModeGetResources(fd);
	drmModeResPtr res = calloc(1, sizeof(*res));
	res->count_crtcs = 2;
	res->crtcs = calloc(2, sizeof(uint32_t));
	res->crtcs[0] = CRTC_OFF;
	res->crtcs[1] = CRTC_ON;
	return res;
}

UI_TEST_WRAP(drmModeFreeResources);
void __wrap_drmModeFreeResources(drmModeResPtr res)
{
	if (res && res->count_crtcs == 2 && res->crtcs[0] == CRTC_OFF) {
		free(res->crtcs);
		free(res);
	} else {
		__real_drmModeFreeResources(res);
	}
}

UI_TEST_WRAP(drmModeGetCrtc);
drmModeCrtcPtr __wrap_drmModeGetCrtc(int fd, uint32_t id)
{
	if (!is_card(fd))
		return __real_drmModeGetCrtc(fd, id);
	if (id != CRTC_OFF && id != CRTC_ON) {
		errno = ENOENT;
		return NULL;
	}
	drmModeCrtcPtr crtc = calloc(1, sizeof(*crtc));
	crtc->crtc_id = id;
	crtc->mode_valid = id == CRTC_ON;
	crtc->buffer_id = id == CRTC_ON && scanout != UI_TEST_SCANOUT_OFF ? FB : 0;
	return crtc;
}

UI_TEST_WRAP(drmModeFreeCrtc);
void __wrap_drmModeFreeCrtc(drmModeCrtcPtr crtc)
{
	free(crtc); /* libdrm's frees it the same way */
}

UI_TEST_WRAP(drmModeGetFB2);
drmModeFB2Ptr __wrap_drmModeGetFB2(int fd, uint32_t id)
{
	if (!is_card(fd))
		return __real_drmModeGetFB2(fd, id);
	if (id != FB) {
		errno = ENOENT;
		return NULL;
	}
	lv_display_t *disp = lv_display_get_default();
	uint32_t w = (uint32_t)lv_display_get_horizontal_resolution(disp);
	uint32_t h = (uint32_t)lv_display_get_vertical_resolution(disp);
	drmModeFB2Ptr fb = calloc(1, sizeof(*fb));
	*fb = (drmModeFB2){
		.fb_id = FB, .width = w, .height = h, .pixel_format = DRM_FORMAT_XRGB8888,
		.modifier = scanout == UI_TEST_SCANOUT_TILED ? DRM_FORMAT_MOD_ALLWINNER_TILED : DRM_FORMAT_MOD_LINEAR,
		.flags = DRM_MODE_FB_MODIFIERS,
		.handles = { scanout == UI_TEST_SCANOUT_NO_HANDLE ? 0 : HANDLE }, .pitches = { w * 4 + PITCH_PAD },
		.offsets = { OFFSET },
	};

	/* The frame on the display: GL's rows bottom first, B G R X as XRGB8888 */
	const uint8_t *px = ui_test_pixels();
	size_t pitch = fb->pitches[0];
	uint8_t *row = calloc(1, pitch);
	for (uint32_t y = 0; y < h; y++) {
		memcpy(row, px + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
		if (pwrite(fd, row, pitch, (off_t)(OFFSET + y * pitch)) != (ssize_t)pitch)
			perror("fake_drm: " CARD);
	}
	free(row);
	return fb;
}

UI_TEST_WRAP(drmModeFreeFB2);
void __wrap_drmModeFreeFB2(drmModeFB2Ptr fb)
{
	free(fb);
}

UI_TEST_WRAP(drmIoctl);
int __wrap_drmIoctl(int fd, unsigned long request, void *arg)
{
	if (!is_card(fd))
		return __real_drmIoctl(fd, request, arg);
	if (request == DRM_IOCTL_MODE_MAP_DUMB) {
		struct drm_mode_map_dumb *map = arg;
		if (map->handle != HANDLE || scanout == UI_TEST_SCANOUT_UNMAPPABLE) {
			errno = ENOENT;
			return -1;
		}
		map->offset = 0; /* the file from its start */
		return 0;
	}
	errno = EINVAL;
	return -1;
}

UI_TEST_WRAP(drmCloseBufferHandle);
int __wrap_drmCloseBufferHandle(int fd, uint32_t handle)
{
	if (!is_card(fd))
		return __real_drmCloseBufferHandle(fd, handle);
	return handle == HANDLE ? 0 : -1;
}
