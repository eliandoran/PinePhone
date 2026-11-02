/*
 * Feature phone user interface on the DRM display (touchscreen input).
 *
 * Home screen: wallpaper, clock and a Menu button. The menu lists the apps (none
 * exist yet: each opens a placeholder). The status bar (network, time, battery)
 * lives on the top layer, so it stays put while screens slide under it.
 *
 * Frames are paced by the display, as in lvgl-bench: the DRM driver waits for the
 * previous page flip before rendering into its buffer, so refreshing as soon as
 * something changed renders once per vblank.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"

#define BATTERY "/sys/class/power_supply/axp20x-battery"

#define STATUS_H 56
#define ANIM_MS 220

struct app {
	const char *name;
	const char *symbol;
	uint32_t color;
};

static const struct app apps[] = {
	{ "Phone",    LV_SYMBOL_CALL,      0x34c759 },
	{ "Messages", LV_SYMBOL_ENVELOPE,  0x0a84ff },
	{ "Contacts", LV_SYMBOL_BARS,      0xff9f0a },
	{ "Camera",   LV_SYMBOL_EYE_OPEN,  0x8e8e93 },
	{ "Gallery",  LV_SYMBOL_IMAGE,     0xff375f },
	{ "Music",    LV_SYMBOL_AUDIO,     0xbf5af2 },
	{ "Videos",   LV_SYMBOL_VIDEO,     0x5e5ce6 },
	{ "Alarm",    LV_SYMBOL_BELL,      0xff453a },
	{ "Notes",    LV_SYMBOL_EDIT,      0xffd60a },
	{ "Files",    LV_SYMBOL_DIRECTORY, 0x64d2ff },
	{ "Maps",     LV_SYMBOL_GPS,       0x30d158 },
	{ "Settings", LV_SYMBOL_SETTINGS,  0x636366 },
};

static lv_draw_buf_t *wallpaper;
static lv_obj_t *home, *menu;
static lv_obj_t *status_time, *status_battery, *home_time, *home_date;
static bool rendered;

static void render_start_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	rendered = true;
}

static void input_found_cb(lv_indev_t *indev, lv_evdev_type_t type, void *user_data)
{
	LV_UNUSED(indev);
	LV_UNUSED(user_data);
	printf("phone-ui: input device added (%s)\n",
	       type == LV_EVDEV_TYPE_ABS ? "touchscreen" :
	       type == LV_EVDEV_TYPE_REL ? "mouse" : "keys");
}

/* Setting the same text still redraws the label: only set it when it changed */
static void set_text(lv_obj_t *label, const char *text)
{
	if (strcmp(lv_label_get_text(label), text) != 0)
		lv_label_set_text(label, text);
}

/*
 * Wallpaper: a dark blue to teal gradient with three soft glows, computed once.
 * A placeholder until wallpapers are image files (no image decoder is built yet).
 */
static void make_wallpaper(int32_t w, int32_t h)
{
	static const struct { float x, y, r, s; float c[3]; } glows[] = {
		{ 0.15f, 0.22f, 0.45f, 0.55f, { 255, 110, 170 } },
		{ 0.95f, 0.55f, 0.50f, 0.45f, {  70, 190, 255 } },
		{ 0.35f, 0.95f, 0.40f, 0.40f, { 110, 255, 200 } },
	};
	static const float top[3] = { 16, 18, 52 }, mid[3] = { 52, 26, 96 }, bot[3] = { 8, 70, 96 };

	wallpaper = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0);
	uint32_t seed = 1;

	for (int32_t y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(wallpaper->data + y * wallpaper->header.stride);
		for (int32_t x = 0; x < w; x++) {
			float nx = (float)x / w, ny = (float)y / w;
			float t = (float)y / h * 0.85f + nx * 0.15f;
			float c[3];
			for (int i = 0; i < 3; i++)
				c[i] = t < 0.5f ? top[i] + (mid[i] - top[i]) * t * 2
				                : mid[i] + (bot[i] - mid[i]) * (t - 0.5f) * 2;

			for (size_t g = 0; g < sizeof(glows) / sizeof(glows[0]); g++) {
				float dx = nx - glows[g].x, dy = ny - glows[g].y * h / w;
				float q = 1 + (dx * dx + dy * dy) / (glows[g].r * glows[g].r) * 6;
				float f = glows[g].s / (q * q);
				for (int i = 0; i < 3; i++)
					c[i] += (255 - c[i]) * f * glows[g].c[i] / 255;
			}

			/* A little noise against banding */
			seed = seed * 1103515245 + 12345;
			float n = (float)((seed >> 16) & 3) - 1.5f;
			uint32_t px = 0xff000000;
			for (int i = 0; i < 3; i++) {
				float v = c[i] + n;
				px |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << (16 - 8 * i);
			}
			row[x] = px;
		}
	}
}

