/* Settings, Sound & vibration */

#include "settings.h"
#include "system/ringer.h"

void sound_summary(lv_obj_t *value)
{
	ui_set_text(value, ui_haptics ? _("Vibrate") : _("Silent"));
}

static void haptics_cb(lv_event_t *e)
{
	ui_haptics = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
	ui_setting_set_int("haptics", ui_haptics);
	ui_vibrate(UI_VIBRATE_TAP);
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

static lv_obj_t *wide_button(lv_obj_t *parent, const char *symbol, const char *text, uint32_t color)
{
	lv_obj_t *button = lv_button_create(parent);
	lv_obj_set_size(button, LV_PCT(100), 96);
	lv_obj_set_style_radius(button, 24, 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_t *label = lv_label_create(button);
	ui_set_text_fmt(label, "%s  %s", symbol, text);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	lv_obj_center(label);
	return button;
}

/*
 * Ringtone: the tones (ringer.c), the chosen one checked. Tapping one chooses it and plays
 * it once, until another is tapped or the list is left.
 */

static lv_obj_t *ringtone_value; /* Sound & vibration's, under the list */

/* Each row's last child is its check mark, shown on the chosen one */
static void ringtone_checks(lv_obj_t *card)
{
	for (unsigned i = 0; i < ui_ringtone_count(); i++) {
		lv_obj_t *check = lv_obj_get_child(lv_obj_get_child(card, (int32_t)i), -1);
		lv_obj_set_hidden(check, i != ui_ringtone());
	}
}

static void ringtone_cb(lv_event_t *e)
{
	unsigned i = (unsigned)(uintptr_t)lv_event_get_user_data(e);
	ui_vibrate(UI_VIBRATE_TAP);
	ui_ringtone_set(i);
	ringtone_checks(lv_obj_get_parent(lv_event_get_target_obj(e)));
	ui_set_text(ringtone_value, _(ui_ringtone_name(i)));
	ui_ringer_play(i);
}

static void ringtone_left_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_ringer_stop();
}

static void ringtone_open_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_t *page;
	lv_obj_t *scr = settings_page(_("Ringtone"), &page);

	lv_obj_t *card = ui_card_create(page);
	for (unsigned i = 0; i < ui_ringtone_count(); i++) {
		lv_obj_t *row = settings_row(card);
		settings_row_tappable(row, ringtone_cb, (void *)(uintptr_t)i);

		lv_obj_t *label = lv_label_create(row);
		lv_label_set_text_static(label, _(ui_ringtone_name(i)));
		lv_obj_set_flex_grow(label, 1);
		lv_obj_t *check = lv_label_create(row);
		lv_label_set_text_static(check, LV_SYMBOL_OK);
		lv_obj_set_style_text_color(check, lv_color_hex(UI_COLOR_ACCENT), 0);
	}
	ringtone_checks(card);
	lv_obj_add_event_cb(scr, ringtone_left_cb, LV_EVENT_DELETE, NULL);
	ui_push(scr);
}

void sound_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = settings_page(_("Sound & vibration"), &page);

	ui_section_label(page, _("Vibration"));
	lv_obj_t *card = ui_card_create(page);
	lv_obj_t *row = settings_row(card);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text_static(label, _("Vibrate on touch"));
	lv_obj_set_flex_grow(label, 1);
	lv_obj_t *sw = ui_switch_create(row, ui_haptics);
	lv_obj_add_event_cb(sw, haptics_cb, LV_EVENT_VALUE_CHANGED, NULL);

	lv_obj_t *button = wide_button(page, LV_SYMBOL_BELL, _("Test vibration"), UI_COLOR_ACCENT);
	lv_obj_add_event_cb(button, test_vibration_cb, LV_EVENT_CLICKED, NULL);

	ui_section_label(page, _("Sound"));
	card = ui_card_create(page);
	ringtone_value = settings_info_row(card, _("Ringtone"));
	ui_set_text(ringtone_value, _(ui_ringtone_name(ui_ringtone())));
	settings_row_tappable(lv_obj_get_parent(ringtone_value), ringtone_open_cb, NULL);
	ui_push(scr);
}
