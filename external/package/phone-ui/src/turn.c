/*
 * Turning sideways: screens that ask to (ui_screen_turns()) turn with the phone.
 * While one is on top, the accelerometer is read a few times a second and the
 * display rotated with lv_display_set_rotation(): LVGL lays the screens out again
 * at 1440 x 720 and sends LV_EVENT_RESOLUTION_CHANGED to the display (ui.c makes
 * its wallpapers and transitions again for the new size; screens lay themselves
 * out again), the OpenGL ES driver turns the display texture as it draws it, and
 * touches are turned back. Any other screen is upright: the display turns back
 * before it is shown (ui_turn_for(), from ui.c's transitions).
 *
 * The accelerometer (MPU6050) is read through sysfs: a few reads a second need no
 * buffer (Compass uses one, but it is not open at the same time).
 */

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ui.h"

#define TURN_MS 300  /* accelerometer reads, while a screen that turns is on top */
#define TURN_READS 2 /* a new orientation is taken once read this many times */
#define TURNERS 16   /* screens that turn at a time */

/* The screens that turn, until deleted */
static struct {
	lv_obj_t *scr;
	bool fullscreen;
} turners[TURNERS];

static struct {
	int fd[3];     /* in_accel_{x,y,z}_raw, -1: none */
	float m[2][3]; /* mount matrix rows: the phone's x, y */
	bool tried;
	lv_display_rotation_t next;
	int reads;
	lv_timer_t *timer;
} turn = { .fd = { -1, -1, -1 } };

static void accel_open(void)
{
	static const char axis[3] = { 'x', 'y', 'z' };
	char path[300];
	turn.tried = true;
	DIR *d = opendir("/sys/bus/iio/devices");
	struct dirent *e;
	while (d && turn.fd[0] < 0 && (e = readdir(d))) {
		if (strncmp(e->d_name, "iio:device", 10) != 0)
			continue;
		for (int i = 0; i < 3; i++) {
			snprintf(path, sizeof(path), "/sys/bus/iio/devices/%s/in_accel_%c_raw", e->d_name, axis[i]);
			turn.fd[i] = open(path, O_RDONLY | O_CLOEXEC);
		}
		if (turn.fd[0] < 0 || turn.fd[1] < 0 || turn.fd[2] < 0) {
			for (int i = 0; i < 3; i++) {
				if (turn.fd[i] >= 0)
					close(turn.fd[i]);
				turn.fd[i] = -1;
			}
			continue;
		}
		float m[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
		snprintf(path, sizeof(path), "/sys/bus/iio/devices/%s/in_accel_mount_matrix", e->d_name);
		FILE *f = fopen(path, "r");
		if (f) {
			float r[9];
			if (fscanf(f, "%f, %f, %f; %f, %f, %f; %f, %f, %f", &r[0], &r[1], &r[2], &r[3], &r[4],
			           &r[5], &r[6], &r[7], &r[8]) == 9)
				memcpy(m, r, sizeof(m));
			fclose(f);
		}
		for (int i = 0; i < 6; i++)
			turn.m[i / 3][i % 3] = m[i];
	}
	if (d)
		closedir(d);
	if (turn.fd[0] < 0)
		printf("phone-ui: no accelerometer, screens stay upright\n");
}

/* The rotation gravity shows; `current` while the phone lies flat, is upside down
 * or between two */
static lv_display_rotation_t accel_rotation(lv_display_rotation_t current)
{
	float raw[3];
	for (int i = 0; i < 3; i++) {
		char buf[32];
		ssize_t n = turn.fd[i] >= 0 ? pread(turn.fd[i], buf, sizeof(buf) - 1, 0) : -1;
		if (n <= 0)
			return current;
		buf[n] = '\0';
		raw[i] = strtof(buf, NULL);
	}
	/* Upward in the phone's axes (x to the right, y to the top): the mount matrix
	 * gives both the other way around (tested) */
	float x = 0, y = 0;
	for (int i = 0; i < 3; i++) {
		x -= turn.m[0][i] * raw[i];
		y -= turn.m[1][i] * raw[i];
	}
	float g2 = raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2];
	if (x * x + y * y < g2 * 0.25f)
		return current; /* within 30 degrees of flat */
	if (y > fabsf(x) * 1.3f)
		return LV_DISPLAY_ROTATION_0;
	if (x > fabsf(y) * 1.3f)
		return LV_DISPLAY_ROTATION_90; /* the right edge up: the top to the left */
	if (-x > fabsf(y) * 1.3f)
		return LV_DISPLAY_ROTATION_270;
	return current;
}

static void rotation_set(lv_display_rotation_t r)
{
	lv_display_t *disp = lv_display_get_default();
	if (lv_display_get_rotation(disp) == r)
		return;
	printf("phone-ui: display turned to %d degrees\n", r * 90);
	lv_display_set_rotation(disp, r);
	/* The OpenGL ES driver turns the display texture counterclockwise, the other
	 * way from what LVGL turns touches back for by default */
	for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i))
		if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER)
			lv_indev_set_ccw(i);
	turn.reads = 0;
	ui_chrome_update();
}

bool ui_sideways(void)
{
	return lv_display_get_rotation(lv_display_get_default()) != LV_DISPLAY_ROTATION_0;
}

void ui_upright(void)
{
	rotation_set(LV_DISPLAY_ROTATION_0);
}

static int turner_find(lv_obj_t *scr)
{
	for (int i = 0; scr && i < TURNERS; i++)
		if (turners[i].scr == scr)
			return i;
	return -1;
}

bool ui_screen_fullscreen(lv_obj_t *scr)
{
	int i = turner_find(scr);
	return i >= 0 && turners[i].fullscreen;
}

void ui_turn_for(lv_obj_t *to)
{
	if (turner_find(to) < 0)
		ui_upright();
}

static void turn_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	lv_obj_t *top = ui_current();
	if (turner_find(top) < 0 || ui_locked()) {
		ui_upright(); /* shown some other way than a transition */
		return;
	}
	/* Not during a transition (its textures are the screen's size) */
	if (lv_screen_active() != top || lv_anim_count_running())
		return;
	if (!turn.tried)
		accel_open();
	if (turn.fd[0] < 0)
		return;
	lv_display_rotation_t current = lv_display_get_rotation(lv_display_get_default());
	lv_display_rotation_t r = accel_rotation(current);
	if (r == current) {
		turn.reads = 0;
		return;
	}
	if (r != turn.next) {
		turn.next = r;
		turn.reads = 0;
	}
	if (++turn.reads >= TURN_READS)
		rotation_set(r);
}

static void turner_delete_cb(lv_event_t *e)
{
	int i = turner_find(lv_event_get_target(e));
	if (i >= 0)
		turners[i].scr = NULL;
}

void ui_screen_turns(lv_obj_t *scr, bool fullscreen)
{
	int i = turner_find(scr);
	if (i < 0) {
		for (i = 0; i < TURNERS && turners[i].scr; i++)
			;
		if (i == TURNERS) {
			printf("phone-ui: more than %d screens turning\n", TURNERS);
			return;
		}
		turners[i].scr = scr;
		lv_obj_add_event_cb(scr, turner_delete_cb, LV_EVENT_DELETE, NULL);
	}
	turners[i].fullscreen = fullscreen;
	if (!turn.timer)
		turn.timer = lv_timer_create(turn_cb, TURN_MS, NULL);
}
