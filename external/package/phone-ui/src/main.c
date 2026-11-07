/*
 * Feature phone user interface on the DRM display (touchscreen input).
 *
 * ui.c has the shared pieces (theme, screens, status bar, wallpaper), home.c the home
 * screen and the menu, and every app has its file.
 *
 * Frames are paced by the display: the DRM EGL backend waits for the previous page
 * flip before queueing the next one, so refreshing as soon as something changed
 * renders once per vblank.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "ui.h"
#include "src/display/lv_display_private.h" /* the performance monitor label */
#include "src/indev/lv_indev_private.h"     /* the pressed object, for taps taken back */

static bool rendered;

#if LV_USE_PERF_MONITOR
/* Performance monitor with only FPS, CPU and the CPU frequency at that moment (LVGL's
 * also shows render and flush times) */
static void perf_cb(lv_observer_t *observer, lv_subject_t *subject)
{
	const lv_sysmon_perf_info_t *perf = lv_subject_get_pointer(subject);
	long khz;
	if (!ui_read_long("/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq", &khz))
		khz = 0;
	lv_label_set_text_fmt(lv_observer_get_target(observer),
	                      "%" LV_PRIu32 " FPS, %" LV_PRIu32 "%% CPU, %ld MHz", /* NON-NLS: development */
	                      perf->calculated.fps, perf->calculated.cpu, khz / 1000);
}

static void perf_monitor_init(lv_display_t *disp)
{
	lv_obj_remove_from_subject(disp->perf_label, disp->perf_sysmon_backend.subject);
	lv_subject_add_observer_obj(disp->perf_sysmon_backend.subject, perf_cb, disp->perf_label, NULL);
}
#endif

static void render_start_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	rendered = true;
}

/*
 * Taps taken back: LVGL keeps the pressed object (PRESS_LOCK) and clicks it wherever
 * the finger is let go. Here, as on other phones, a finger that leaves the object
 * (and a margin around it) shows it unpressed, and let go there does not click it;
 * back on it, it is pressed again. Scrolls and drags (RELEASED) are not concerned.
 */

#define TAP_MARGIN 40 /* px around the object */

static lv_indev_read_cb_t evdev_read;
static lv_obj_t *tap_unpressed; /* the object shown unpressed while the finger is out */

/* The last point LVGL processed (rotated with the display), on `obj` */
static bool tap_on(lv_indev_t *indev, lv_obj_t *obj)
{
	lv_point_t p;
	lv_area_t a;
	lv_indev_get_point(indev, &p);
	lv_obj_get_coords(obj, &a);
	return p.x >= a.x1 - TAP_MARGIN && p.x <= a.x2 + TAP_MARGIN &&
	       p.y >= a.y1 - TAP_MARGIN && p.y <= a.y2 + TAP_MARGIN;
}

static void tap_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	evdev_read(indev, data);
	lv_obj_t *obj = indev->pointer.act_obj;
	if (!obj || indev->pointer.scroll_obj || data->state != LV_INDEV_STATE_PRESSED) {
		tap_unpressed = NULL;
		return;
	}
	bool on = tap_on(indev, obj);
	if (!on && !tap_unpressed && lv_obj_has_state(obj, LV_STATE_PRESSED)) {
		lv_obj_remove_state(obj, LV_STATE_PRESSED);
		tap_unpressed = obj;
	} else if (on && tap_unpressed == obj) {
		lv_obj_add_state(obj, LV_STATE_PRESSED);
		tap_unpressed = NULL;
	}
}

static void tap_click_cb(lv_event_t *e)
{
	lv_indev_t *indev = lv_event_get_target(e);
	lv_obj_t *obj = lv_event_get_param(e);
	if (obj && !tap_on(indev, obj))
		lv_indev_stop_processing(indev); /* the object does not get it */
}

static void input_found_cb(lv_indev_t *indev, lv_evdev_type_t type, void *user_data)
{
	LV_UNUSED(user_data);
	printf("phone-ui: input device added (%s)\n",
	       type == LV_EVDEV_TYPE_ABS ? "touchscreen" :
	       type == LV_EVDEV_TYPE_REL ? "mouse" : "keys");
	if (type == LV_EVDEV_TYPE_KEY)
		ui_volume_keys(indev);
	if (type == LV_EVDEV_TYPE_ABS && lv_indev_get_read_cb(indev) != tap_read_cb) {
		evdev_read = lv_indev_get_read_cb(indev);
		lv_indev_set_read_cb(indev, tap_read_cb);
		lv_indev_add_event_cb(indev, tap_click_cb, LV_EVENT_SHORT_CLICKED, NULL);
		lv_indev_add_event_cb(indev, tap_click_cb, LV_EVENT_CLICKED, NULL);
	}
}

void ui_input_start(void)
{
	lv_evdev_discovery_start(input_found_cb, NULL);
}

/*
 * --bench [ms] [app]: runs the screen transitions by itself (slowed down to `ms`, into
 * `app`, Gallery by default), prints frame statistics for each one, then exits. Frame time includes waiting for the
 * previous page flip.
 */

static const char *bench_app = "Gallery"; /* NON-NLS: home.c's name */

