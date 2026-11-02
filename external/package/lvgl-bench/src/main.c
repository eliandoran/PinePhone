/*
 * LVGL benchmark on the DRM display. Runs the benchmark demo once (the summary
 * stays on screen and is printed to stdout), then keeps rendering.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "lvgl.h"
#include "demos/lv_demos.h"

int main(void)
{
	/* stdout goes to a log file: flush every line, the program never exits */
	setvbuf(stdout, NULL, _IOLBF, 0);

	lv_init();

	lv_display_t *disp = lv_linux_drm_create();
	if (lv_linux_drm_set_file(disp, "/dev/dri/card0", -1) != LV_RESULT_OK) {
		fprintf(stderr, "lvgl-bench: cannot use /dev/dri/card0\n");
		return EXIT_FAILURE;
	}

	lv_demo_benchmark();

	for (;;) {
		uint32_t idle = lv_timer_handler();
		usleep((idle == LV_NO_TIMER_READY ? LV_DEF_REFR_PERIOD : idle) * 1000);
	}
}
