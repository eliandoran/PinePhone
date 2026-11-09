/*
 * A mouse on the monitor (ui_internal.h): the first input device with relative X and Y and
 * a left button (USB on a dock, or Bluetooth), opened beside LVGL's devices (main.c drops
 * the pointer LVGL's discovery makes of it on the phone's display). Read into a pointer
 * on the monitor's display, the position kept on it, a cursor there while one is
 * connected; its wheel scrolls what is under the cursor. Without one, /dev/input is
 * looked through again once a second; one disconnected reads ENODEV (the tests' FIFO:
 * the end of the file).
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>

#include "ui/ui_internal.h"

#define SCAN_MS 1000
#define WHEEL_PX 96 /* scrolled per click of the wheel */
#define CURSOR 22   /* across */

static struct {
	lv_display_t *disp;
	lv_indev_t *indev;
	lv_obj_t *cursor; /* its hot spot: a 1 x 1 object, the disc drawn around it */
	int fd;           /* -1: none connected */
	char name[64];
	int32_t x, y;
	bool left;
	uint64_t scan_ms;
} m = { .fd = -1 };

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static bool has_bit(const unsigned char *bits, int bit)
{
	return bits[bit / 8] & 1 << bit % 8;
}

static bool is_mouse(int fd)
{
	unsigned char rel[REL_MAX / 8 + 1] = { 0 }, keys[KEY_MAX / 8 + 1] = { 0 };
	return ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel) >= 0 && has_bit(rel, REL_X) && has_bit(rel, REL_Y) &&
	       ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 && has_bit(keys, BTN_LEFT);
}

/* The first mouse in /dev/input, if any: its cursor in the middle */
static void scan(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	while (d && m.fd < 0 && (e = readdir(d))) {
		char path[300];
		if (strncmp(e->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!is_mouse(fd)) {
			close(fd);
			continue;
		}
		m.fd = fd;
		if (ioctl(fd, EVIOCGNAME(sizeof(m.name)), m.name) < 0)
			snprintf(m.name, sizeof(m.name), "%.32s", e->d_name);
		m.x = lv_display_get_horizontal_resolution(m.disp) / 2;
		m.y = lv_display_get_vertical_resolution(m.disp) / 2;
		m.left = false;
		lv_obj_set_hidden(m.cursor, false);
		printf("phone-ui: mouse %s (%s) on the monitor\n", m.name, path);
	}
	if (d)
		closedir(d);
}

static void gone(void)
{
	printf("phone-ui: mouse %s gone\n", m.name);
	close(m.fd);
	m.fd = -1;
	m.left = false;
	lv_obj_set_hidden(m.cursor, true);
}

static int32_t clamp(int32_t v, int32_t max)
{
	return v < 0 ? 0 : v > max ? max : v;
}

/* The wheel turned `clicks` (up positive): what scrolls up and down under the cursor */
static void scroll(int clicks)
{
	lv_point_t p = { m.x, m.y };
	lv_obj_t *obj = lv_indev_search_obj(lv_display_get_screen_active(m.disp), &p);
	while (obj && !((lv_obj_get_scroll_dir(obj) & LV_DIR_VER) &&
	                (lv_obj_get_scroll_top(obj) > 0 || lv_obj_get_scroll_bottom(obj) > 0)))
		obj = lv_obj_get_parent(obj);
	if (obj)
		lv_obj_scroll_by_bounded(obj, 0, clicks * WHEEL_PX, LV_ANIM_ON);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	LV_UNUSED(indev);
	uint64_t now = now_ms();
	if (m.fd < 0 && now - m.scan_ms >= SCAN_MS) {
		m.scan_ms = now;
		scan();
	}
	int32_t w = lv_display_get_horizontal_resolution(m.disp), h = lv_display_get_vertical_resolution(m.disp);
	int wheel = 0;
	struct input_event ev;
	ssize_t n = -1;
	while (m.fd >= 0 && (n = read(m.fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
		if (ev.type == EV_REL && ev.code == REL_X)
			m.x = clamp(m.x + ev.value, w - 1);
		else if (ev.type == EV_REL && ev.code == REL_Y)
			m.y = clamp(m.y + ev.value, h - 1);
		else if (ev.type == EV_REL && ev.code == REL_WHEEL)
			wheel += ev.value;
		else if (ev.type == EV_KEY && ev.code == BTN_LEFT)
			m.left = ev.value != 0;
	}
	if (m.fd >= 0 && (n == 0 || (n < 0 && errno != EAGAIN)))
		gone();
	if (wheel)
		scroll(wheel);
	data->point = (lv_point_t){ m.x, m.y };
	data->state = m.left ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void mouse_start(lv_display_t *disp)
{
	mouse_stop();
	m.disp = disp;
	m.scan_ms = now_ms() - SCAN_MS; /* looked for at the first read */

	m.cursor = lv_obj_create(lv_display_get_layer_sys(disp));
	lv_obj_remove_style_all(m.cursor);
	lv_obj_set_size(m.cursor, 1, 1);
	lv_obj_set_overflow_visible(m.cursor, true);
	lv_obj_t *disc = lv_obj_create(m.cursor);
	lv_obj_remove_style_all(disc);
	lv_obj_set_size(disc, CURSOR, CURSOR);
	lv_obj_set_pos(disc, -CURSOR / 2, -CURSOR / 2);
	lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(disc, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(disc, lv_color_black(), 0);
	lv_obj_set_style_border_width(disc, 3, 0);
	lv_obj_set_clickable(disc, false);
	lv_obj_set_hidden(m.cursor, true);

	m.indev = lv_indev_create();
	lv_indev_set_type(m.indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_display(m.indev, disp);
	lv_indev_set_read_cb(m.indev, read_cb);
	lv_indev_set_cursor(m.indev, m.cursor);
}

void mouse_stop(void)
{
	if (!m.disp)
		return;
	lv_indev_delete(m.indev);
	lv_obj_delete(m.cursor);
	if (m.fd >= 0)
		close(m.fd);
	m = (typeof(m)){ .fd = -1 };
}

lv_obj_t *mouse_cursor(void)
{
	return m.cursor;
}
