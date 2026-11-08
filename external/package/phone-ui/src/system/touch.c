/*
 * The touchscreen for plugin apps' threads (ui.h: Doom's and Genesis' controls), where
 * LVGL sees one pointer: the multi-touch device (the first with ABS_MT_POSITION_*) opened
 * beside LVGL's, which keeps reading its own. Slots (protocol B) followed through each
 * report, their positions scaled to the display and turned as LVGL turns its own pointer
 * (see turn.c).
 */

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/input.h>

#include "ui.h"

struct ui_touch {
	int fd;
	int max_x, max_y;          /* the panel's */
	int w, h;                  /* the display's, upright */
	int slot;                  /* the one the events are about */
	struct ui_touch_finger fingers[UI_TOUCH_FINGERS]; /* x, y upright on the display */
};

static void lift_all(struct ui_touch *t)
{
	for (int i = 0; i < UI_TOUCH_FINGERS; i++)
		t->fingers[i] = (struct ui_touch_finger){ .id = -1 };
	t->slot = 0;
}

struct ui_touch *ui_touch_open(const char *name)
{
	int fd = -1;
	struct input_absinfo x, y;
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	while (d && fd < 0 && (e = readdir(d))) {
		char path[300];
		if (strncmp(e->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0 && (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &x) != 0 ||
		                ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &y) != 0 || x.maximum <= 0 || y.maximum <= 0)) {
			close(fd);
			fd = -1;
		}
	}
	if (d)
		closedir(d);
	struct ui_touch *t = fd >= 0 ? calloc(1, sizeof(*t)) : NULL;
	if (!t) {
		printf("%s: no touchscreen\n", name);
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	lv_display_t *disp = lv_display_get_default();
	t->fd = fd;
	t->max_x = x.maximum;
	t->max_y = y.maximum;
	t->w = lv_display_get_original_horizontal_resolution(disp);
	t->h = lv_display_get_original_vertical_resolution(disp);
	lift_all(t);
	return t;
}

/* After a report: the fingers turned as the display is */
static void report_turned(struct ui_touch *t, int rotation, ui_touch_report_cb *report, void *user_data)
{
	struct ui_touch_finger turned[UI_TOUCH_FINGERS];
	for (int i = 0; i < UI_TOUCH_FINGERS; i++) {
		const struct ui_touch_finger *f = &t->fingers[i];
		turned[i] = *f;
		switch (rotation) {
		case LV_DISPLAY_ROTATION_90: turned[i].x = f->y; turned[i].y = t->w - 1 - f->x; break;
		case LV_DISPLAY_ROTATION_180: turned[i].x = t->w - 1 - f->x; turned[i].y = t->h - 1 - f->y; break;
		case LV_DISPLAY_ROTATION_270: turned[i].x = t->h - 1 - f->y; turned[i].y = f->x; break;
		default: break;
		}
		t->fingers[i].landed = false;
	}
	report(turned, user_data);
}

void ui_touch_read(struct ui_touch *t, int rotation, ui_touch_report_cb *report, void *user_data)
{
	struct input_event ev;
	while (t && read(t->fd, &ev, sizeof(ev)) == sizeof(ev)) {
		struct ui_touch_finger *f = &t->fingers[t->slot];
		if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			report_turned(t, rotation, report, user_data);
		} else if (ev.type != EV_ABS) {
			continue;
		} else if (ev.code == ABS_MT_SLOT) {
			t->slot = ev.value >= 0 && ev.value < UI_TOUCH_FINGERS ? ev.value : 0;
		} else if (ev.code == ABS_MT_TRACKING_ID) {
			f->landed = ev.value >= 0 && f->id < 0;
			f->id = ev.value;
		} else if (ev.code == ABS_MT_POSITION_X) {
			f->x = (int)((long long)ev.value * t->w / (t->max_x + 1));
		} else if (ev.code == ABS_MT_POSITION_Y) {
			f->y = (int)((long long)ev.value * t->h / (t->max_y + 1));
		}
	}
}

void ui_touch_drop(struct ui_touch *t)
{
	struct input_event ev;
	while (t && read(t->fd, &ev, sizeof(ev)) == sizeof(ev))
		;
	if (t)
		lift_all(t);
}

void ui_touch_close(struct ui_touch *t)
{
	if (!t)
		return;
	close(t->fd);
	free(t);
}
