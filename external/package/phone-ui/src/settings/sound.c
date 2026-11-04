/* Settings, Sound & vibration */

#include "settings.h"

void sound_summary(lv_obj_t *value)
{
	ui_set_text(value, ui_haptics ? "Vibrate" : "Silent");
}

static void haptics_cb(lv_event_t *e)
{
	ui_haptics = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
	ui_setting_set_int("haptics", ui_haptics);
	ui_vibrate(30);
}

static void test_vibration_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	/* Testing works with touch vibration off as well */
	bool haptics = ui_haptics;
	ui_haptics = true;
	ui_vibrate(300);
	ui_haptics = haptics;
}

static lv_obj_t *wide_button(lv_obj_t *parent, const char *text, uint32_t color)
{
	lv_obj_t *button = lv_button_create(parent);
	lv_obj_set_size(button, LV_PCT(100), 96);
	lv_obj_set_style_radius(button, 24, 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	lv_obj_center(label);
	return button;
}

void sound_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = settings_page("Sound & vibration", &page);

	ui_section_label(page, "Vibration");
	lv_obj_t *card = ui_card_create(page);
	lv_obj_t *row = settings_row(card);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text_static(label, "Vibrate on touch");
	lv_obj_set_flex_grow(label, 1);
	lv_obj_t *sw = ui_switch_create(row, ui_haptics);
	lv_obj_add_event_cb(sw, haptics_cb, LV_EVENT_VALUE_CHANGED, NULL);

	lv_obj_t *button = wide_button(page, LV_SYMBOL_BELL "  Test vibration", UI_COLOR_ACCENT);
	lv_obj_add_event_cb(button, test_vibration_cb, LV_EVENT_CLICKED, NULL);

	ui_section_label(page, "Sound");
	card = ui_card_create(page);
	ui_set_text(settings_info_row(card, "Ringtone"), "Not set up yet");
	ui_push(scr);
}
