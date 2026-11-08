/* Settings, Language: each one by its own name, the current one checked; the
 * keyboard's autocorrection, on unless turned off here */

#include "settings.h"

void language_summary(lv_obj_t *value)
{
	ui_set_text(value, ui_language()->name);
}

/* The screens are rebuilt in the new language, this one included */
static void language_reopen(void)
{
	settings_open();
	language_open();
}

static void autocorrect_cb(lv_event_t *e)
{
	ui_setting_set_int("autocorrect", lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}

static void language_cb(lv_event_t *e)
{
	const struct ui_language *l = lv_event_get_user_data(e);
	ui_language_set(l->code, language_reopen);
}

void language_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = settings_page(_("Language"), &page);

	lv_obj_t *card = ui_card_create(page);
	for (size_t i = 0; i < ui_language_count; i++) {
		const struct ui_language *l = ui_languages[i];
		lv_obj_t *row = settings_row(card);
		settings_row_tappable(row, language_cb, (void *)l);

		lv_obj_t *label = lv_label_create(row);
		lv_label_set_text_static(label, l->name);
		lv_obj_set_flex_grow(label, 1);
		if (l == ui_language()) {
			lv_obj_t *check = lv_label_create(row);
			lv_label_set_text_static(check, LV_SYMBOL_OK);
			lv_obj_set_style_text_color(check, lv_color_hex(UI_COLOR_ACCENT), 0);
		}
	}

	/* The keyboard's corrections of what is typed (correct.c): in a language with a dictionary */
	ui_section_label(page, _("Keyboard"));
	card = ui_card_create(page);
	lv_obj_t *row = settings_row(card);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text_static(label, _("Autocorrection"));
	lv_obj_set_flex_grow(label, 1);
	lv_obj_t *sw = ui_switch_create(row, ui_setting_get_int("autocorrect", 1));
	lv_obj_add_event_cb(sw, autocorrect_cb, LV_EVENT_VALUE_CHANGED, NULL);
	ui_push(scr);
}
