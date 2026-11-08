/*
 * The pieces of the shared UI (text.c, theme.c, nav.c, sysfs.c...) and the widgets the tested files use, without the rest of the
 * UI (hardware, settings, navigation). Screens are plain LVGL screens: ui_push()
 * loads one, nothing animates.
 */

#include <stdlib.h>
#include <string.h>

#include "ui.h"

/* theme.c's dark theme */
static const struct ui_colors dark = {
	.text = 0xffffff, .dim = 0xa0a0aa, .card = 0x2c2c34, .pressed = 0x3c3c46,
	.line = 0x3c3c46, .track = 0x48484f, .button = 0x3a3a42, .button_pressed = 0x56565e,
	.bar = 0x1c1c22, .shade = 0x000000,
};
const struct ui_colors *ui_colors = &dark;
bool ui_light;
uint32_t ui_anim_ms;

/* The same pointer for the same text, as text.c's */
const char *ui_intern(const char *text)
{
	static char **table;
	static size_t count;

	for (size_t i = 0; i < count; i++)
		if (strcmp(table[i], text) == 0)
			return table[i];
	table = realloc(table, (count + 1) * sizeof(*table));
	return table[count++] = strdup(text);
}

void ui_set_text(lv_obj_t *label, const char *text)
{
	lv_label_set_text_static(label, ui_intern(text));
}

lv_obj_t *ui_box(lv_obj_t *parent)
{
	lv_obj_t *box = lv_obj_create(parent);
	lv_obj_remove_style_all(box);
	lv_obj_set_clickable(box, false);
	lv_obj_set_scrollable(box, false);
	return box;
}

lv_obj_t *ui_app_screen_create(const char *title, lv_obj_t **header, lv_obj_t **body)
{
	LV_UNUSED(title);
	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_t *content = ui_box(scr);
	lv_obj_set_size(content, LV_PCT(100), LV_VER_RES - UI_STATUS_H - UI_HEADER_H);
	lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	if (header)
		*header = NULL;
	*body = content;
	return scr;
}

void ui_push(lv_obj_t *screen)
{
	lv_screen_load(screen);
}

void ui_vibrate(uint32_t ms)
{
	LV_UNUSED(ms);
}

lv_obj_t *ui_avatar_create(lv_obj_t *parent, int32_t size)
{
	lv_obj_t *avatar = ui_box(parent);
	lv_obj_set_size(avatar, size, size);
	lv_label_set_text_static(lv_label_create(avatar), "");
	return avatar;
}

void ui_avatar_set(lv_obj_t *avatar, uint32_t color, const char *text)
{
	lv_obj_set_style_bg_color(avatar, lv_color_hex(color), 0);
	ui_set_text(lv_obj_get_child(avatar, 0), text);
}
