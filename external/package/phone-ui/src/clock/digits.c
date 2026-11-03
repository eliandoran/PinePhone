/*
 * Clock: fixed-width running digits, see clock.h.
 */

#include "clock.h"

void digits_create(struct digits *d, lv_obj_t *parent, const char *pattern)
{
	static const char *const separators[] = { ":", "." };

	lv_obj_t *row = ui_box(parent);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_text_font(row, &ui_font_48, 0);

	d->count = 0;
	for (const char *c = pattern; *c && d->count < 8; c++) {
		lv_obj_t *cell = lv_label_create(row);
		bool sep = *c == ':' || *c == '.';
		lv_obj_set_width(cell, sep ? 20 : 38);
		lv_obj_set_style_text_align(cell, LV_TEXT_ALIGN_CENTER, 0);
		lv_label_set_text_static(cell, sep ? separators[*c == '.'] : "0");
		d->cells[d->count++] = cell;
	}
}

void digits_set(struct digits *d, const char *text)
{
	static const char *const nums[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9" };

	for (unsigned i = 0; i < d->count && text[i]; i++) {
		if (text[i] < '0' || text[i] > '9')
			continue;
		const char *s = nums[text[i] - '0'];
		if (lv_label_get_text(d->cells[i]) != s)
			lv_label_set_text_static(d->cells[i], s);
	}
}
