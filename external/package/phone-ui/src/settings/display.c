/* Settings, Display: brightness (the backlight, ui_brightness_*()), theme, screen */

#include <stdio.h>

#include "../ui_symbols.h"
#include "settings.h"

/* Backlight: the first device of the class */

static const char *backlight(void)
{
	static struct settings_device dev;
	return settings_device(&dev, "/sys/class/backlight", NULL);
}

static long backlight_max(void)
{
	char path[320];
	long max;
	if (!backlight())
		return 0;
	snprintf(path, sizeof(path), "%s/max_brightness", backlight());
	return ui_read_long(path, &max) && max > 0 ? max : 0;
}

void ui_brightness_set(int percent)
{
	long max = backlight_max();
	char path[320];
	if (!max)
		return;
	if (percent < UI_BRIGHTNESS_MIN)
		percent = UI_BRIGHTNESS_MIN;
	if (percent > 100)
		percent = 100;
	long raw = (max * percent + 50) / 100;
	snprintf(path, sizeof(path), "%s/brightness", backlight());
	ui_write_long(path, raw < 1 ? 1 : raw);
}

void ui_backlight_on(bool on)
{
	char path[320];
	if (!backlight())
		return;
	snprintf(path, sizeof(path), "%s/bl_power", backlight());
	ui_write_long(path, on ? 0 : 4); /* FB_BLANK_UNBLANK, FB_BLANK_POWERDOWN */
}

int ui_brightness_get(void)
{
	long max = backlight_max(), raw;
	char path[320];
	if (!max)
		return -1;
	snprintf(path, sizeof(path), "%s/brightness", backlight());
	if (!ui_read_long(path, &raw))
		return -1;
	return (int)((raw * 100 + max / 2) / max);
}

void display_summary(lv_obj_t *value)
{
	int b = ui_brightness_get();
	if (b >= 0)
		ui_set_text_fmt(value, "%d%%", b);
	else
		ui_set_text(value, "");
}

static lv_obj_t *brightness_label;

static void brightness_cb(lv_event_t *e)
{
	lv_obj_t *slider = lv_event_get_target_obj(e);
	int v = (int)lv_slider_get_value(slider);
	ui_brightness_set(v);
	ui_set_text_fmt(brightness_label, "%d%%", v);
	/* Saving writes and syncs the file: once the finger is up */
	if (lv_event_get_code(e) == LV_EVENT_RELEASED)
		ui_setting_set_int("brightness", v);
}

/* The screens are rebuilt in the new theme's colors, this one included */
static void display_reopen(void)
{
	settings_open();
	display_open();
}

static void theme_cb(lv_event_t *e)
{
	ui_set_light((uintptr_t)lv_event_get_user_data(e) != 0, display_reopen);
}

void display_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = settings_page("Display", &page);

	ui_section_label(page, "Brightness");
	lv_obj_t *card = settings_panel(page);
	int b = ui_brightness_get();
	if (b < 0) {
		lv_obj_t *label = lv_label_create(card);
		lv_label_set_text_static(label, "No backlight found");
		lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	} else {
		brightness_label = settings_title_line(card, "Screen brightness");
		ui_set_text_fmt(brightness_label, "%d%%", b);

		lv_obj_t *row = ui_box(card);
		lv_obj_set_size(row, LV_PCT(100), 88);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(row, 32, 0);

		lv_obj_t *dim = lv_label_create(row);
		lv_label_set_text_static(dim, UI_SYMBOL_SUN_REGULAR);
		lv_obj_set_style_text_font(dim, &ui_font_24, 0);
		lv_obj_set_style_text_color(dim, lv_color_hex(UI_COLOR_DIM), 0);

		lv_obj_t *slider = ui_slider_create(row);
		lv_slider_set_range(slider, UI_BRIGHTNESS_MIN, 100);
		lv_slider_set_value(slider, b, LV_ANIM_OFF);
		lv_obj_add_event_cb(slider, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
		lv_obj_add_event_cb(slider, brightness_cb, LV_EVENT_RELEASED, NULL);

		lv_obj_t *bright = lv_label_create(row);
		lv_label_set_text_static(bright, UI_SYMBOL_SUN);
		lv_obj_set_style_text_font(bright, &ui_font_32, 0);
	}

	ui_section_label(page, "Appearance");
	card = ui_card_create(page);
	for (uintptr_t light = 0; light <= 1; light++) {
		lv_obj_t *row = settings_row(card);
		settings_row_tappable(row, theme_cb, (void *)light);

		lv_obj_t *label = lv_label_create(row);
		lv_label_set_text_static(label, light ? "Light" : "Dark");
		lv_obj_set_flex_grow(label, 1);
		if (light == ui_light) {
			lv_obj_t *check = lv_label_create(row);
			lv_label_set_text_static(check, LV_SYMBOL_OK);
			lv_obj_set_style_text_color(check, lv_color_hex(UI_COLOR_ACCENT), 0);
		}
	}

	ui_section_label(page, "Screen");
	card = ui_card_create(page);
	lv_display_t *disp = lv_display_get_default();
	ui_set_text_fmt(settings_info_row(card, "Resolution"), "%d x %d",
	                (int)lv_display_get_horizontal_resolution(disp),
	                (int)lv_display_get_vertical_resolution(disp));
	ui_set_text_fmt(settings_info_row(card, "Density"), "%d dpi", (int)lv_display_get_dpi(disp));
	ui_set_text_fmt(settings_info_row(card, "Animations"), "%u ms", (unsigned)ui_anim_ms);
	ui_push(scr);
}
