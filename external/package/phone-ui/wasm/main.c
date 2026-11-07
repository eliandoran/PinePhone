/*
 * phone-ui in the browser, instead of ../src/main.c: the display is LVGL's OpenGL ES
 * texture display on the page's canvas (#canvas, 720 x 1440, WebGL 1: GLES 2 as on the
 * phone), drawn on it once a frame when something changed; the pointer is the mouse or a
 * finger on the canvas. The board is a PinePhone's sysfs in Emscripten's memory
 * filesystem; the modem and Wi-Fi are modem.c's and wifi.c's. Emoji images come as they
 * are drawn (emoji.c), the repository's demo files after it started (demo.c).
 *
 * The loop is the browser's (requestAnimationFrame): frames at the screen's refresh rate.
 */

#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ui.h"

#define WIDTH 720
#define HEIGHT 1440
#define CANVAS "#canvas"

static lv_display_t *disp;
static bool rendered;

static struct {
	int32_t x, y;
	bool pressed;
} finger;

/* main.c's: the pointer is made in main(), there are no key devices */
void ui_input_start(void)
{
}

/* cpu.c's: no CPU frequency in a browser */
void ui_cpu_boost(bool on)
{
	LV_UNUSED(on);
}

static void write_file(const char *path, const char *text)
{
	char dir[256];
	snprintf(dir, sizeof(dir), "%s", path);
	for (char *s = strchr(dir + 1, '/'); s; s = strchr(s + 1, '/')) {
		*s = '\0';
		mkdir(dir, 0755);
		*s = '/';
	}
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

/* A PinePhone's sysfs, as the tests' (ui_test.c): backlight, battery, temperatures */
static void board(void)
{
	write_file("/sys/class/backlight/backlight/max_brightness", "1000\n");
	write_file("/sys/class/backlight/backlight/brightness", "500\n");
	write_file("/sys/class/backlight/backlight/bl_power", "0\n");
	write_file("/sys/class/power_supply/axp20x-usb/online", "0\n");
	write_file("/sys/class/power_supply/axp20x-usb/input_current_limit", "500000\n");
	write_file(UI_BATTERY "/present", "1\n");
	write_file(UI_BATTERY "/capacity", "76\n");
	write_file(UI_BATTERY "/status", "Discharging\n");
	write_file(UI_BATTERY "/health", "Good\n");
	write_file(UI_BATTERY "/technology", "Li-ion\n");
	write_file(UI_BATTERY "/charge_full_design", "2750000\n");
	write_file(UI_BATTERY "/voltage_now", "3850000\n");
	write_file(UI_BATTERY "/current_now", "-312000\n");
	write_file("/sys/class/thermal/thermal_zone0/temp", "41234\n");
	write_file("/sys/class/thermal/thermal_zone0/trip_point_0_temp", "75000\n");
	write_file("/sys/class/thermal/thermal_zone1/temp", "38000\n");
	write_file("/sys/class/thermal/thermal_zone2/temp", "39600\n");
	write_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "1152000\n");
	write_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "schedutil\n");
	/* modem.c and wifi.c here play the modem and wpa_supplicant */
	write_file("/etc/phone-ui/modem.conf",
	           "provider=at\n"
	           "data=wwan0\n"
	           "data_script=/etc/phone-ui/mobile-data.sh\n"); /* not there: no fork() anyway */
	/* Wi-Fi's default route, for its router */
	write_file("/proc/net/route", "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n"
	                              "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t0\t00000000\t0\t0\t0\n");
	/* Where the apps keep their data: they make their directories in it */
	mkdir("/var", 0755);
	mkdir("/var/lib", 0755);
}

static uint32_t tick_cb(void)
{
	return (uint32_t)emscripten_get_now();
}

static void render_start_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	rendered = true;
}

static void pointer_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	LV_UNUSED(indev);
	data->point.x = finger.x;
	data->point.y = finger.y;
	data->state = finger.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* A point on the canvas, in the display's pixels whatever its size on the page */
static void finger_at(double x, double y)
{
	double w, h;
	emscripten_get_element_css_size(CANVAS, &w, &h);
	if (w > 0 && h > 0) {
		finger.x = (int32_t)(x * WIDTH / w);
		finger.y = (int32_t)(y * HEIGHT / h);
	}
}

