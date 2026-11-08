/* Settings: the list of sections, settings_apply(), the building blocks of their pages */

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/bluetooth.h"
#include "ui_symbols.h"
#include "settings.h"

void settings_apply(void)
{
	ui_language_use(ui_setting_get("language", "en"));
	datetime_apply();
	wifi_apply();
	bt_apply();
	ui_haptics = ui_setting_get_int("haptics", 1) != 0;
	int brightness = ui_setting_get_int("brightness", -1);
	if (brightness >= 0)
		ui_brightness_set(brightness);
}

/* Building blocks */

static void timer_delete_cb(lv_event_t *e)
{
	lv_timer_delete(lv_event_get_user_data(e));
}

void settings_timer(lv_obj_t *scr, lv_timer_cb_t cb, uint32_t period)
{
	lv_timer_t *t = lv_timer_create(cb, period, NULL);
	lv_obj_add_event_cb(scr, timer_delete_cb, LV_EVENT_DELETE, t);
	cb(t);
}

lv_obj_t *settings_page(const char *title, lv_obj_t **page)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(title, NULL, &body);
	lv_obj_set_style_pad_hor(body, 20, 0);
	lv_obj_set_style_pad_top(body, 8, 0);
	lv_obj_set_style_pad_bottom(body, 40, 0);
	lv_obj_set_style_pad_row(body, 16, 0);
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	*page = body;
	return scr;
}

lv_obj_t *settings_panel(lv_obj_t *parent)
{
	lv_obj_t *card = ui_card_create(parent);
	lv_obj_set_style_pad_all(card, 28, 0);
	lv_obj_set_style_pad_row(card, 12, 0);
	return card;
}

lv_obj_t *settings_row(lv_obj_t *card)
{
	lv_obj_t *row = ui_box(card);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(row, 24, 0);
	lv_obj_set_style_pad_ver(row, 16, 0);
	lv_obj_set_style_pad_column(row, 20, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	/* Its title, as the other lists' rows; values (settings_value()) are body text */
	lv_obj_set_style_text_font(row, UI_FONT_TITLE, 0);
	/* At least 88 px high with the content centered. A min_height would leave it at the top
	 * (LVGL places the line of a content-sized flex at the start), so instead an invisible
	 * strut makes the line 56 px high and the items are centered on it. The negative margin
	 * cancels the column gap after it. */
	lv_obj_t *strut = ui_box(row);
	lv_obj_set_size(strut, 0, 56);
	lv_obj_set_style_margin_right(strut, -20, 0);
	if (lv_obj_get_child_count(card) > 1) {
		lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, 0);
		lv_obj_set_style_border_width(row, 1, 0);
		lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_LINE), 0);
	}
	return row;
}

void settings_row_tappable(lv_obj_t *row, lv_event_cb_t cb, void *user_data)
{
	lv_obj_set_style_radius(row, 24, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_clickable(row, true);
	lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
}

lv_obj_t *settings_value(lv_obj_t *row)
{
	lv_obj_t *value = lv_label_create(row);
	ui_set_text(value, "");
	lv_obj_set_style_text_font(value, UI_FONT_BODY, 0);
	lv_obj_set_flex_grow(value, 1);
	lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_style_text_color(value, lv_color_hex(UI_COLOR_DIM), 0);
	lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_WRAP);
	return value;
}

lv_obj_t *settings_info_row(lv_obj_t *card, const char *title)
{
	lv_obj_t *row = settings_row(card);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text_static(label, title);
	return settings_value(row);
}

