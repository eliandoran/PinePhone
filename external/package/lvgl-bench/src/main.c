/*
 * LVGL demos on the DRM display. Runs the benchmark demo once (the summary
 * stays on screen and is printed to stdout), then keeps rendering. Arguments:
 * scene names to run, all scenes without any; --widgets runs the widgets demo
 * instead. Input devices (touchscreen, keys) are picked up from /dev/input.
 *
 * Frames are paced by the display, not by LVGL's refresh timer: the DRM driver
 * waits for the previous page flip before rendering into its buffer, so
 * refreshing as soon as something changed renders once per vblank. A fixed
 * timer drifts against the panel (55.3 Hz) and misses vblanks.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl.h"
#include "demos/lv_demos.h"

static bool rendered;

static void render_start_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	rendered = true;
}

/* Required by the discovery: it calls it for every device it adds */
static void input_found_cb(lv_indev_t *indev, lv_evdev_type_t type, void *user_data)
{
	LV_UNUSED(indev);
	LV_UNUSED(user_data);
	printf("lvgl-bench: input device added (%s)\n",
	       type == LV_EVDEV_TYPE_ABS ? "touchscreen" :
	       type == LV_EVDEV_TYPE_REL ? "mouse" : "keys");
}

int main(int argc, char **argv)
{
	/* stdout goes to a log file: flush every line, the program never exits */
	setvbuf(stdout, NULL, _IOLBF, 0);

	lv_init();

	lv_display_t *disp = lv_linux_drm_create();
	if (lv_linux_drm_set_file(disp, "/dev/dri/card0", -1) != LV_RESULT_OK) {
		fprintf(stderr, "lvgl-bench: cannot use /dev/dri/card0\n");
		return EXIT_FAILURE;
	}

	/* Refresh from the loop below; step animations on every pass (once per frame) */
	lv_display_delete_refr_timer(disp);
	lv_timer_set_period(lv_anim_get_timer(), 0);
	lv_display_add_event_cb(disp, render_start_cb, LV_EVENT_RENDER_START, NULL);

	/* Touchscreen and keys, also when plugged in later */
	lv_evdev_discovery_start(input_found_cb, NULL);

	if (argc > 1 && strcmp(argv[1], "--widgets") == 0) {
		lv_demo_widgets();
	} else {
		/* argv is NULL terminated */
		if (argc > 1)
			lv_demo_benchmark_set_scenes((const char * const *)argv + 1);
		lv_demo_benchmark();
	}

	for (;;) {
		uint32_t idle = lv_timer_handler();

		rendered = false;
		lv_display_refr_timer(NULL);
		if (rendered)
			continue;

		/* Nothing changed: sleep one frame while animations run (e.g. waiting out
		 * a delay; their timer always reports ready), else until the next timer */
		if (idle == LV_NO_TIMER_READY || lv_anim_count_running())
			idle = LV_DEF_REFR_PERIOD;
		usleep((idle ? idle : 1) * 1000);
	}
}
