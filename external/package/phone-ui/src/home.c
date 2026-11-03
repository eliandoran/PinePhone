/*
 * Home screen (clock, date, Menu button) and the menu with the apps.
 *
 * The home screen and the menu turn sideways with the phone (a trial of turning
 * the whole display): while one of them is shown, the accelerometer is read a few
 * times a second and the display rotated (lv_display_set_rotation(): LVGL lays the
 * screens out again at 1440 x 720, the OpenGL ES driver turns the display texture
 * as it draws it, touches are turned back; ui.c makes its wallpapers and
 * transitions for the new size). The menu has 8 tiles to a row sideways. Apps are
 * upright: the display turns back before one opens.
 */

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ui.h"

static const struct ui_app builtin_apps[] = {
	{ "Phone",      LV_SYMBOL_CALL,      0x34c759, phone_open },
	{ "Messages",   LV_SYMBOL_ENVELOPE,  0x0a84ff, messages_open },
	{ "Contacts",   LV_SYMBOL_BARS,      0xff9f0a, contacts_open },
	{ "Camera",     LV_SYMBOL_EYE_OPEN,  0x8e8e93, camera_open },
	{ "Gallery",    LV_SYMBOL_IMAGE,     0xff375f, gallery_open },
	{ "Music",      LV_SYMBOL_AUDIO,     0xbf5af2, music_open },
	{ "Clock",      LV_SYMBOL_BELL,      0xff453a, clock_open },
	{ "Calendar",   LV_SYMBOL_LIST,      0xe0592a, calendar_open },
	{ "Calculator", LV_SYMBOL_PLUS,      0x5e5ce6, calculator_open },
	{ "Notes",      LV_SYMBOL_EDIT,      0xffd60a, notes_open },
	{ "Recorder",   LV_SYMBOL_AUDIO,     0xff453a, recorder_open },
	{ "Files",      LV_SYMBOL_DIRECTORY, 0x64d2ff, files_open },
	{ "Maps",       LV_SYMBOL_GPS,       0x30d158, maps_open },
	{ "Weather",    LV_SYMBOL_CHARGE,    0x409cff, weather_open },
	{ "Compass",    LV_SYMBOL_UP,        0xac8e68, compass_open },
	{ "Browser",    LV_SYMBOL_WIFI,      0x5ac8fa, browser_open },
	{ "Assistant",  LV_SYMBOL_KEYBOARD,  0x4d6bfe, assistant_open },
	{ "Snake",      LV_SYMBOL_PLAY,      0x66d4cf, snake_open },
	{ "Settings",   LV_SYMBOL_SETTINGS,  0x636366, settings_open }, /* last: external.c */
};
const struct ui_app *ui_apps = builtin_apps;
unsigned ui_app_count = sizeof(builtin_apps) / sizeof(builtin_apps[0]);

static lv_obj_t *home_time, *home_date;

static void home_update(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	char buf[64];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);

	strftime(buf, sizeof(buf), "%H:%M", &tm);
	ui_set_text(home_time, buf);
	strftime(buf, sizeof(buf), "%A %-d %B", &tm);
	ui_set_text(home_date, buf);
	ui_status_update();
}

/* Apps that are not done yet */
static void placeholder_open(const struct ui_app *app)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(app->name, NULL, &body);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(body, 32, 0);
	lv_obj_set_style_pad_bottom(body, 96, 0);

	ui_app_icon_create(body, app, 180);
	lv_obj_t *label = lv_label_create(body);
	lv_label_set_text_static(label, "Coming soon");
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	ui_push(scr);
}

void ui_app_open(const struct ui_app *app)
{
	if (app->open)
		app->open();
	else if (app->dir)
		external_open(app);
	else
		placeholder_open(app);
}

static void home_upright(void);

static void open_app_cb(lv_event_t *e)
{
	home_upright();
	ui_app_open(lv_event_get_user_data(e));
}

#define MENU_PAD 12 /* on each side of the grid */

static lv_obj_t *home_screen, *menu_screen, *menu_grid;

