/*
 * The shared UI started: the theme, the status bar and the shade, the transitions
 * and gestures, and what follows the display when it turns. The pieces: nav.c
 * (screens), transition.c, gesture.c, status.c, theme.c, wallpaper.c,
 * battery_icon.c, usb_net.c, text.c, prefs.c, sysfs.c, apps.c.
 */

#include "net/pairing.h"
#include "ui_internal.h"

uint32_t ui_anim_ms = 220;

int32_t ui_screen_w, ui_screen_h;

/* The display turned (home.c): what was made for its size, made again for the new one */
static void resolution_cb(lv_event_t *e)
{
	lv_display_t *disp = lv_event_get_target(e);
	ui_screen_w = lv_display_get_horizontal_resolution(disp);
	ui_screen_h = lv_display_get_vertical_resolution(disp);
#if LV_USE_3DTEXTURE
	transition_resize();
	gestures_resize();
#endif
	nav_refit_wallpapers();
}

void ui_fps_counter_set(bool on)
{
#if LV_USE_PERF_MONITOR
	if (on)
		lv_sysmon_show_performance(lv_display_get_default());
	else
		lv_sysmon_hide_performance(lv_display_get_default());
#else
	LV_UNUSED(on);
#endif
}

void ui_init(lv_display_t *disp)
{
	ui_screen_w = lv_display_get_horizontal_resolution(disp);
	ui_screen_h = lv_display_get_vertical_resolution(disp);
	theme_apply(ui_setting_get_int("light", 0) != 0);
	ui_fps_counter_set(ui_setting_get_int("fps_counter", 0) != 0);

	ui_shade_init(status_init());
	pairing_init();
#if LV_USE_3DTEXTURE
	transition_init(disp);
	gestures_init();
#endif
	lv_display_add_event_cb(disp, resolution_cb, LV_EVENT_RESOLUTION_CHANGED, NULL);
}
