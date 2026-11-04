/*
 * phone-ui on the host, see ui_test.h. Linked with all of phone-ui but main.c, whose
 * startup it does here on its own display.
 */

#define _XOPEN_SOURCE 700 /* nftw */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <ftw.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ui_test.h"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h" /* the keys */

#define WIDTH 720
#define HEIGHT 1440
#define FRAME_MS 16
/* Made by the Makefile in the sandbox's /var/lib and /sys: never empty other ones */
#define MARK ".phone-ui-tests"

#define BATTERY UI_BATTERY "/"
#define PMIC_ADC "/sys/devices/platform/axp813-adc/iio:device0"

/* The keyboard's mode keys (ui.c) */
#define KB_SHIFT LV_SYMBOL_UP
#define KB_TO_SYMBOLS "?123"
#define KB_TO_MORE "=\\<"
#define KB_DOUBLE_MS 400 /* shift twice within this: caps lock */

static lv_display_t *disp;

/* main.c's, without the hardware: the emulated input devices are made at startup */
void ui_input_start(void)
{
}

void ui_cpu_boost(bool on)
{
	LV_UNUSED(on);
}

/* OpenGL ES 2 (the Mali-400's) without a window or GPU: Mesa's surfaceless platform */
static void gl_init(void)
{
	EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	EGLint n = 0;
	EGLConfig config;
	static const EGLint config_attrs[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE,
	};
	static const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = EGL_NO_CONTEXT;
	if (eglInitialize(dpy, NULL, NULL) && eglBindAPI(EGL_OPENGL_ES_API) &&
	    eglChooseConfig(dpy, config_attrs, &config, 1, &n) && n)
		ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, context_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "ui_test: no EGL context (EGL error %#x)\n", eglGetError());
		exit(EXIT_FAILURE);
	}
	/* LVGL calls GL through glad, loaded by its own EGL contexts otherwise */
	extern int gladLoadGLES2(void *load);
	if (!gladLoadGLES2((void *)eglGetProcAddress)) {
		fprintf(stderr, "ui_test: glad cannot load GLES2\n");
		exit(EXIT_FAILURE);
	}
}

static int remove_cb(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
	LV_UNUSED(st);
	LV_UNUSED(type);
	if (ftw->level > 0 && strcmp(path + ftw->base, MARK) != 0 && remove(path) != 0)
		perror(path);
	return 0;
}

