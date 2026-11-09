/*
 * What a monitor shows (ui_monitor_fill()): main.c mirrors the display on one connected
 * over USB-C (LVGL's DRM driver, external/patches/lvgl/0023-…), all of it, or the area
 * of an object an app asked for while its screen is on top and uncovered.
 */

#include "ui/ui_internal.h"

static lv_obj_t *fill;

static void fill_delete_cb(lv_event_t *e)
{
	if (lv_event_get_target(e) == fill)
		fill = NULL;
}

void ui_monitor_fill(lv_obj_t *obj)
{
	if (fill)
		lv_obj_remove_event_cb_with_user_data(fill, fill_delete_cb, NULL);
	fill = obj;
	if (obj)
		lv_obj_add_event_cb(obj, fill_delete_cb, LV_EVENT_DELETE, NULL);
}

bool ui_monitor_area(lv_area_t *area)
{
	if (!fill || lv_obj_is_hidden(fill))
		return false;
	lv_obj_t *scr = lv_obj_get_screen(fill);
	if (scr != ui_current() || scr != lv_screen_active() || ui_locked() || ui_shade_shown())
		return false;
	lv_obj_get_coords(fill, area);
	return lv_area_get_width(area) > 1 && lv_area_get_height(area) > 1;
}
