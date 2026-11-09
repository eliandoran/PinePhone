/*
 * The shared UI started: the theme, the status bar and the shade, the transitions
 * and gestures, and what follows the display when it turns. The pieces: nav.c
 * (screens), transition.c, gesture.c, status.c, theme.c, wallpaper.c,
 * battery_icon.c, usb_net.c, text.c, prefs.c, sysfs.c, apps.c.
 */

#include "net/pairing.h"
#include "ui_internal.h"
#include "src/display/lv_display_private.h" /* the performance monitor's label */

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

static uint32_t (*monitor_frames)(void);
static uint32_t monitor_shown; /* its frames at the last report */

void ui_fps_counter_monitor(uint32_t (*frames)(void))
{
	monitor_frames = frames;
	monitor_shown = frames ? frames() : 0;
}

#if LV_USE_PERF_MONITOR
static uint32_t per_second(uint32_t frames, uint32_t ms)
{
	return ms ? (frames * 1000 + ms / 2) / ms : 0;
}

/* The counter's text, in place of LVGL's (which also shows render and flush times): the
 * frames drawn a second since the last report (LVGL's figure counts the refreshes asked
 * for, drawn or not: the display's rate whatever was drawn), those a monitor showed, the
 * CPU and its frequency at that moment */
static void perf_cb(lv_observer_t *observer, lv_subject_t *subject)
{
	const lv_sysmon_perf_info_t *perf = lv_subject_get_pointer(subject);
	uint32_t ms = lv_tick_elaps(perf->measured.last_report_timestamp);
	char monitor[32] = "";
	if (monitor_frames) {
		uint32_t shown = monitor_frames();
		snprintf(monitor, sizeof(monitor), ", monitor %" LV_PRIu32, /* NON-NLS: development */
		         per_second(shown - monitor_shown, ms));
		monitor_shown = shown;
	}
	long khz;
	if (!ui_read_long("/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq", &khz))
		khz = 0;
	lv_label_set_text_fmt(lv_observer_get_target(observer),
	                      "%" LV_PRIu32 " FPS%s, %" LV_PRIu32 "%% CPU, %ld MHz", /* NON-NLS: development */
	                      per_second(perf->measured.render_cnt, ms), monitor, perf->calculated.cpu, khz / 1000);
}
#endif

void ui_init(lv_display_t *disp)
{
	ui_screen_w = lv_display_get_horizontal_resolution(disp);
	ui_screen_h = lv_display_get_vertical_resolution(disp);
#if LV_USE_PERF_MONITOR
	lv_obj_remove_from_subject(disp->perf_label, disp->perf_sysmon_backend.subject);
	lv_subject_add_observer_obj(disp->perf_sysmon_backend.subject, perf_cb, disp->perf_label, NULL);
#endif
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