/* The sandbox's directory, emptied */
static void empty(const char *dir)
{
	char mark[64];
	snprintf(mark, sizeof(mark), "%s/" MARK, dir);
	if (access(mark, F_OK) != 0) {
		fprintf(stderr, "ui_test: not in the tests' sandbox (no %s): run them with `just test`\n", mark);
		exit(EXIT_FAILURE);
	}
	nftw(dir, remove_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static bool reset;

/* A PinePhone's sysfs, as much as phone-ui reads of it: the status bar shows 76%,
 * discharging; Settings' pages find a backlight at half, a battery, temperatures */
void ui_test_reset(void)
{
	empty("/var/lib");
	empty("/sys");
	reset = true;

	ui_test_write("/sys/class/backlight/backlight/max_brightness", "1000\n");
	ui_test_write("/sys/class/backlight/backlight/brightness", "500\n");
	ui_test_write("/sys/class/backlight/backlight/bl_power", "0\n");

	ui_test_write(BATTERY "present", "1\n");
	ui_test_write(BATTERY "capacity", "76\n");
	ui_test_write(BATTERY "status", "Discharging\n");
	ui_test_write(BATTERY "health", "Good\n");
	ui_test_write(BATTERY "technology", "Li-ion\n");
	ui_test_write(BATTERY "voltage_now", "3850000\n");
	ui_test_write(BATTERY "current_now", "-312000\n");

	ui_test_write("/sys/class/thermal/thermal_zone0/temp", "41234\n");
	ui_test_write("/sys/class/thermal/thermal_zone0/trip_point_0_temp", "75000\n");
	ui_test_write("/sys/class/thermal/thermal_zone1/temp", "38000\n");
	ui_test_write("/sys/class/thermal/thermal_zone2/temp", "39600\n");

	/* Found by its real path */
	ui_test_write(PMIC_ADC "/in_temp_raw", "1907\n");
	ui_test_write(PMIC_ADC "/in_temp_offset", "-1447\n");
	ui_test_write(PMIC_ADC "/in_temp_scale", "100\n");
	ui_test_write("/sys/bus/iio/devices/.keep", "");
	if (symlink(PMIC_ADC, "/sys/bus/iio/devices/iio:device0") != 0)
		perror("ui_test: iio:device0");
	remove("/sys/bus/iio/devices/.keep");

	ui_test_write("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "1152000\n");
	ui_test_write("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "schedutil\n");
}

static bool started;

void ui_test_start(void)
{
	if (started)
		return;
	started = true;
	if (!reset)
		ui_test_reset();

	/* As main.c */
	setvbuf(stdout, NULL, _IOLBF, 0);
	settings_apply();
	gl_init();
	lv_init();
	disp = lv_opengles_texture_create(WIDTH, HEIGHT);
	if (!disp) {
		fprintf(stderr, "ui_test: no display\n");
		exit(EXIT_FAILURE);
	}
	lv_display_set_dpi(disp, 269);
	lv_display_delete_refr_timer(disp);
	lv_timer_set_period(lv_anim_get_timer(), 0);
	lv_test_indev_create_all();

	ui_init(disp);
	ui_volume_init();
	external_scan();
	home_create();
	ui_test_settle();
}

/* A pass of main.c's loop, FRAME_MS later (lv_test_fast_forward() does not refresh a
 * display without its refresh timer, which main.c deletes) */
static void frame(void)
{
	lv_tick_inc(FRAME_MS);
	lv_timer_handler();
	lv_indev_read(lv_test_indev_get_indev(LV_INDEV_TYPE_POINTER));
	lv_display_refr_timer(NULL);
}

void ui_test_wait(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += FRAME_MS)
		frame();
}

void ui_test_settle(void)
{
	ui_test_wait(2 * FRAME_MS);
	for (uint32_t t = 0; lv_anim_count_running() && t < 5000; t += FRAME_MS)
		ui_test_wait(FRAME_MS);
	ui_test_wait(FRAME_MS);
}

/* Objects shown, depth first: the screen on top, then the top layer */
typedef bool (*visit_cb)(lv_obj_t *obj, void *data); /* true: stop there */

static lv_obj_t *visit_tree(lv_obj_t *obj, visit_cb cb, void *data)
{
	/* Scrolled out of view counts: scrolling shows it, a tap scrolls to it */
	if (lv_obj_is_hidden(obj))
		return NULL;
	if (cb(obj, data))
		return obj;
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = visit_tree(lv_obj_get_child(obj, (int32_t)i), cb, data);
		if (found)
			return found;
	}
	return NULL;
}

static lv_obj_t *visit(visit_cb cb, void *data)
{
	lv_obj_t *found = visit_tree(lv_screen_active(), cb, data);
	return found ? found : visit_tree(lv_layer_top(), cb, data);
}

static bool is_text_cb(lv_obj_t *obj, void *data)
{
	return lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), data) == 0;
}

static bool dump_cb(lv_obj_t *obj, void *data)
{
	LV_UNUSED(data);
	if (lv_obj_check_type(obj, &lv_label_class)) {
		lv_area_t a;
		lv_obj_get_coords(obj, &a);
		fprintf(stderr, "    %4d,%4d  \"%s\"\n", (int)a.x1, (int)a.y1, lv_label_get_text(obj));
	}
	return false;
}

void ui_test_dump(void)
{
	fprintf(stderr, "  shown:\n");
	visit(dump_cb, NULL);
}

lv_obj_t *ui_test_find(const char *text)
{
	return visit(is_text_cb, (void *)text);
}

bool ui_test_shows(const char *text)
{
	return ui_test_find(text) != NULL;
}

struct find_all {
	const char *text;
	lv_obj_t **found;
	unsigned count, max;
};

static bool find_all_cb(lv_obj_t *obj, void *data)
{
	struct find_all *s = data;
	if (is_text_cb(obj, (void *)s->text) && s->count++ < s->max)
		s->found[s->count - 1] = obj;
	return false;
}

unsigned ui_test_find_all(const char *text, lv_obj_t **found, unsigned max)
{
	struct find_all s = { text, found, 0, max };
	visit(find_all_cb, &s);
	return s.count;
}

static bool has_part_cb(lv_obj_t *obj, void *data)
{
	return lv_obj_check_type(obj, &lv_label_class) && strstr(lv_label_get_text(obj), data);
}

bool ui_test_shows_part(const char *part)
{
	return visit(has_part_cb, (void *)part) != NULL;
}

