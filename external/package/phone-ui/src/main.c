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

static bool rendered;

#if LV_USE_PERF_MONITOR
/* Performance monitor with only FPS and CPU (LVGL's also shows render and flush times) */
static void perf_cb(lv_observer_t *observer, lv_subject_t *subject)
{
	const lv_sysmon_perf_info_t *perf = lv_subject_get_pointer(subject);
	lv_label_set_text_fmt(lv_observer_get_target(observer), "%" LV_PRIu32 " FPS, %" LV_PRIu32 "%% CPU",
	                      perf->calculated.fps, perf->calculated.cpu);
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

static void input_found_cb(lv_indev_t *indev, lv_evdev_type_t type, void *user_data)
{
	LV_UNUSED(indev);
	LV_UNUSED(user_data);
	printf("phone-ui: input device added (%s)\n",
	       type == LV_EVDEV_TYPE_ABS ? "touchscreen" :
	       type == LV_EVDEV_TYPE_REL ? "mouse" : "keys");
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

static const char *bench_app = "Gallery";

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
 * The CPU at full speed while the user interacts (finger down, scrolling, animations) and
 * for a second after. schedutil alone keeps changing the frequency while the touchscreen
 * reports: each change runs the clock driver's PLL search and a PMIC voltage write in a
 * deadline-priority thread (sugov) that preempts rendering; it took 15% of the CPU and
 * dragging dropped to about 40 FPS. Replaying the same touches without the touchscreen's
 * interrupts did not, and ran at 60 FPS.
 */

#define CPUFREQ "/sys/devices/system/cpu/cpufreq/policy0/"

void ui_cpu_boost(bool on)
{
	long khz;
	if (ui_read_long(on ? CPUFREQ "cpuinfo_max_freq" : CPUFREQ "cpuinfo_min_freq", &khz))
		ui_write_long(CPUFREQ "scaling_min_freq", khz);
}

static void cpu_boost_update(void)
{
	static bool boosted;
	static uint64_t until;

	bool active = lv_anim_count_running() > 0;
	for (lv_indev_t *i = lv_indev_get_next(NULL); i && !active; i = lv_indev_get_next(i))
		active = lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER &&
		         (lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED || lv_indev_get_scroll_obj(i));
	uint64_t now = time_us();
	if (active)
		until = now + 1000000;
	if (boosted != (now < until)) {
		boosted = !boosted;
		ui_cpu_boost(boosted);
	}
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
	bool bench = argc > 1 && strcmp(argv[1], "--bench") == 0;
	if (bench && argc > 2)
		ui_anim_ms = atoi(argv[2]);
	if (bench && argc > 3 && ui_app_find(argv[3]))
		bench_app = argv[3];

	/* stdout goes to a log file: flush every line, the program never exits */
	setvbuf(stdout, NULL, _IOLBF, 0);

	settings_apply();
	ui_cpu_boost(false); /* a previous run may have exited boosted */
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
	external_scan();
	home_create();
	printf("phone-ui: ready in %llu ms\n", (unsigned long long)(time_us() - start) / 1000);
	if (bench)
		lv_timer_create(bench_step, ui_anim_ms + 800, NULL);
	/* --open <app>: start in that app (development) */
	if (argc > 2 && strcmp(argv[1], "--open") == 0 && ui_app_find(argv[2]))
		ui_app_open(ui_app_find(argv[2]));

	for (;;) {
		uint32_t idle = lv_timer_handler();
		cpu_boost_update();

		rendered = false;
		start = time_us();
		lv_display_refr_timer(NULL);
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
