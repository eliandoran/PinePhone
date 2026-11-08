/*
 * The battery icon: its state in the user data, the level + 2 (0 unknown, 1 absent) and
 * the charging and percentage bits above, drawn by battery_draw_cb() at the object's
 * size. The body's outline is half opaque, the fill and nub whole: the level reads
 * first, as the signal's lit bars do. With the percentage (the status bar's, if asked)
 * the number is inside instead of the bolt, as the iPhone's.
 */

#include "ui_internal.h"
#include "src/misc/lv_area_private.h" /* the battery's number clipped to its fill */

#define BATTERY_CHARGING 0x100
#define BATTERY_PERCENT 0x200

/* `label`'s text drawn only within `clip` */
static void draw_label_clipped(lv_layer_t *layer, lv_draw_label_dsc_t *label, const lv_area_t *area,
                               const lv_area_t *clip)
{
	lv_area_t old = layer->_clip_area, in;
	if (!lv_area_intersect(&in, &old, clip))
		return;
	layer->_clip_area = in;
	lv_draw_label(layer, label, area);
	layer->_clip_area = old;
}

static void battery_draw_cb(lv_event_t *e)
{
	lv_obj_t *obj = lv_event_get_current_target_obj(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	uintptr_t s = (uintptr_t)lv_obj_get_user_data(obj);
	int level = (int)(s & 0xff) - 2;
	bool charging = s & BATTERY_CHARGING;
	lv_color_t color = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	int32_t h = lv_area_get_height(&a);
	int32_t stroke = LV_MAX(h / 10, 2), gap = LV_MAX(stroke / 2, 1);
	int32_t nub = LV_MAX(h / 8, 2);

	/* The body, then the nub on the right, 1 px apart */
	lv_area_t body = { a.x1, a.y1, a.x2 - nub - 1, a.y2 };
	lv_draw_rect_dsc_t rect;
	lv_draw_rect_dsc_init(&rect);
	rect.radius = h / 4;
	rect.bg_opa = LV_OPA_TRANSP;
	rect.border_color = color;
	rect.border_width = stroke;
	rect.border_opa = LV_OPA_50;
	lv_draw_rect(layer, &rect, &body);

	lv_draw_rect_dsc_init(&rect);
	rect.bg_color = color;
	rect.bg_opa = LV_OPA_50;
	rect.radius = nub / 2;
	lv_area_t tip = { a.x2 - nub + 1, a.y1 + h * 3 / 10, a.x2, a.y2 - h * 3 / 10 };
	lv_draw_rect(layer, &rect, &tip);

	lv_area_t in = { body.x1 + stroke + gap, body.y1 + stroke + gap,
	                 body.x2 - stroke - gap, body.y2 - stroke - gap };
	int32_t in_w = lv_area_get_width(&in);

	if (level == UI_BATTERY_ABSENT) {
		/* Crossed out in red, corner to corner */
		lv_draw_line_dsc_t line;
		lv_draw_line_dsc_init(&line);
		line.color = lv_color_hex(UI_COLOR_RED);
		line.width = stroke + 1;
		line.round_start = line.round_end = 1;
		line.p1 = (lv_point_precise_t){ body.x1 + stroke, body.y2 - h / 6 };
		line.p2 = (lv_point_precise_t){ body.x2 - stroke, body.y1 + h / 6 };
		lv_draw_line(layer, &line);
		return;
	}
	/* A "?" or the bolt, centered on the inside */
	lv_draw_label_dsc_t label;
	lv_draw_label_dsc_init(&label);
	label.color = color;
	label.font = h < 40 ? &ui_font_16 : &ui_font_32;
	label.align = LV_TEXT_ALIGN_CENTER;
	lv_area_t text = in;
	int32_t font_h = lv_font_get_line_height(label.font);
	text.y1 = (in.y1 + in.y2 + 1 - font_h) / 2;
	text.y2 = text.y1 + font_h - 1;
	if (level == UI_BATTERY_UNKNOWN) {
		label.text = "?";
		lv_draw_label(layer, &label, &text);
		return;
	}

	int32_t fill_w = level <= 0 ? 0 : LV_MAX((in_w * level + 50) / 100, 2);
	lv_area_t fill = { in.x1, in.y1, in.x1 + fill_w - 1, in.y2 };
	if (fill_w > 0) {
		lv_draw_rect_dsc_init(&rect);
		rect.bg_color = charging ? lv_color_hex(UI_COLOR_GREEN) :
		                level < 15 ? lv_color_hex(UI_COLOR_RED) : color;
		rect.radius = LV_MAX(h / 4 - stroke - gap, 1);
		lv_draw_rect(layer, &rect, &fill);
	}

	/* The number, cut out of the fill (the shade's color, the wallpaper's side) and in
	 * the text color past it */
	if (s & BATTERY_PERCENT) {
		char number[4];
		lv_snprintf(number, sizeof(number), "%d", level);
		label.text = number;
		label.font = &ui_font_16;
		font_h = lv_font_get_line_height(label.font);
		text.y1 = (in.y1 + in.y2 + 1 - font_h) / 2;
		text.y2 = text.y1 + font_h - 1;
		lv_area_t rest = { fill.x2 + 1, a.y1, a.x2, a.y2 };
		if (fill_w > 0) {
			label.color = lv_color_hex(UI_COLOR_SHADE);
			fill.y1 = a.y1, fill.y2 = a.y2; /* the digits may pass the inside's height */
			draw_label_clipped(layer, &label, &text, &fill);
		}
		label.color = color;
		draw_label_clipped(layer, &label, &text, &rest);
		return;
	}

	/* The bolt in the text color: it reads over the fill and the empty part alike */
	if (charging) {
		label.text = LV_SYMBOL_CHARGE;
		lv_draw_label(layer, &label, &text);
	}
}

lv_obj_t *ui_battery_icon_create(lv_obj_t *parent, int32_t w, int32_t h)
{
	lv_obj_t *icon = ui_box(parent);
	lv_obj_set_size(icon, w, h);
	lv_obj_set_user_data(icon, (void *)(uintptr_t)(UI_BATTERY_UNKNOWN + 2));
	lv_obj_add_event_cb(icon, battery_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
	return icon;
}

bool battery_icon_percent(lv_obj_t *icon, bool on)
{
	uintptr_t s = (uintptr_t)lv_obj_get_user_data(icon);
	bool was = s & BATTERY_PERCENT;
	if (was != on) {
		lv_obj_set_user_data(icon, (void *)(on ? s | BATTERY_PERCENT : s & ~(uintptr_t)BATTERY_PERCENT));
		lv_obj_invalidate(icon);
	}
	return was;
}

void ui_battery_icon_set(lv_obj_t *icon, int level, bool charging)
{
	level = LV_CLAMP(UI_BATTERY_ABSENT, level, 100);
	uintptr_t old = (uintptr_t)lv_obj_get_user_data(icon);
	uintptr_t s = (uintptr_t)(level + 2) | (charging && level >= 0 ? BATTERY_CHARGING : 0) |
	              (old & BATTERY_PERCENT);
	if (old == s)
		return;
	lv_obj_set_user_data(icon, (void *)s);
	lv_obj_invalidate(icon);
}

void ui_battery_icon_update(lv_obj_t *icon)
{
	long capacity;
	char status[32];
	if (!ui_read_long(UI_BATTERY "/capacity", &capacity))
		ui_battery_icon_set(icon, UI_BATTERY_UNKNOWN, false);
	else if (!ui_battery_present())
		ui_battery_icon_set(icon, UI_BATTERY_ABSENT, false);
	else {
		ui_battery_status(status, sizeof(status));
		ui_battery_icon_set(icon, (int)capacity, ui_battery_plugged(status));
	}
}

bool ui_battery_icon_get(lv_obj_t *obj, int *level, bool *charging)
{
	uint32_t n = lv_obj_get_event_count(obj);
	bool found = false;
	for (uint32_t i = 0; i < n && !found; i++)
		found = lv_event_dsc_get_cb(lv_obj_get_event_dsc(obj, i)) == battery_draw_cb;
	if (!found)
		return false;
	uintptr_t s = (uintptr_t)lv_obj_get_user_data(obj);
	if (level)
		*level = (int)(s & 0xff) - 2;
	if (charging)
		*charging = s & BATTERY_CHARGING;
	return true;
}
