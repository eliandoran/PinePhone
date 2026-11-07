/*
 * A segmented control, as Clock's tabs: a pill for each choice in a rounded bar, the
 * one selected in the accent color. Each pill is a centered row with its name; more
 * can go after it (a count).
 */

#include "github.h"

lv_obj_t *segments_create(lv_obj_t *parent, lv_obj_t *segs[], unsigned count, const char *const names[],
			  lv_event_cb_t cb)
{
	lv_obj_t *bar = ui_box(parent);
	lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(bar, 16, 0);
	lv_obj_set_style_pad_bottom(bar, 8, 0);

	lv_obj_t *seg = ui_box(bar);
	lv_obj_set_size(seg, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_radius(seg, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(seg, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(seg, 6, 0);
	lv_obj_set_style_pad_column(seg, 6, 0);
	lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);

	for (uintptr_t i = 0; i < count; i++) {
		lv_obj_t *tab = segs[i] = ui_box(seg);
		lv_obj_set_height(tab, 72);
		lv_obj_set_flex_grow(tab, 1);
		lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(tab, 10, 0);
		lv_obj_set_style_radius(tab, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_STATE_CHECKED);
		lv_obj_set_style_bg_color(tab, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_CHECKED);
		lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(tab, lv_color_hex(UI_COLOR_BUTTON_PRESSED), LV_STATE_PRESSED);
		lv_obj_set_style_text_color(tab, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_set_style_text_color(tab, lv_color_white(), LV_STATE_CHECKED);
		lv_obj_set_clickable(tab, true);
		lv_obj_add_event_cb(tab, cb, LV_EVENT_CLICKED, (void *)i);
		lv_obj_set_state(tab, LV_STATE_CHECKED, i == 0);

		lv_obj_t *label = lv_label_create(tab);
		lv_label_set_text_static(label, names[i]);
		lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
	}
	return bar;
}

void segments_select(lv_obj_t *segs[], unsigned count, unsigned selected)
{
	for (unsigned i = 0; i < count; i++)
		lv_obj_set_state(segs[i], LV_STATE_CHECKED, i == selected);
}
