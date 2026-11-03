/*
 * Screenshots: Power and Volume Down together (volume.c) save what the display
 * shows as a JPEG in /var/lib/photos, seen in the gallery; `phone-ui --screenshot
 * [file|-]` does the same from a shell, also over an external app (`just screenshot`).
 *
 * The pixels come from the framebuffer the CRTC scans out, not from LVGL: a second
 * DRM file descriptor gets its buffer (handles need DRM master or CAP_SYS_ADMIN,
 * root here) and maps it like a dumb buffer. Mesa allocates the scanout buffers on
 * the display driver (XRGB8888, linear), where they can be mapped like that.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "ui.h"

#define PHOTOS "/var/lib/photos"
#define QUALITY 95
#define DELAY_MS 100 /* for the volume slider to go off the screen */

/* The displayed frame as BGRX pixels (malloc'd), NULL if there is none */
static uint8_t *frame_read(int *w, int *h)
{
	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("screenshot: /dev/dri/card0");
		return NULL;
	}
	uint32_t fb_id = 0;
	drmModeRes *res = drmModeGetResources(fd);
	for (int i = 0; res && i < res->count_crtcs && !fb_id; i++) {
		drmModeCrtc *crtc = drmModeGetCrtc(fd, res->crtcs[i]);
		if (crtc && crtc->mode_valid)
			fb_id = crtc->buffer_id;
		drmModeFreeCrtc(crtc);
	}
	drmModeFreeResources(res);

	uint8_t *pixels = NULL;
	drmModeFB2 *fb = fb_id ? drmModeGetFB2(fd, fb_id) : NULL;
	if (!fb)
		fprintf(stderr, "screenshot: nothing on the display\n");
	else if (!fb->handles[0])
		fprintf(stderr, "screenshot: no access to the framebuffer (not root?)\n");
	else if ((fb->pixel_format != DRM_FORMAT_XRGB8888 && fb->pixel_format != DRM_FORMAT_ARGB8888) ||
	         ((fb->flags & DRM_MODE_FB_MODIFIERS) && fb->modifier != DRM_FORMAT_MOD_LINEAR))
		fprintf(stderr, "screenshot: unsupported framebuffer format %.4s\n", (char *)&fb->pixel_format);
	else {
		struct drm_mode_map_dumb map = { .handle = fb->handles[0] };
		size_t size = fb->offsets[0] + (size_t)fb->pitches[0] * fb->height;
		void *mem = MAP_FAILED;
		if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0)
			mem = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, map.offset);
		if (mem == MAP_FAILED)
			fprintf(stderr, "screenshot: cannot map the framebuffer: %s\n", strerror(errno));
		else {
			/* Copied first: the mapping is uncached, row reads are the fast way */
			size_t row = (size_t)fb->width * 4;
			pixels = malloc(row * fb->height);
			for (uint32_t y = 0; pixels && y < fb->height; y++)
				memcpy(pixels + y * row, (uint8_t *)mem + fb->offsets[0] + (size_t)y * fb->pitches[0], row);
			*w = fb->width;
			*h = fb->height;
			munmap(mem, size);
		}
	}
	if (fb) {
		for (int i = 0; i < 4; i++)
			if (fb->handles[i] && (i == 0 || fb->handles[i] != fb->handles[0]))
				drmCloseBufferHandle(fd, fb->handles[i]);
		drmModeFreeFB2(fb);
	}
	close(fd);
	return pixels;
}

bool ui_screenshot_save(const char *path)
{
	char name[sizeof(PHOTOS) + 32];
	if (!path) {
		mkdir(PHOTOS, 0755);
		time_t now = time(NULL);
		struct tm tm;
		localtime_r(&now, &tm);
		strftime(name, sizeof(name), PHOTOS "/SCR_%Y%m%d_%H%M%S.jpg", &tm);
		path = name;
	}
	int w, h;
	uint8_t *pixels = frame_read(&w, &h);
	if (!pixels)
		return false;

	bool out = !strcmp(path, "-");
	FILE *f = out ? stdout : fopen(path, "wb");
	bool ok = f && ui_jpeg_write(f, pixels, w, h, (long)w * 4, QUALITY, true);
	free(pixels);
	if (f && !out && fclose(f) != 0)
		ok = false;
	if (!ok) {
		fprintf(stderr, "screenshot: cannot write %s\n", path);
		if (!out)
			unlink(path);
		return false;
	}
	if (!out)
		printf("screenshot: saved %s (%dx%d)\n", path, w, h);
	return true;
}

/* From the keys: a white flash over the screen once saved */

static void flash_opa_cb(void *obj, int32_t v)
{
	lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0);
}

static void flash_done_cb(lv_anim_t *a)
{
	lv_obj_delete(a->var);
}

static void take_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	if (!ui_screenshot_save(NULL))
		return;
	ui_vibrate(30);
	lv_obj_t *flash = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(flash);
	lv_obj_set_size(flash, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_color(flash, lv_color_white(), 0);
	lv_obj_set_clickable(flash, false);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, flash);
	lv_anim_set_values(&a, LV_OPA_70, LV_OPA_TRANSP);
	lv_anim_set_duration(&a, 300);
	lv_anim_set_exec_cb(&a, flash_opa_cb);
	lv_anim_set_completed_cb(&a, flash_done_cb);
	lv_anim_start(&a);
}

void ui_screenshot(void)
{
	lv_timer_t *t = lv_timer_create(take_cb, DELAY_MS, NULL);
	lv_timer_set_repeat_count(t, 1);
}