/* A screen with the wallpaper behind everything, content below the status bar */
static lv_obj_t *screen_create(lv_opa_t dim)
{
	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_set_scrollable(scr, false);

	lv_obj_t *img = lv_image_create(scr);
	lv_image_set_src(img, wallpaper);

	lv_obj_t *content = lv_obj_create(scr);
	lv_obj_remove_style_all(content);
	lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_pad_top(content, STATUS_H, 0);
	lv_obj_set_style_bg_color(content, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(content, dim, 0);
	return content;
}

/* Status bar */

static void status_update(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	char buf[64];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);

	strftime(buf, sizeof(buf), "%H:%M", &tm);
	set_text(status_time, buf);
	set_text(home_time, buf);
	strftime(buf, sizeof(buf), "%A %-d %B", &tm);
	set_text(home_date, buf);

	int capacity = -1;
	char status[32] = "";
	FILE *f = fopen(BATTERY "/capacity", "r");
	if (f) {
		if (fscanf(f, "%d", &capacity) != 1)
			capacity = -1;
		fclose(f);
	}
	f = fopen(BATTERY "/status", "r");
	if (f) {
		if (!fgets(status, sizeof(status), f))
			status[0] = '\0';
		fclose(f);
	}
	bool charging = strncmp(status, "Charging", 8) == 0;

	if (capacity < 0) {
		set_text(status_battery, LV_SYMBOL_BATTERY_EMPTY " ?");
		return;
	}
	const char *icon = capacity >= 90 ? LV_SYMBOL_BATTERY_FULL :
	                   capacity >= 65 ? LV_SYMBOL_BATTERY_3 :
	                   capacity >= 40 ? LV_SYMBOL_BATTERY_2 :
	                   capacity >= 15 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
	snprintf(buf, sizeof(buf), "%s%d%% %s", charging ? LV_SYMBOL_CHARGE " " : "", capacity, icon);
	set_text(status_battery, buf);
	lv_obj_set_style_text_color(status_battery, capacity < 15 ?
	                            lv_palette_main(LV_PALETTE_RED) : lv_color_white(), 0);
}

static void status_bar_create(void)
{
	lv_obj_t *bar = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, LV_PCT(100), STATUS_H);
	lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);
	lv_obj_set_style_pad_hor(bar, 20, 0);
	lv_obj_set_style_text_color(bar, lv_color_white(), 0);
	lv_obj_set_style_text_font(bar, &lv_font_montserrat_24, 0);

	/* The modem is not driven yet */
	lv_obj_t *network = lv_label_create(bar);
	lv_label_set_text(network, "No SIM");
	lv_obj_align(network, LV_ALIGN_LEFT_MID, 0, 0);

	status_time = lv_label_create(bar);
	lv_label_set_text(status_time, "");
	lv_obj_align(status_time, LV_ALIGN_CENTER, 0, 0);

	status_battery = lv_label_create(bar);
	lv_label_set_text(status_battery, "");
	lv_obj_align(status_battery, LV_ALIGN_RIGHT_MID, 0, 0);
}

