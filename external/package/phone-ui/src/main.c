/*
 * Feature phone user interface on the DRM display (touchscreen input).
 *
 * ui.c has the shared pieces (screens, status bar, wallpaper), home.c the home
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

static bool rendered;

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

/*
 * --bench [ms]: runs the screen transitions by itself (slowed down to `ms`), prints
 * frame statistics for each one, then exits. Frame time includes waiting for the
 * previous page flip.
 */

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
	case 1: ui_app_open(ui_app_find("Gallery")); break;
	default: ui_pop(); break;
	}
}

int main(int argc, char **argv)
{
	bool bench = argc > 1 && strcmp(argv[1], "--bench") == 0;
	if (bench && argc > 2)
		ui_anim_ms = atoi(argv[2]);

	/* stdout goes to a log file: flush every line, the program never exits */
	setvbuf(stdout, NULL, _IOLBF, 0);

	settings_apply();
	lv_init();

	lv_display_t *disp = lv_linux_drm_create();
	if (lv_linux_drm_set_file(disp, "/dev/dri/card0", -1) != LV_RESULT_OK) {
		fprintf(stderr, "phone-ui: cannot use /dev/dri/card0\n");
		return EXIT_FAILURE;
	}
#if LV_USE_PERF_MONITOR
	lv_sysmon_hide_performance(disp);
#endif

	/* Refresh from the loop below; step animations on every pass (once per frame) */
	lv_display_delete_refr_timer(disp);
	lv_timer_set_period(lv_anim_get_timer(), 0);
	lv_display_add_event_cb(disp, render_start_cb, LV_EVENT_RENDER_START, NULL);

	lv_display_set_theme(disp, lv_theme_default_init(disp, lv_color_hex(UI_COLOR_ACCENT),
	                                                 lv_palette_main(LV_PALETTE_TEAL), true,
	                                                 &lv_font_montserrat_24));

	lv_evdev_discovery_start(input_found_cb, NULL);

	uint64_t start = time_us();
	ui_init(disp);
	home_create();
	printf("phone-ui: ready in %llu ms\n", (unsigned long long)(time_us() - start) / 1000);
	if (bench)
		lv_timer_create(bench_step, ui_anim_ms + 800, NULL);
	/* --open <app>: start in that app (development) */
	if (argc > 2 && strcmp(argv[1], "--open") == 0 && ui_app_find(argv[2]))
		ui_app_open(ui_app_find(argv[2]));

	for (;;) {
		uint32_t idle = lv_timer_handler();

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