static bool mouse_cb(int type, const EmscriptenMouseEvent *e, void *user_data)
{
	LV_UNUSED(user_data);
	if (e->button != 0)
		return false;
	finger_at(e->targetX, e->targetY);
	if (type == EMSCRIPTEN_EVENT_MOUSEDOWN)
		finger.pressed = true;
	else if (type == EMSCRIPTEN_EVENT_MOUSEUP)
		finger.pressed = false;
	return true;
}

/* The page's mouseup: let go outside the canvas too */
static bool mouseup_cb(int type, const EmscriptenMouseEvent *e, void *user_data)
{
	LV_UNUSED(type);
	LV_UNUSED(e);
	LV_UNUSED(user_data);
	finger.pressed = false;
	return false;
}

/* The first finger on the canvas */
static bool touch_cb(int type, const EmscriptenTouchEvent *e, void *user_data)
{
	LV_UNUSED(user_data);
	if (e->numTouches > 0)
		finger_at(e->touches[0].targetX, e->touches[0].targetY);
	finger.pressed = type == EMSCRIPTEN_EVENT_TOUCHSTART || type == EMSCRIPTEN_EVENT_TOUCHMOVE;
	return true; /* no scrolling the page, no mouse events after */
}

void emoji_start(void);
void emoji_frame(void);
void demo_start(void);

static void frame(void)
{
	lv_timer_handler();
	emoji_frame();
	rendered = false;
	lv_display_refr_timer(NULL);
	if (!rendered)
		return;
	/* The display's texture on the canvas */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIDTH, HEIGHT);
	lv_area_t area = { 0, 0, WIDTH - 1, HEIGHT - 1 };
	lv_opengles_render_texture(lv_opengles_texture_get_texture_id(disp), &area, LV_OPA_COVER, WIDTH, HEIGHT,
	                           &area, false, true); /* the texture is bottom-up */
}

static bool gl_init(void)
{
	EmscriptenWebGLContextAttributes attrs;
	emscripten_webgl_init_context_attributes(&attrs);
	attrs.majorVersion = 1;
	attrs.alpha = false;
	attrs.antialias = false;
	attrs.depth = false;
	attrs.stencil = false;
	EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context(CANVAS, &attrs);
	if (ctx <= 0 || emscripten_webgl_make_context_current(ctx) != EMSCRIPTEN_RESULT_SUCCESS) {
		fprintf(stderr, "phone-ui: no WebGL\n");
		return false;
	}
	/* LVGL calls GL through glad */
	extern int gladLoadGLES2(void *load);
	return gladLoadGLES2((void *)emscripten_webgl_get_proc_address);
}

int main(void)
{
	emscripten_set_canvas_element_size(CANVAS, WIDTH, HEIGHT);
	if (!gl_init())
		return EXIT_FAILURE;
	board();
	emoji_start();
	settings_apply();
	lv_init();
	lv_tick_set_cb(tick_cb);

	disp = lv_opengles_texture_create(WIDTH, HEIGHT);
	lv_display_set_dpi(disp, 269);
	lv_display_delete_refr_timer(disp);
	lv_timer_set_period(lv_anim_get_timer(), 0);
	lv_display_add_event_cb(disp, render_start_cb, LV_EVENT_RENDER_START, NULL);

	lv_indev_t *pointer = lv_indev_create();
	lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(pointer, pointer_read_cb);
	emscripten_set_mousedown_callback(CANVAS, NULL, true, mouse_cb);
	emscripten_set_mousemove_callback(CANVAS, NULL, true, mouse_cb);
	emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, true, mouseup_cb);
	emscripten_set_touchstart_callback(CANVAS, NULL, true, touch_cb);
	emscripten_set_touchmove_callback(CANVAS, NULL, true, touch_cb);
	emscripten_set_touchend_callback(CANVAS, NULL, true, touch_cb);
	emscripten_set_touchcancel_callback(CANVAS, NULL, true, touch_cb);

	ui_init(disp);
	ui_volume_init();
	external_scan();
	home_create();
	ui_splash();
	demo_start();
	emscripten_set_main_loop(frame, 0, false);
	return EXIT_SUCCESS;
}