lv_obj_t *settings_title_line(lv_obj_t *panel, const char *title)
{
	lv_obj_t *top = ui_box(panel);
	lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_t *label = lv_label_create(top);
	lv_label_set_text_static(label, title);
	lv_obj_t *value = lv_label_create(top);
	ui_set_text(value, "");
	lv_obj_set_style_text_color(value, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_align(value, LV_ALIGN_RIGHT_MID, 0, 0);
	return value;
}

lv_obj_t *settings_bar(lv_obj_t *panel, int32_t max, uint32_t color)
{
	lv_obj_t *bar = lv_bar_create(panel);
	lv_obj_set_size(bar, LV_PCT(100), 20);
	lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
	lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
	lv_bar_set_range(bar, 0, max);
	return bar;
}

const char *settings_device(struct settings_device *d, const char *dir, const char *match)
{
	if (!d->searched) {
		d->searched = true;
		DIR *dp = opendir(dir);
		struct dirent *ent;
		char link[300], real[PATH_MAX];
		while (dp && !d->path[0] && (ent = readdir(dp))) {
			snprintf(link, sizeof(link), "%s/%s", dir, ent->d_name);
			if (ent->d_name[0] != '.' && (!match || (realpath(link, real) && strstr(real, match))))
				snprintf(d->path, sizeof(d->path), "%s", link);
		}
		if (dp)
			closedir(dp);
	}
	return d->path[0] ? d->path : NULL;
}

/* The AXP803 ADC: the IIO device whose sysfs path goes through axp813-adc */
const char *settings_pmic_adc(void)
{
	static struct settings_device adc;
	return settings_device(&adc, "/sys/bus/iio/devices", "axp813-adc");
}

bool settings_attr_long(const char *dev, const char *attr, long *v)
{
	char path[400];
	if (!dev)
		return false;
	snprintf(path, sizeof(path), "%s/%s", dev, attr);
	return ui_read_long(path, v);
}

bool settings_attr_double(const char *dev, const char *attr, double *v)
{
	char path[400], buf[32];
	if (!dev)
		return false;
	snprintf(path, sizeof(path), "%s/%s", dev, attr);
	if (!ui_read_line(path, buf, sizeof(buf)))
		return false;
	*v = strtod(buf, NULL);
	return true;
}

/* The list: cards of sections, each with its value */

static const struct section {
	const char *symbol;
	uint32_t color;
	const char *title;
	void (*open)(void);
	void (*summary)(lv_obj_t *value);
	bool card; /* starts a card */
} sections[] = {
	{ LV_SYMBOL_IMAGE,      0x0a84ff, N_("Display"),           display_open,     display_summary, true },
	{ LV_SYMBOL_VOLUME_MAX, 0xff375f, N_("Sound & vibration"), sound_open,       sound_summary },
	{ LV_SYMBOL_BELL,       0xff9f0a, N_("Date & time"),       datetime_open,    datetime_summary },
	{ UI_SYMBOL_LANGUAGE,   0xbf5af2, N_("Language"),          language_open,    language_summary },
	{ LV_SYMBOL_BATTERY_3,  0x34c759, N_("Battery"),           battery_open,     battery_summary, true },
	{ LV_SYMBOL_WIFI,       0x0a84ff, N_("Wi-Fi"),             wifi_open,        wifi_summary },
	{ LV_SYMBOL_BLUETOOTH,  0x0a84ff, "Bluetooth",             bluetooth_open,   bluetooth_summary }, /* NON-NLS */
	{ LV_SYMBOL_SHUFFLE,    0x30b0c7, N_("Network"),           network_open,     network_summary },
	{ LV_SYMBOL_SD_CARD,    0x8e8e93, N_("Storage"),           storage_open,     storage_summary },
	{ LV_SYMBOL_LIST,       0x5e5ce6, N_("About phone"),       about_open,       about_summary, true },
	{ UI_SYMBOL_CODE,       0x636366, N_("Developer mode"),    developer_open,   NULL },
};
#define SECTION_COUNT (sizeof(sections) / sizeof(sections[0]))

static lv_obj_t *values[SECTION_COUNT];

static void open_cb(lv_event_t *e)
{
	const struct section *s = lv_event_get_user_data(e);
	s->open();
}

static lv_obj_t *nav_row(lv_obj_t *card, const struct section *s)
{
	lv_obj_t *row = settings_row(card);
	lv_obj_set_style_pad_left(row, 20, 0);
	settings_row_tappable(row, open_cb, (void *)s);

	lv_obj_t *icon = ui_box(row);
	lv_obj_set_size(icon, 60, 60);
	lv_obj_set_style_radius(icon, 16, 0);
	lv_obj_set_style_bg_color(icon, lv_color_hex(s->color), 0);
	lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
	lv_obj_t *sym = lv_label_create(icon);
	lv_label_set_text_static(sym, s->symbol);
	lv_obj_set_style_text_font(sym, &ui_font_24, 0);
	lv_obj_set_style_text_color(sym, lv_color_white(), 0);
	lv_obj_center(sym);

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text_static(label, _(s->title));

	lv_obj_t *value = settings_value(row);

	lv_obj_t *chevron = lv_label_create(row);
	lv_label_set_text_static(chevron, LV_SYMBOL_RIGHT);
	lv_obj_set_style_text_font(chevron, &ui_font_20, 0);
	lv_obj_set_style_text_color(chevron, lv_color_hex(UI_COLOR_DIM), 0);
	return value;
}

static void list_update(void)
{
	for (size_t i = 0; i < SECTION_COUNT; i++)
		if (sections[i].summary)
			sections[i].summary(values[i]);
}

static void list_loaded_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	list_update();
}

void settings_open(void)
{
	lv_obj_t *page, *card = NULL;
	lv_obj_t *scr = settings_page(_("Settings"), &page);

	for (size_t i = 0; i < SECTION_COUNT; i++) {
		if (sections[i].card || !card)
			card = ui_card_create(page);
		values[i] = nav_row(card, &sections[i]);
	}

	/* Coming back from a section: its value may have changed */
	lv_obj_add_event_cb(scr, list_loaded_cb, LV_EVENT_SCREEN_LOAD_START, NULL);
	list_update();
	ui_push(scr);
}