/* Screens */

static void back_to_menu_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_screen_load_anim(menu, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, ANIM_MS, 0, true);
}

static void back_to_home_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_screen_load_anim(home, LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM, ANIM_MS, 0, false);
}

static void open_menu_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_screen_load_anim(menu, LV_SCREEN_LOAD_ANIM_MOVE_TOP, ANIM_MS, 0, false);
}

/* Header with a back button and a title */
static void header_create(lv_obj_t *parent, const char *title, lv_event_cb_t back_cb)
{
	lv_obj_t *header = lv_obj_create(parent);
	lv_obj_remove_style_all(header);
	lv_obj_set_size(header, LV_PCT(100), 96);

	lv_obj_t *back = lv_button_create(header);
	lv_obj_set_size(back, 80, 72);
	lv_obj_align(back, LV_ALIGN_LEFT_MID, 12, 0);
	lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(back, 0, 0);
	lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *icon = lv_label_create(back);
	lv_label_set_text(icon, LV_SYMBOL_LEFT);
	lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);
	lv_obj_center(icon);

	lv_obj_t *label = lv_label_create(header);
	lv_label_set_text(label, title);
	lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

/* Rounded colored square with the app's symbol */
static lv_obj_t *app_icon_create(lv_obj_t *parent, const struct app *app, int32_t size)
{
	lv_obj_t *icon = lv_obj_create(parent);
	lv_obj_remove_style_all(icon);
	lv_obj_set_size(icon, size, size);
	lv_obj_set_style_radius(icon, size / 4, 0);
	lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(icon, lv_color_hex(app->color), 0);
	lv_obj_set_clickable(icon, false);

	lv_obj_t *symbol = lv_label_create(icon);
	lv_label_set_text(symbol, app->symbol);
	lv_obj_set_style_text_font(symbol, &lv_font_montserrat_48, 0);
	lv_obj_set_style_text_color(symbol, lv_color_white(), 0);
	lv_obj_center(symbol);
	return icon;
}

static void open_app_cb(lv_event_t *e)
{
	const struct app *app = lv_event_get_user_data(e);
	lv_obj_t *content = screen_create(LV_OPA_70);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	header_create(content, app->name, back_to_menu_cb);

	lv_obj_t *body = lv_obj_create(content);
	lv_obj_remove_style_all(body);
	lv_obj_set_width(body, LV_PCT(100));
	lv_obj_set_flex_grow(body, 1);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(body, 32, 0);
	lv_obj_set_style_pad_bottom(body, 96, 0);

	app_icon_create(body, app, 180);
	lv_obj_t *label = lv_label_create(body);
	lv_label_set_text(label, "Coming soon");
	lv_obj_set_style_text_color(label, lv_color_hex(0xc0c0c8), 0);

	lv_screen_load_anim(lv_obj_get_screen(content), LV_SCREEN_LOAD_ANIM_MOVE_LEFT, ANIM_MS, 0, false);
}

