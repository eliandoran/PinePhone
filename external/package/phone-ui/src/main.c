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

static void flog_read(const lv_indev_data_t *data);

static void tap_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	evdev_read(indev, data);
	flog_read(data);
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

/*
 * The CPU at a fixed frequency while the user interacts (finger down, scrolling,
 * animations) and for a second after. schedutil alone keeps changing the frequency while
 * the touchscreen reports: each change runs the clock driver's PLL search and a PMIC
 * voltage write in a deadline-priority thread (sugov) that preempts rendering; it took 15%
 * of the CPU and dragging dropped to about 40 FPS. Replaying the same touches without the
 * touchscreen's interrupts did not, and ran at 60 FPS. Pinned at the lowest frequency
 * (648 MHz, 1.04 V instead of 1.3 V at 1152): the menu scrolls at 66 FPS there, about
 * half a core. Plugin games ask for full speed instead (ui_cpu_boost()), which wins.
 */

#define CPUFREQ "/sys/devices/system/cpu/cpufreq/policy0/"

static bool cpu_boosted, cpu_pinned;

static void cpu_apply(void)
{
	long min, max;
	if (!ui_read_long(CPUFREQ "cpuinfo_min_freq", &min) || !ui_read_long(CPUFREQ "cpuinfo_max_freq", &max))
		return;
	ui_write_long(CPUFREQ "scaling_min_freq", cpu_boosted ? max : min);
	ui_write_long(CPUFREQ "scaling_max_freq", cpu_pinned && !cpu_boosted ? min : max);
}

void ui_cpu_boost(bool on)
{
	cpu_boosted = on;
	if (!on)
		cpu_pinned = false; /* also before an external app, which blocks the loop */
	cpu_apply();
}

static void cpu_pin_update(void)
{
	static uint64_t until;

	bool active = lv_anim_count_running() > 0;
	for (lv_indev_t *i = lv_indev_get_next(NULL); i && !active; i = lv_indev_get_next(i))
		active = lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER &&
		         (lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED || lv_indev_get_scroll_obj(i));
	uint64_t now = time_us();
	if (active)
		until = now + 1000000;
	if (cpu_pinned != (now < until)) {
		cpu_pinned = !cpu_pinned;
		cpu_apply();
	}
}

/*
 * Frame log while the shade is pulled (shown, a finger on the touchscreen): each
 * second, frames drawn and the touchscreen's readings (those that moved), where a
 * frame's time went, the area drawn, CPU; and each frame drawn more than SLOW_US
 * after the previous one. Time in a frame: events (lv_timer_handler(): readings,
 * the shade following, animations), layout, draw (LVGL's draw tasks into GL),
 * swap (the display texture, eglSwapBuffers, waiting for the GPU and the previous page
 * flip). Slept: in the loop, nothing to draw.
 */

#define SLOW_US 20000

enum { EVENTS, LAYOUT, DRAW, SWAP, SLEPT, PHASES };

static struct {
	bool on, touching;
	lv_point_t point;                /* the last reading */
	uint64_t second, cpu;            /* when this second began, CPU time then */
	uint64_t last;                   /* the previous frame drawn ended */
	uint64_t refr, render, flush;    /* this frame's events */
	uint64_t us[PHASES], sum[PHASES], max_frame;
	uint32_t px, sum_px;             /* drawn in this frame, this second */
	uint32_t readings, moved, frame_moved;
	uint32_t frames, slow;
} flog;

static void flog_read(const lv_indev_data_t *data)
{
	flog.touching = data->state == LV_INDEV_STATE_PRESSED;
	if (!flog.on)
		return;
	flog.readings++;
	if (data->point.x != flog.point.x || data->point.y != flog.point.y) {
		flog.moved++;
		flog.frame_moved++;
	}
	flog.point = data->point;
}