/* How far up from obj is what a finger on its middle presses (0: obj itself), -1 if
 * that is not obj or what it is in, or only its screen */
static int tap_distance(lv_obj_t *obj)
{
	lv_obj_t *root = lv_obj_get_screen(obj);
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	lv_point_t p = { (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2 };
	lv_obj_t *hit = lv_indev_search_obj(root, &p);
	if (!hit || hit == root)
		return -1;
	int d = 0;
	for (lv_obj_t *o = obj; o; o = lv_obj_get_parent(o), d++)
		if (o == hit)
			return d;
	return -1;
}

/* Of the labels showing a text, the one pressed nearest to it: a key's button, not a
 * clickable page around the number it typed */
struct tap_search {
	const char *text;
	lv_obj_t *best;
	int distance;
};

static bool tap_search_cb(lv_obj_t *obj, void *data)
{
	struct tap_search *s = data;
	if (is_text_cb(obj, (void *)s->text)) {
		int d = tap_distance(obj);
		if (d >= 0 && (!s->best || d < s->distance)) {
			s->best = obj;
			s->distance = d;
		}
	}
	return false; /* all of them */
}

static bool is_class_cb(lv_obj_t *obj, void *data)
{
	return lv_obj_check_type(obj, data);
}

lv_obj_t *ui_test_find_widget(const lv_obj_class_t *cls)
{
	return visit(is_class_cb, (void *)cls);
}

static void press_at(int32_t x, int32_t y, uint32_t ms)
{
	lv_test_mouse_move_to(x, y);
	lv_test_mouse_press();
	ui_test_wait(ms);
	lv_test_mouse_release();
	ui_test_settle();
}

void ui_test_tap_at(int32_t x, int32_t y)
{
	press_at(x, y, 3 * FRAME_MS);
}

/* The finger down on obj's middle for `ms`, then what it started */
static void press(lv_obj_t *obj, uint32_t ms)
{
	lv_obj_scroll_to_view_recursive(obj, LV_ANIM_OFF);
	ui_test_wait(FRAME_MS);
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	press_at((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, ms);
}

void ui_test_tap(lv_obj_t *obj)
{
	press(obj, 3 * FRAME_MS);
}

void ui_test_long_press(lv_obj_t *obj)
{
	press(obj, LV_INDEV_DEF_LONG_PRESS_TIME + 200);
}

static lv_obj_t *must_find(const char *text)
{
	struct tap_search search = { text, NULL, 0 };
	visit(tap_search_cb, &search);
	lv_obj_t *obj = search.best ? search.best : ui_test_find(text);
	if (!obj) {
		fprintf(stderr, "  ui_test: \"%s\" not shown\n", text);
		ui_test_dump();
	}
	return obj;
}

bool ui_test_tap_text(const char *text)
{
	lv_obj_t *obj = must_find(text);
	if (obj)
		ui_test_tap(obj);
	return obj != NULL;
}

bool ui_test_long_press_text(const char *text)
{
	lv_obj_t *obj = must_find(text);
	if (obj)
		ui_test_long_press(obj);
	return obj != NULL;
}

static bool is_field_cb(lv_obj_t *obj, void *data)
{
	if (!lv_obj_check_type(obj, &lv_textarea_class))
		return false;
	const char *placeholder = lv_textarea_get_placeholder_text(obj);
	return strcmp(lv_textarea_get_text(obj), data) == 0 || (placeholder && strcmp(placeholder, data) == 0);
}

bool ui_test_tap_field(const char *text)
{
	lv_obj_t *ta = visit(is_field_cb, (void *)text);
	if (!ta) {
		fprintf(stderr, "  ui_test: no field \"%s\"\n", text);
		ui_test_dump();
		return false;
	}
	ui_test_tap(ta);
	return true;
}

bool ui_test_write(const char *path, const char *text)
{
	char dir[256];
	snprintf(dir, sizeof(dir), "%s", path);
	for (char *s = strchr(dir + 1, '/'); s; s = strchr(s + 1, '/')) {
		*s = '\0';
		mkdir(dir, 0755);
		*s = '/';
	}
	FILE *f = fopen(path, "w");
	if (!f) {
		perror(path);
		return false;
	}
	fputs(text, f);
	return fclose(f) == 0;
}

bool ui_test_back(void)
{
	return ui_test_tap_text(LV_SYMBOL_LEFT);
}

bool ui_test_open(const char *app)
{
	ui_test_start();
	ui_home();
	ui_test_settle();
	ui_open_menu();
	ui_test_settle();
	return ui_test_tap_text(app);
}

/* Keyboard */

static bool is_keyboard_cb(lv_obj_t *obj, void *data)
{
	LV_UNUSED(data);
	return lv_obj_check_type(obj, &lv_keyboard_class);
}

/* Presses the shown key `key` as a tap does (its selection, then VALUE_CHANGED) */
static bool key_press(lv_obj_t *kb, const char *key)
{
	lv_buttonmatrix_t *bm = (lv_buttonmatrix_t *)kb;
	for (uint32_t id = 0; id < bm->btn_cnt; id++) {
		if (lv_buttonmatrix_has_button_ctrl(kb, id, LV_BUTTONMATRIX_CTRL_HIDDEN) ||
		    strcmp(lv_buttonmatrix_get_button_text(kb, id), key) != 0)
			continue;
		lv_buttonmatrix_set_selected_button(kb, id);
		lv_obj_send_event(kb, LV_EVENT_VALUE_CHANGED, &id);
		ui_test_wait(FRAME_MS);
		return true;
	}
	return false;
}

static bool type_char(lv_obj_t *kb, char c)
{
	char key[2] = { c, '\0' };
	const char *k = c == '\n' ? LV_SYMBOL_NEW_LINE : c == '\b' ? LV_SYMBOL_BACKSPACE : key;
	bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');

	/* Letters: shift (not twice in a row: caps lock), back to letters from symbols;
	 * the rest: the symbols, then more of them */
	for (int tries = 0; tries < 4; tries++) {
		if (key_press(kb, k))
			return true;
		if (letter) {
			ui_test_wait(KB_DOUBLE_MS + FRAME_MS);
			if (!key_press(kb, KB_SHIFT) && !key_press(kb, "ABC"))
				break;
		} else if (!key_press(kb, KB_TO_SYMBOLS) && !key_press(kb, KB_TO_MORE)) {
			break;
		}
	}
	fprintf(stderr, "  ui_test: no key for '%c'\n", c);
	return false;
}

static lv_obj_t *keyboard(void)
{
	lv_obj_t *kb = visit(is_keyboard_cb, NULL);
	if (!kb)
		fprintf(stderr, "  ui_test: no keyboard shown\n");
	return kb;
}

bool ui_test_type(const char *text)
{
	lv_obj_t *kb = keyboard();
	if (!kb)
		return false;
	for (; *text; text++)
		if (!type_char(kb, *text))
			return false;
	ui_test_settle();
	return true;
}

bool ui_test_key(const char *key)
{
	lv_obj_t *kb = keyboard();
	if (!kb)
		return false;
	if (!key_press(kb, key)) {
		fprintf(stderr, "  ui_test: no key \"%s\"\n", key);
		return false;
	}
	ui_test_settle();
	return true;
}

static bool is_focused_ta_cb(lv_obj_t *obj, void *data)
{
	LV_UNUSED(data);
	return lv_obj_check_type(obj, &lv_textarea_class) && lv_obj_has_state(obj, LV_STATE_FOCUSED);
}

lv_obj_t *ui_test_textarea(void)
{
	return visit(is_focused_ta_cb, NULL);
}

const char *ui_test_typed(void)
{
	lv_obj_t *ta = ui_test_textarea();
	return ta ? lv_textarea_get_text(ta) : NULL;
}

/* The display's texture, which LVGL keeps bottom-up in BGRA */
bool ui_test_screenshot(const char *path)
{
	static GLuint fbo;
	static uint8_t px[WIDTH * HEIGHT * 4];
	GLint bound;

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
	if (!fbo)
		glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       lv_opengles_texture_get_texture_id(disp), 0);
	glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glBindFramebuffer(GL_FRAMEBUFFER, bound);
	for (size_t i = 3; i < sizeof(px); i += 4)
		px[i] = 0xff; /* opaque, whatever the texture's alpha */

	png_image image = {
		.version = PNG_IMAGE_VERSION, .width = WIDTH, .height = HEIGHT, .format = PNG_FORMAT_BGRA,
	};
	if (!png_image_write_to_file(&image, path, 0, px, -WIDTH * 4, NULL)) {
		fprintf(stderr, "ui_test: %s: %s\n", path, image.message);
		return false;
	}
	return true;
}