static void menu_create(void)
{
	lv_obj_t *content = screen_create(LV_OPA_60);
	menu = lv_obj_get_screen(content);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	header_create(content, "Menu", back_to_home_cb);

	lv_obj_t *grid = lv_obj_create(content);
	lv_obj_remove_style_all(grid);
	lv_obj_set_width(grid, LV_PCT(100));
	lv_obj_set_flex_grow(grid, 1);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_hor(grid, 12, 0);
	lv_obj_set_style_pad_ver(grid, 16, 0);
	lv_obj_set_style_pad_row(grid, 24, 0);
	lv_obj_set_scroll_dir(grid, LV_DIR_VER);

	for (size_t i = 0; i < sizeof(apps) / sizeof(apps[0]); i++) {
		lv_obj_t *tile = lv_obj_create(grid);
		lv_obj_remove_style_all(tile);
		lv_obj_set_size(tile, LV_PCT(33), LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_ver(tile, 12, 0);
		lv_obj_set_style_pad_row(tile, 12, 0);
		lv_obj_set_style_radius(tile, 24, 0);
		lv_obj_set_style_bg_color(tile, lv_color_white(), LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(tile, LV_OPA_20, LV_STATE_PRESSED);
		lv_obj_set_clickable(tile, true);
		lv_obj_set_scrollable(tile, false);
		lv_obj_add_event_cb(tile, open_app_cb, LV_EVENT_CLICKED, (void *)&apps[i]);

		app_icon_create(tile, &apps[i], 128);
		lv_obj_t *label = lv_label_create(tile);
		lv_label_set_text(label, apps[i].name);
		lv_obj_set_style_text_color(label, lv_color_white(), 0);
	}
}

static void home_create(void)
{
	lv_obj_t *content = screen_create(LV_OPA_TRANSP);
	home = lv_obj_get_screen(content);
	lv_obj_set_style_text_color(content, lv_color_white(), 0);

	home_time = lv_label_create(content);
	lv_label_set_text(home_time, "");
	lv_obj_set_style_text_font(home_time, &lv_font_montserrat_48, 0);
	lv_obj_align(home_time, LV_ALIGN_TOP_MID, 0, 160);

	home_date = lv_label_create(content);
	lv_label_set_text(home_date, "");
	lv_obj_set_style_text_font(home_date, &lv_font_montserrat_26, 0);
	lv_obj_align(home_date, LV_ALIGN_TOP_MID, 0, 270);

	lv_obj_t *button = lv_button_create(content);
	lv_obj_set_size(button, 240, 88);
	lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -80);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(button, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_20, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_40, LV_STATE_PRESSED);
	lv_obj_set_style_border_color(button, lv_color_white(), 0);
	lv_obj_set_style_border_opa(button, LV_OPA_40, 0);
	lv_obj_set_style_border_width(button, 2, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, open_menu_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text(label, LV_SYMBOL_LIST "  Menu");
	lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
	lv_obj_center(label);
}

int main(void)
{
	/* stdout goes to a log file: flush every line, the program never exits */
	setvbuf(stdout, NULL, _IOLBF, 0);

	lv_init();

	lv_display_t *disp = lv_linux_drm_create();
	if (lv_linux_drm_set_file(disp, "/dev/dri/card0", -1) != LV_RESULT_OK) {
		fprintf(stderr, "phone-ui: cannot use /dev/dri/card0\n");
		return EXIT_FAILURE;
	}
#if LV_USE_PERF_MONITOR
	lv_sysmon_hide_performance(disp);
#endif

	/* Refresh from the loop below; step animations on every pass (once per frame) */
	lv_display_delete_refr_timer(disp);
	lv_timer_set_period(lv_anim_get_timer(), 0);
	lv_display_add_event_cb(disp, render_start_cb, LV_EVENT_RENDER_START, NULL);

	lv_display_set_theme(disp, lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
	                                                 lv_palette_main(LV_PALETTE_TEAL), true,
	                                                 &lv_font_montserrat_24));

	lv_evdev_discovery_start(input_found_cb, NULL);

	make_wallpaper(lv_display_get_horizontal_resolution(disp),
	               lv_display_get_vertical_resolution(disp));
	status_bar_create();
	home_create();
	menu_create();
	lv_screen_load(home);
	lv_timer_ready(lv_timer_create(status_update, 1000, NULL));

	for (;;) {
		uint32_t idle = lv_timer_handler();

		rendered = false;
		lv_display_refr_timer(NULL);
		if (rendered)
			continue;

		if (idle == LV_NO_TIMER_READY || lv_anim_count_running())
			idle = LV_DEF_REFR_PERIOD;
		usleep((idle ? idle : 1) * 1000);
	}
}
