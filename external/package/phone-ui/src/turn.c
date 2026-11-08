/*
 * Turning sideways: screens that ask to (ui_screen_turns()) turn with the phone.
 * While one is on top, the accelerometer is read a few times a second and the
 * display rotated with lv_display_set_rotation(): LVGL lays the screens out again
 * at 1440 x 720 and sends LV_EVENT_RESOLUTION_CHANGED to the display (ui.c has
 * its wallpapers and transitions made again for the new size; screens lay themselves
 * out again), the OpenGL ES driver turns the display texture as it draws it, and
 * touches are turned back. Any other screen is upright: the display turns back
 * before it is shown (ui_turn_for(), from transition.c).
 * Rotation lock: screens that turn stay as the display was when it was locked,
 * the accelerometer unread.
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
	struct ui_accel accel; /* the device, its mount matrix and scale; raw values when read */
	bool tried;
	lv_display_rotation_t next;
	int reads;
	lv_timer_t *timer;
	lv_display_rotation_t dropped; /* by ui_upright(), at `dropped_at`: see ui_turn_for() */
	uint32_t dropped_at;
	int lock; /* the rotation held by the rotation lock, -1: none; -2: not read yet */
} turn = { .fd = { -1, -1, -1 }, .lock = -2 };

/* The first value of an IIO attribute, `fallback` if there is none */
static float attr_float(const char *dev, const char *attr, float fallback)
{
	char path[300], buf[32];
	snprintf(path, sizeof(path), "/sys/bus/iio/devices/%s/%s", dev, attr);
	return ui_read_line(path, buf, sizeof(buf)) ? strtof(buf, NULL) : fallback;
}

static void accel_open(void)
{
	static const char axis[3] = { 'x', 'y', 'z' };
	char path[300];
	bool first = !turn.tried;
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
		struct ui_accel *a = &turn.accel;
		snprintf(a->dev, sizeof(a->dev), "%s", e->d_name);
		snprintf(path, sizeof(path), "/sys/bus/iio/devices/%s/name", e->d_name);
		if (!ui_read_line(path, a->name, sizeof(a->name)))
			a->name[0] = '\0';
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
		for (int i = 0; i < 9; i++)
			a->m[i / 3][i % 3] = m[i];
		a->scale = attr_float(e->d_name, "in_accel_scale", 0);
	}
	if (d)
		closedir(d);
	if (turn.fd[0] < 0 && first)
		printf("phone-ui: no accelerometer, screens stay upright\n");
}

static bool accel_read(void)
{
	for (int i = 0; i < 3; i++) {
		char buf[32];
		ssize_t n = turn.fd[i] >= 0 ? pread(turn.fd[i], buf, sizeof(buf) - 1, 0) : -1;
		if (n <= 0)
			return false;
		buf[n] = '\0';
		turn.accel.raw[i] = strtof(buf, NULL);
	}
	return true;
}

/* The rotation gravity shows in the raw values read; -1 while the phone lies flat,
 * is upside down or between two */
static int accel_rotation(void)
{
	const float *raw = turn.accel.raw;
	/* Upward in the phone's axes (x to the right, y to the top): the MPU6050 reads
	 * +1 g upward, the mount matrix turns it to the phone's axes (mainline's had x
	 * and y the other way around: kernel patch 0016) */
	float x = 0, y = 0;
	for (int i = 0; i < 3; i++) {
		x += turn.accel.m[0][i] * raw[i];
		y += turn.accel.m[1][i] * raw[i];
	}
	float g2 = raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2];
	if (x * x + y * y < g2 * 0.25f)
		return -1; /* within 30 degrees of flat */
	if (y > fabsf(x) * 1.3f)
		return LV_DISPLAY_ROTATION_0;
	if (x > fabsf(y) * 1.3f)
		return LV_DISPLAY_ROTATION_90; /* the right edge up: the top to the left */
	if (-x > fabsf(y) * 1.3f)
		return LV_DISPLAY_ROTATION_270;
	return -1;
}

bool ui_accel_read(struct ui_accel *a)
{
	if (turn.fd[0] < 0)
		accel_open();
	if (!accel_read())
		return false;
	turn.accel.rotation = accel_rotation();
	*a = turn.accel;
	return true;
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

#define DROPPED_MS 200 /* a screen made upright is pushed this soon */

void ui_upright(void)
{
	lv_display_rotation_t r = lv_display_get_rotation(lv_display_get_default());
	if (r != LV_DISPLAY_ROTATION_0) {
		turn.dropped = r;
		turn.dropped_at = lv_tick_get();
	}
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

/* Before a transition from the screen shown to `to`. A screen opened from one that
 * turns (the menu) is made upright (ui_upright(): it may take sizes from the
 * display); if it turns too, it goes sideways again before the transition, as the
 * phone is still held, rather than upright until the next accelerometer reads. */
void ui_turn_for(lv_obj_t *to)
{
	lv_display_rotation_t dropped = turn.dropped;
	turn.dropped = LV_DISPLAY_ROTATION_0;
	if (turner_find(to) < 0)
		rotation_set(LV_DISPLAY_ROTATION_0);
	else if (ui_rotation_lock() >= 0)
		rotation_set(ui_rotation_lock());
	else if (dropped != LV_DISPLAY_ROTATION_0 && lv_tick_elaps(turn.dropped_at) < DROPPED_MS &&
	         turner_find(lv_screen_active()) >= 0 && !ui_locked())
		rotation_set(dropped);
}

static void turn_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	lv_obj_t *top = ui_current();
	if (turner_find(top) < 0 || ui_locked()) {
		rotation_set(LV_DISPLAY_ROTATION_0); /* shown some other way than a transition */
		return;
	}
	/* Not during a transition (its textures are the screen's size) */
	if (lv_screen_active() != top || lv_anim_count_running())
		return;
	if (ui_rotation_lock() >= 0) {
		rotation_set(ui_rotation_lock());
		return;
	}
	if (!turn.tried)
		accel_open();
	if (turn.fd[0] < 0)
		return;
	lv_display_rotation_t current = lv_display_get_rotation(lv_display_get_default());
	int seen = accel_read() ? accel_rotation() : -1;
	lv_display_rotation_t r = seen < 0 ? current : (lv_display_rotation_t)seen;
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

int ui_rotation_lock(void)
{
	if (turn.lock == -2) {
		turn.lock = ui_setting_get_int("rotation_lock", -1);
		if (turn.lock < -1 || turn.lock > LV_DISPLAY_ROTATION_270)
			turn.lock = -1;
	}
	return turn.lock;
}

void ui_rotation_lock_set(bool on)
{
	turn.lock = on ? (int)lv_display_get_rotation(lv_display_get_default()) : -1;
	turn.reads = 0;
	ui_setting_set_int("rotation_lock", turn.lock);
}