static void flog_second(const char *why)
{
	uint64_t wall = time_us() - flog.second;
	if (!wall)
		return;
	uint32_t n = flog.frames ? flog.frames : 1;
	long khz;
	if (!ui_read_long(CPUFREQ "scaling_cur_freq", &khz))
		khz = 0;
	printf("phone-ui: frames: %s%u in %llu ms (%u slow, longest %.1f ms), readings %u, moved %u; "
	       "per frame events %.1f layout %.1f draw %.1f swap %.1f ms, %u kpx; "
	       "slept %llu ms, CPU %d%% at %ld MHz\n",
	       why, flog.frames, (unsigned long long)wall / 1000, flog.slow, flog.max_frame / 1e3,
	       flog.readings, flog.moved, flog.sum[EVENTS] / 1e3 / n, flog.sum[LAYOUT] / 1e3 / n,
	       flog.sum[DRAW] / 1e3 / n, flog.sum[SWAP] / 1e3 / n, flog.sum_px / n / 1000,
	       (unsigned long long)flog.sum[SLEPT] / 1000, (int)((cpu_us() - flog.cpu) * 100 / wall),
	       khz / 1000);
	flog.second = time_us();
	flog.cpu = cpu_us();
	memset(flog.sum, 0, sizeof(flog.sum));
	flog.max_frame = 0;
	flog.sum_px = flog.readings = flog.moved = flog.frames = flog.slow = 0;
}

static void flog_update(void)
{
	bool on = flog.touching && ui_shade_shown();
	if (on == flog.on)
		return;
	if (on) {
		flog.on = true;
		flog.second = flog.last = time_us();
		flog.cpu = cpu_us();
		memset(flog.us, 0, sizeof(flog.us));
		memset(flog.sum, 0, sizeof(flog.sum));
		flog.max_frame = 0;
		flog.sum_px = flog.readings = flog.moved = flog.frame_moved = flog.frames = flog.slow = 0;
		printf("phone-ui: frames: the shade pulled\n");
	} else {
		flog_second("until let go: ");
		flog.on = false;
	}
}

static void flog_event_cb(lv_event_t *e)
{
	if (!flog.on)
		return;
	uint64_t now = time_us();
	switch (lv_event_get_code(e)) {
	case LV_EVENT_REFR_START:
		flog.refr = flog.render = flog.flush = now;
		flog.px = 0;
		break;
	case LV_EVENT_RENDER_START: flog.render = now; break;
	case LV_EVENT_FLUSH_START:
		flog.px += lv_area_get_size(lv_event_get_param(e));
		if (flog.flush == flog.refr)
			flog.flush = now;
		break;
	default: break;
	}
}

/* After a refresh that drew, the loop's events before it taking `events_us` */
static void flog_frame(uint64_t events_us)
{
	if (!flog.on)
		return;
	uint64_t now = time_us();
	flog.us[EVENTS] = events_us;
	flog.us[LAYOUT] = flog.render - flog.refr;
	flog.us[DRAW] = flog.flush - flog.render;
	flog.us[SWAP] = now - flog.flush;
	uint64_t since = now - flog.last;
	if (since > SLOW_US) {
		flog.slow++;
		printf("phone-ui: frames: slow, %.1f ms since the previous: events %.1f layout %.1f "
		       "draw %.1f swap %.1f ms, slept %.1f ms before, %u kpx, %u readings moved\n",
		       since / 1e3, events_us / 1e3, flog.us[LAYOUT] / 1e3, flog.us[DRAW] / 1e3,
		       flog.us[SWAP] / 1e3, flog.us[SLEPT] / 1e3, flog.px / 1000, flog.frame_moved);
	}
	for (int i = 0; i < SLEPT; i++) /* slept: added as it goes */
		flog.sum[i] += flog.us[i];
	if (since > flog.max_frame)
		flog.max_frame = since;
	flog.sum_px += flog.px;
	flog.frames++;
	flog.frame_moved = 0;
	flog.us[SLEPT] = 0;
	flog.last = now;
	if (now - flog.second >= 1000000)
		flog_second("");
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
	ui_cpu_boost(false); /* a previous run may have exited boosted or pinned */
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
	lv_display_add_event_cb(disp, flog_event_cb, LV_EVENT_REFR_START, NULL);
	lv_display_add_event_cb(disp, flog_event_cb, LV_EVENT_RENDER_START, NULL);
	lv_display_add_event_cb(disp, flog_event_cb, LV_EVENT_FLUSH_START, NULL);

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
		uint64_t events = time_us();
		uint32_t idle = lv_timer_handler();
		cpu_pin_update();
		flog_update();

		rendered = false;
		start = time_us();
		events = start - events;
		lv_display_refr_timer(NULL);
		if (rendered) {
			flog_frame(events);
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
		uint64_t slept = time_us();
		usleep((idle ? idle : 1) * 1000);
		if (flog.on) {
			slept = time_us() - slept;
			flog.us[SLEPT] += slept;
			flog.sum[SLEPT] += slept;
		}
	}
}