static struct {
	const char *phase;
	uint32_t frames;
	uint64_t frame_us, max_us, start_us, cpu_us;
} stats;

static uint64_t time_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint64_t cpu_us(void)
{
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 +
	       ru.ru_utime.tv_usec + ru.ru_stime.tv_usec;
}

static void stats_end(void)
{
	if (!stats.phase)
		return;
	uint64_t wall = time_us() - stats.start_us;
	printf("%-12s %3u frames %5.1f FPS  frame avg %5.1f max %5.1f ms  CPU %3d%%\n",
	       stats.phase, stats.frames, stats.frames * 1e6 / wall,
	       stats.frames ? stats.frame_us / 1e3 / stats.frames : 0, stats.max_us / 1e3,
	       (int)((cpu_us() - stats.cpu_us) * 100 / wall));
	stats.phase = NULL;
}

static void stats_begin(const char *phase)
{
	stats_end();
	stats.phase = phase;
	stats.frames = 0;
	stats.frame_us = stats.max_us = 0;
	stats.start_us = time_us();
	stats.cpu_us = cpu_us();
}

static void bench_step(lv_timer_t *timer)
{
	static const char *const steps[] = { "home>menu", "menu>app", "app>menu", "menu>home" };
	static unsigned step;

	LV_UNUSED(timer);
	if (step == 3 * 4) {
		stats_end();
		exit(EXIT_SUCCESS);
	}
	stats_begin(steps[step % 4]);
	switch (step++ % 4) {
	case 0: ui_open_menu(); break;
	case 1: ui_app_open(ui_app_find(bench_app)); break;
	default: ui_pop(); break;
	}
}

int main(int argc, char **argv)
{
	/* --screenshot [file|-]: of the display as it is (the running phone-ui, an external app) */
	if (argc > 1 && strcmp(argv[1], "--screenshot") == 0)
		return ui_screenshot_save(argc > 2 ? argv[2] : NULL) ? EXIT_SUCCESS : EXIT_FAILURE;

	bool bench = argc > 1 && strcmp(argv[1], "--bench") == 0;
	if (bench && argc > 2)
		ui_anim_ms = atoi(argv[2]);
	if (bench && argc > 3 && ui_app_find(argv[3]))
		bench_app = argv[3];

	/* stdout goes to a log file: flush every line, the program never exits */
	setvbuf(stdout, NULL, _IOLBF, 0);

	settings_apply();
	ui_cpu_boost(false); /* its lowest (cpu.c), also after a run that exited boosted */
	lv_init();

	lv_display_t *disp = lv_linux_drm_create();
	if (lv_linux_drm_set_file(disp, "/dev/dri/card0", -1) != LV_RESULT_OK) {
		fprintf(stderr, "phone-ui: cannot use /dev/dri/card0\n");
		return EXIT_FAILURE;
	}
	/* The panel is 68 x 136 mm for 720 x 1440 (269 dpi): the theme's sizes
	 * (padding, radius, scrollbars, knobs) are scaled by dpi / 160 */
	lv_display_set_dpi(disp, 269);
#if LV_USE_PERF_MONITOR
	perf_monitor_init(disp);
#endif
	/* Refresh from the loop below; step animations on every pass (once per frame) */
	lv_display_delete_refr_timer(disp);
	lv_timer_set_period(lv_anim_get_timer(), 0);
	lv_display_add_event_cb(disp, render_start_cb, LV_EVENT_RENDER_START, NULL);

	ui_input_start();

	uint64_t start = time_us();
	ui_init(disp);
	ui_volume_init();
	external_scan();
	home_create();
	ui_splash();
	printf("phone-ui: ready in %llu ms\n", (unsigned long long)(time_us() - start) / 1000);
	if (bench)
		lv_timer_create(bench_step, ui_anim_ms + 800, NULL);
	/* --open <app>: start in that app, or in the menu with "Menu" (development) */
	if (argc > 2 && strcmp(argv[1], "--open") == 0 && ui_app_find(argv[2]))
		ui_app_open(ui_app_find(argv[2]));
	else if (argc > 2 && strcmp(argv[1], "--open") == 0 && strcmp(argv[2], "Menu") == 0)
		ui_open_menu();

	for (;;) {
		uint32_t idle = lv_timer_handler();

		rendered = false;
		start = time_us();
		/* Under LVGL's lock as the timers are: Web Lite builds long pages on a thread
		 * that measures text with the same fonts (browser/page.cpp) */
		lv_lock();
		lv_display_refr_timer(NULL);
		lv_unlock();
		if (rendered) {
			if (stats.phase) {
				uint64_t us = time_us() - start;
				stats.frames++;
				stats.frame_us += us;
				if (us > stats.max_us)
					stats.max_us = us;
			}
			continue;
		}
		if (stats.phase && !lv_anim_count_running())
			stats_end();

		/* Nothing changed: sleep one frame while animations run (e.g. waiting out
		 * a delay; their timer always reports ready), else until the next timer */
		if (idle == LV_NO_TIMER_READY || lv_anim_count_running())
			idle = LV_DEF_REFR_PERIOD;
		usleep((idle ? idle : 1) * 1000);
	}
}
