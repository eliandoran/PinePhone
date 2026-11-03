/*
 * Assistant's settings: the API key of each provider, the model, thinking and
 * web search.
 */

#include <string.h>

#include "assistant.h"

static struct {
	lv_obj_t *kb;
	lv_obj_t *keys[MAX_PROVIDERS];
	lv_obj_t *checks[MAX_PROVIDERS * MAX_MODELS];
} st;

static void keys_save(void)
{
	for (unsigned i = 0; i < ai_provider_count; i++) {
		if (!st.keys[i])
			continue;
		/* Pasted or typed with spaces around */
		char key[200];
		const char *text = lv_textarea_get_text(st.keys[i]);
		while (*text == ' ')
			text++;
		snprintf(key, sizeof(key), "%s", text);
		size_t n = strlen(key);
		while (n && key[n - 1] == ' ')
			key[--n] = '\0';
		ui_setting_set(ai_providers[i].key_setting, key);
	}
}

static void key_cb(lv_event_t *e)
{
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		lv_keyboard_set_textarea(st.kb, lv_event_get_target_obj(e));
		ui_keyboard_show(st.kb, true);
		break;
	case LV_EVENT_READY:
	case LV_EVENT_CANCEL:
		ui_keyboard_show(st.kb, false);
		keys_save();
		break;
	default:
		break;
	}
}

static void model_cb(lv_event_t *e)
{
	const struct model *m = lv_event_get_user_data(e);
	ui_vibrate(15);
	ui_setting_set("assistant.model", m->id);
	const struct model *cur = ai_model_current(NULL);
	unsigned k = 0;
	for (unsigned p = 0; p < ai_provider_count; p++)
		for (const struct model *pm = ai_providers[p].models; pm->id; pm++, k++)
			lv_obj_set_hidden(st.checks[k], pm != cur);
}

static void switch_cb(lv_event_t *e)
{
	lv_obj_t *sw = lv_event_get_target_obj(e);
	ui_vibrate(15);
	ui_setting_set_int(lv_event_get_user_data(e), lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/* A row of the card with a switch for the setting `key` (0 or 1, off unless set) */
static void switch_row(lv_obj_t *card, bool divider, const char *title, const char *info, const char *key)
{
	lv_obj_t *row = ui_row_create(card, ROW_H, divider, NULL, NULL);
	lv_obj_t *text = ui_row_text(row);
	lv_obj_set_width(ui_one_line_label(text, title, &ui_font_26), LV_PCT(100));
	lv_obj_set_width(ui_dim_label(text, info, &ui_font_20), LV_PCT(100));

	lv_obj_t *sw = lv_switch_create(row);
	lv_obj_set_size(sw, 100, 56);
	lv_obj_set_ext_click_area(sw, 16);
	lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
	lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_pad_all(sw, -4, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
	lv_obj_set_style_outline_width(sw, 0, LV_STATE_FOCUSED);
	lv_obj_set_state(sw, LV_STATE_CHECKED, ui_setting_get_int(key, 0));
	lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, (void *)key);
}

static void settings_unload_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	keys_save();
}

static void settings_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	memset(&st, 0, sizeof(st));
}

void ai_settings_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Assistant settings", NULL, &body);
	lv_obj_add_event_cb(scr, settings_unload_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
	lv_obj_add_event_cb(scr, settings_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_scrollable(body, false);

	/* Scrolls above the keyboard */
	lv_obj_t *content = ui_box(body);
	lv_obj_set_width(content, LV_PCT(100));
	lv_obj_set_flex_grow(content, 1);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(content, 16, 0);
	lv_obj_set_style_pad_bottom(content, 32, 0);
	lv_obj_set_style_pad_row(content, 12, 0);
	lv_obj_set_scrollable(content, true);
	lv_obj_set_clickable(content, true);
	lv_obj_set_scroll_dir(content, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

	const struct model *cur = ai_model_current(NULL);
	unsigned k = 0;
	for (unsigned p = 0; p < ai_provider_count; p++) {
		const struct provider *pr = &ai_providers[p];
		ui_section_label(content, pr->name);
		lv_obj_t *ta = ui_text_field(content, 88, 190, key_cb, NULL);
		lv_obj_set_width(ta, LV_PCT(100));
		lv_textarea_set_placeholder_text(ta, "API key");
		lv_textarea_set_text(ta, ai_provider_key(pr));
		st.keys[p] = ta;
		lv_obj_t *hint = ui_dim_label(content, pr->key_hint, &ui_font_20);
		lv_obj_set_width(hint, LV_PCT(100));
		lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_pad_hor(hint, 28, 0);

		ui_section_label(content, "Model");
		lv_obj_t *card = ui_card_create(content);
		for (const struct model *m = pr->models; m->id; m++, k++) {
			lv_obj_t *row = ui_row_create(card, ROW_H, m[1].id != NULL, model_cb, (void *)m);
			lv_obj_t *text = ui_row_text(row);
			lv_obj_set_width(ui_one_line_label(text, m->id, &ui_font_26), LV_PCT(100));
			lv_obj_set_width(ui_dim_label(text, m->info, &ui_font_20), LV_PCT(100));
			st.checks[k] = lv_label_create(row);
			lv_label_set_text_static(st.checks[k], LV_SYMBOL_OK);
			lv_obj_set_style_text_color(st.checks[k], lv_color_hex(UI_COLOR_ACCENT), 0);
			lv_obj_set_hidden(st.checks[k], m != cur);
		}
		if (pr->thinking || pr->search_url) {
			card = ui_card_create(content);
			lv_obj_set_style_margin_top(card, 16, 0);
		}
		if (pr->thinking)
			switch_row(card, pr->search_url != NULL, "Thinking", "Better answers, takes much longer",
			           "assistant.thinking");
		if (pr->search_url)
			switch_row(card, false, "Web search", "Looks things up, $0.75 per 1000 searches",
			           "assistant.search");
	}

	st.kb = ui_keyboard_create(body, st.keys[0]);
	ui_push(scr);
}