/* The menu's tiles: 4 to a row upright, 8 sideways */
static void menu_tiles_fit(void)
{
	int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
	int32_t cols = w > lv_display_get_vertical_resolution(lv_display_get_default()) ? 8 : 4;
	for (uint32_t i = 0; menu_grid && i < lv_obj_get_child_count(menu_grid); i++)
		lv_obj_set_width(lv_obj_get_child(menu_grid, i), (w - 2 * MENU_PAD) / cols);
}

static void back_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_pop();
}

static lv_obj_t *menu_create(void)
{
	lv_obj_t *content;
	lv_obj_t *scr = ui_screen_create(LV_OPA_60, &content);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *header = lv_obj_create(content);
	lv_obj_remove_style_all(header);
	lv_obj_set_size(header, LV_PCT(100), UI_HEADER_H);
	lv_obj_t *back = lv_button_create(header);
	lv_obj_set_size(back, 80, 72);
	lv_obj_align(back, LV_ALIGN_LEFT_MID, 12, 0);
	lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(back, 0, 0);
	lv_obj_set_style_text_color(back, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *icon = lv_label_create(back);
	lv_label_set_text_static(icon, LV_SYMBOL_DOWN);
	lv_obj_set_style_text_font(icon, &ui_font_32, 0);
	lv_obj_center(icon);
	lv_obj_t *title = lv_label_create(header);
	lv_label_set_text_static(title, "Menu");
	lv_obj_set_style_text_font(title, &ui_font_32, 0);
	lv_obj_center(title);

	lv_obj_t *grid = lv_obj_create(content);
	lv_obj_remove_style_all(grid);
	lv_obj_set_width(grid, LV_PCT(100));
	lv_obj_set_flex_grow(grid, 1);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_hor(grid, MENU_PAD, 0);
	lv_obj_set_style_pad_ver(grid, 16, 0);
	lv_obj_set_style_pad_row(grid, 24, 0);
	lv_obj_set_scroll_dir(grid, LV_DIR_VER);

	for (unsigned i = 0; i < ui_app_count; i++) {
		lv_obj_t *tile = lv_obj_create(grid);
		lv_obj_remove_style_all(tile);
		lv_obj_set_height(tile, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_ver(tile, 12, 0);
		lv_obj_set_style_pad_row(tile, 12, 0);
		lv_obj_set_style_radius(tile, 24, 0);
		lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_TEXT), LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(tile, LV_OPA_20, LV_STATE_PRESSED);
		lv_obj_set_clickable(tile, true);
		lv_obj_set_scrollable(tile, false);
		lv_obj_add_event_cb(tile, open_app_cb, LV_EVENT_CLICKED, (void *)&ui_apps[i]);

		ui_app_icon_create(tile, &ui_apps[i], 112);
		lv_obj_t *label = lv_label_create(tile);
		lv_label_set_text_static(label, ui_apps[i].name);
	}
	menu_grid = grid;
	menu_tiles_fit();
	return scr;
}

/* Turning sideways */

#define ORIENT_MS 300  /* accelerometer reads, while the home screen is shown */
#define ORIENT_READS 2 /* a new orientation is taken once read this many times */

static struct {
	int fd[3];       /* in_accel_{x,y,z}_raw, -1: none */
	float m[2][3];   /* mount matrix rows: the phone's x, y */
	bool tried;
	lv_display_rotation_t next;
	int reads;
	lv_timer_t *timer;
} orient = { .fd = { -1, -1, -1 } };

static void accel_open(void)
{
	static const char axis[3] = { 'x', 'y', 'z' };
	char path[300];
	orient.tried = true;
	DIR *d = opendir("/sys/bus/iio/devices");
	struct dirent *e;
	while (d && orient.fd[0] < 0 && (e = readdir(d))) {
		if (strncmp(e->d_name, "iio:device", 10) != 0)
			continue;
		for (int i = 0; i < 3; i++) {
			snprintf(path, sizeof(path), "/sys/bus/iio/devices/%s/in_accel_%c_raw", e->d_name, axis[i]);
			orient.fd[i] = open(path, O_RDONLY | O_CLOEXEC);
		}
		if (orient.fd[0] < 0 || orient.fd[1] < 0 || orient.fd[2] < 0) {
			for (int i = 0; i < 3; i++) {
				if (orient.fd[i] >= 0)
					close(orient.fd[i]);
				orient.fd[i] = -1;
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
			orient.m[i / 3][i % 3] = m[i];
	}
	if (d)
		closedir(d);
	if (orient.fd[0] < 0)
		printf("phone-ui: no accelerometer, the home screen stays upright\n");
}

/* The rotation gravity shows; `current` while the phone lies flat, is upside down
 * or between two */
static lv_display_rotation_t accel_rotation(lv_display_rotation_t current)
{
	float raw[3];
	for (int i = 0; i < 3; i++) {
		char buf[32];
		ssize_t n = orient.fd[i] >= 0 ? pread(orient.fd[i], buf, sizeof(buf) - 1, 0) : -1;
		if (n <= 0)
			return current;
		buf[n] = '\0';
		raw[i] = strtof(buf, NULL);
	}
	/* Upward in the phone's axes (x to the right, y to the top): the mount matrix
	 * gives both the other way around (as found in apps/genesis) */
	float x = 0, y = 0;
	for (int i = 0; i < 3; i++) {
		x -= orient.m[0][i] * raw[i];
		y -= orient.m[1][i] * raw[i];
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

/* ui.c makes its wallpapers and transitions again for the new size
 * (LV_EVENT_RESOLUTION_CHANGED) */
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
	menu_tiles_fit();
	orient.reads = 0;
}

/* Upright again before anything but the home screen and the menu is shown */
static void home_upright(void)
{
	rotation_set(LV_DISPLAY_ROTATION_0);
}

static void orient_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	if (!ui_at_home()) {
		home_upright(); /* an app shown some other way than from the menu */
		return;
	}
	/* Not while sliding between the two */
	lv_obj_t *active = lv_screen_active();
	if ((active != home_screen && active != menu_screen) || lv_anim_count_running())
		return;
	if (!orient.tried)
		accel_open();
	if (orient.fd[0] < 0)
		return;
	lv_display_rotation_t current = lv_display_get_rotation(lv_display_get_default());
	lv_display_rotation_t r = accel_rotation(current);
	if (r == current) {
		orient.reads = 0;
		return;
	}
	if (r != orient.next) {
		orient.next = r;
		orient.reads = 0;
	}
	if (++orient.reads >= ORIENT_READS)
		rotation_set(r);
}

static void open_menu_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_open_menu();
}

void home_create(void)
{
	lv_obj_t *content;
	lv_obj_t *home = ui_screen_create(LV_OPA_TRANSP, &content);
	home_screen = home;
	if (!orient.timer)
		orient.timer = lv_timer_create(orient_cb, ORIENT_MS, NULL);

	home_time = lv_label_create(content);
	ui_set_text(home_time, "");
	lv_obj_set_style_text_font(home_time, &ui_font_48, 0);
	lv_obj_align(home_time, LV_ALIGN_TOP_MID, 0, 160);

	home_date = lv_label_create(content);
	ui_set_text(home_date, "");
	lv_obj_set_style_text_font(home_date, &ui_font_26, 0);
	lv_obj_align(home_date, LV_ALIGN_TOP_MID, 0, 270);

	lv_obj_t *button = lv_button_create(content);
	lv_obj_set_size(button, 240, 88);
	lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -80);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_20, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_40, LV_STATE_PRESSED);
	lv_obj_set_style_border_color(button, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(button, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_border_opa(button, LV_OPA_40, 0);
	lv_obj_set_style_border_width(button, 2, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, open_menu_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text_static(label, LV_SYMBOL_LIST "  Menu");
	lv_obj_set_style_text_font(label, &ui_font_32, 0);
	lv_obj_center(label);

	menu_screen = menu_create();
	ui_set_screens(home, menu_screen);
	/* Created again when the theme changes: one timer for all */
	static lv_timer_t *timer;
	if (!timer)
		timer = lv_timer_create(home_update, 1000, NULL);
	lv_timer_ready(timer);
}
