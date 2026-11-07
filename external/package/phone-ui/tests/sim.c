/*
 * phone-ui in a desktop window (`just sim`): the UI tests' phone-ui (ui_test.c, same
 * display, sandbox and fakes) in real time, shown with SDL. The mouse is the finger,
 * the keyboard types into the focused field. libcurl and ALSA are the host's (real
 * network and sound); the modem (a SIM, registered) and Wi-Fi are the tests' fakes.
 * /var/lib is kept between runs (tests/build/sim/var-lib).
 *
 * Keys: End Power, Page Up / Page Down the volume keys, Escape back, F5 an SMS arrives,
 * F6 the charger plugged in or out, F12 a screenshot (in tests/build/sim/, where it runs),
 * Ctrl+V the host's clipboard into the focused field, Ctrl+= / Ctrl+- zoom, Ctrl+0 the
 * phone's real size again.
 *
 * The window is the phone's display at its real size (68 x 136 mm) on the monitor, measured by
 * its EDID (displays/, copied from the host's /sys/class/drm by the Makefile); 80% of the
 * screen's height when unknown. Smaller than the display's pixels, each window pixel is the
 * average of those it covers, as the eye would see them.
 *
 * sim [--scale f] [--open <app>|Menu]
 */

#include <EGL/egl.h>
#include <SDL3/SDL.h>
#include <dirent.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ui_test.h"
#include "modem.h"

#define WIDTH 720
#define HEIGHT 1440
#define FRAME_MS 16
#define SCROLL_PX 120 /* a wheel notch */
#define PANEL_H_MM 136.0f /* the panel's height, as its driver gives it (panel-sitronix-st7703.c) */

static bool rendered;

static void render_start_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	rendered = true;
}

static uint64_t real_ms(void)
{
	return SDL_GetTicks();
}

/* The clickable object a label is in (a header's back button) */
static lv_obj_t *clickable(lv_obj_t *obj)
{
	while (obj && !lv_obj_is_clickable(obj))
		obj = lv_obj_get_parent(obj);
	return obj;
}

static void back(void)
{
	lv_obj_t *obj = clickable(ui_test_find(LV_SYMBOL_LEFT));
	if (obj)
		lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
}

/* The wheel scrolls what is under the pointer */
static void scroll(int32_t x, int32_t y, int notches)
{
	lv_point_t p = { x, y };
	lv_obj_t *obj = lv_indev_search_obj(lv_screen_active(), &p);
	if (!obj)
		obj = lv_indev_search_obj(lv_layer_top(), &p);
	for (; obj; obj = lv_obj_get_parent(obj))
		if (lv_obj_is_scrollable(obj) &&
		    (lv_obj_get_scroll_top(obj) > 0 || lv_obj_get_scroll_bottom(obj) > 0))
			break;
	if (obj)
		lv_obj_scroll_by_bounded(obj, 0, notches * SCROLL_PX, LV_ANIM_ON);
}

static void type(const char *text)
{
	lv_obj_t *ta = ui_test_textarea();
	if (ta)
		lv_textarea_add_text(ta, text);
}

static void text_key(SDL_Keycode key)
{
	lv_obj_t *ta = ui_test_textarea();
	if (!ta)
		return;
	switch (key) {
	case SDLK_BACKSPACE: lv_textarea_delete_char(ta); break;
	case SDLK_DELETE: lv_textarea_delete_char_forward(ta); break;
	case SDLK_LEFT: lv_textarea_cursor_left(ta); break;
	case SDLK_RIGHT: lv_textarea_cursor_right(ta); break;
	case SDLK_RETURN:
	case SDLK_KP_ENTER: lv_textarea_add_char(ta, '\n'); break; /* one line: READY */
	}
}

static bool charging;

static void charger(void)
{
	charging = !charging;
	ui_test_write("/sys/class/power_supply/axp20x-usb/online", charging ? "1\n" : "0\n");
	ui_test_write(UI_BATTERY "/status", charging ? "Charging\n" : "Discharging\n");
	ui_test_write(UI_BATTERY "/current_now", charging ? "850000\n" : "-312000\n");
	printf("sim: charger %s\n", charging ? "plugged in" : "unplugged");
}

static void screenshot(void)
{
	char path[64];
	time_t t = time(NULL);
	strftime(path, sizeof(path), "sim-%Y%m%d-%H%M%S.png", localtime(&t));
	if (ui_test_screenshot(path))
		printf("sim: %s\n", path);
}

/* A monitor's size in mm from its EDID: the first detailed timing's, else the cm of the
 * basic parameters; false if it has none (projectors) */
static bool edid_size(const char *path, int *w_mm, int *h_mm)
{
	static const uint8_t header[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
	uint8_t e[128];
	FILE *f = fopen(path, "rb");
	size_t n = f ? fread(e, 1, sizeof(e), f) : 0;
	if (f)
		fclose(f);
	if (n < sizeof(e) || memcmp(e, header, sizeof(header)) != 0)
		return false;
	if (e[54] || e[55]) {
		*w_mm = e[66] | (e[68] >> 4) << 8;
		*h_mm = e[67] | (e[68] & 0xf) << 8;
	} else {
		*w_mm = e[21] * 10;
		*h_mm = e[22] * 10;
	}
	return *w_mm >= 100 && *h_mm >= 100;
}

/* Whether a display's name has the connector's ("HDMI-A-1 27\"", not eDP-1 for DP-1) */
static bool names_connector(const char *name, const char *connector)
{
	size_t len = strlen(connector);
	for (const char *s = strstr(name, connector); s; s = strstr(s + 1, connector))
		if ((s == name || !SDL_isalnum(s[-1])) && !SDL_isalnum(s[len]))
			return true;
	return false;
}

/* The window's scale at which the phone's display has its real size on this one: its
 * connector's EDID (by name), else the monitors' if they all agree; 0 when unknown */
static float real_scale(SDL_DisplayID display)
{
	const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display);
	const char *name = SDL_GetDisplayName(display);
	DIR *dir = opendir("displays");
	if (!dir)
		return 0;
	float named = 0, any = 0;
	bool agree = true;
	struct dirent *ent;
	while (mode && (ent = readdir(dir))) {
		char path[300];
		int w_mm, h_mm;
		snprintf(path, sizeof(path), "displays/%s", ent->d_name);
		if (ent->d_name[0] == '.' || !edid_size(path, &w_mm, &h_mm))
			continue;
		/* by the long sides, the same on a monitor turned upright */
		float px_per_mm = (float)SDL_max(mode->w, mode->h) * mode->pixel_density / SDL_max(w_mm, h_mm);
		if (name && names_connector(name, ent->d_name))
			named = px_per_mm;
		if (any && (px_per_mm > any * 1.05f || px_per_mm < any / 1.05f))
			agree = false;
		if (!any)
			any = px_per_mm;
	}
	closedir(dir);
	float px_per_mm = named ? named : agree ? any : 0;
	return px_per_mm * PANEL_H_MM / HEIGHT / (mode ? mode->pixel_density : 1);
}

/* The display's pixels (bottom-up) shrunk to w x h, each the average of those it covers */
static void shrink(const uint8_t *src, uint8_t *dst, int w, int h)
{
	for (int oy = 0; oy < h; oy++) {
		int y0 = oy * HEIGHT / h, y1 = (oy + 1) * HEIGHT / h;
		for (int ox = 0; ox < w; ox++) {
			int x0 = ox * WIDTH / w, x1 = (ox + 1) * WIDTH / w;
			unsigned sum[4] = { 0 }, n = (unsigned)((y1 - y0) * (x1 - x0));
			for (int y = y0; y < y1; y++) {
				const uint8_t *p = src + ((size_t)(HEIGHT - 1 - y) * WIDTH + x0) * 4;
				for (int x = x0; x < x1; x++, p += 4)
					for (int c = 0; c < 4; c++)
						sum[c] += p[c];
			}
			for (int c = 0; c < 4; c++)
				*dst++ = (uint8_t)((sum[c] + n / 2) / n);
		}
	}
}

int main(int argc, char **argv)
{
	float scale = 0;
	const char *open = NULL;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (strcmp(argv[i], "--scale") == 0)
			scale = strtof(argv[i + 1], NULL);
		else if (strcmp(argv[i], "--open") == 0)
			open = argv[i + 1];
	}

	/* The window's class (X11) and app ID (Wayland), for window manager rules */
	SDL_SetAppMetadata("PinePhone simulator", NULL, "pinephone-sim");
	SDL_SetHint(SDL_HINT_APP_ID, "pinephone-sim");
	/* Wayland first (SDL prefers it only with some protocols): through XWayland, a scaled
	 * screen's compositor stretches the window, blurred */
	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "sim: %s\n", SDL_GetError());
		return EXIT_FAILURE;
	}
	/* Unless set: the real size, else 80% of the screen's height (larger, a window manager
	 * may maximize it) */
	SDL_Rect usable;
	float real = real_scale(SDL_GetPrimaryDisplay());
	if (!real)
		real = SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) ?
		       0.8f * usable.h / HEIGHT : 0.5f;
	if (scale <= 0)
		scale = real;
	/* Not resizable: a window of the display's shape, which tiling window managers leave
	 * floating (they size resizable ones); Ctrl+= and Ctrl+- zoom. High density: in the
	 * screen's pixels, not its scaled ones */
	SDL_Window *win = SDL_CreateWindow("PinePhone", (int)(WIDTH * scale + 0.5f), (int)(HEIGHT * scale + 0.5f),
	                                   SDL_WINDOW_HIGH_PIXEL_DENSITY);
	/* Software: the sandbox has no GPU, LVGL draws in llvmpipe anyway */
	SDL_Renderer *ren = win ? SDL_CreateRenderer(win, SDL_SOFTWARE_RENDERER) : NULL;
	SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
	                                           WIDTH, HEIGHT) : NULL;
	if (!tex) {
		fprintf(stderr, "sim: %s\n", SDL_GetError());
		return EXIT_FAILURE;
	}
	int px_w, px_h;
	SDL_GetWindowSizeInPixels(win, &px_w, &px_h);
	printf("sim: %dx%d pixels on %s (%s), %s\n", px_w, px_h, SDL_GetDisplayName(SDL_GetDisplayForWindow(win)),
	       SDL_GetCurrentVideoDriver(), scale == real && real_scale(SDL_GetPrimaryDisplay()) ? "the real size" : "scaled");
	SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
	/* Smaller: the display shrunk to the window's pixels, drawn as they are */
	SDL_Texture *small = NULL;
	uint8_t *small_pixels = NULL;
	SDL_SetRenderLogicalPresentation(ren, WIDTH, HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX);
	SDL_StartTextInput(win);

	/* The board's sysfs, the fakes; /var/lib as the last run left it */
	ui_test_board();
	ui_test_start();
	lv_display_add_event_cb(lv_display_get_default(), render_start_cb, LV_EVENT_RENDER_START, NULL);
	ui_test_wifi_running();
	ui_test_wifi_ap("Home", -48, "[WPA2-PSK-CCMP][ESS]", "password");
	ui_test_wifi_ap("Café", -71, "[ESS]", NULL);
	ui_test_wifi_ap("Neighbours", -84, "[WPA2-PSK-CCMP][ESS]", "secret");
	ui_test_modem(UI_TEST_SIM_READY);
	if (open && strcmp(open, "Menu") == 0)
		ui_open_menu();
	else if (open && ui_app_find(open))
		ui_app_open(ui_app_find(open));
	rendered = true;
	printf("sim: End Power, Page Up/Down volume, Escape back, F5 an SMS, F6 the charger, F12 a screenshot, "
	       "Ctrl+V paste, Ctrl+=/- zoom, Ctrl+0 real size\n");

	uint64_t last = real_ms();
	for (;;) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			SDL_ConvertEventToRenderCoordinates(ren, &ev); /* the display's pixels */
			switch (ev.type) {
			case SDL_EVENT_QUIT:
				return EXIT_SUCCESS;
			case SDL_EVENT_MOUSE_MOTION:
				lv_test_mouse_move_to((int32_t)ev.motion.x, (int32_t)ev.motion.y);
				break;
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:
				if (ev.button.button != SDL_BUTTON_LEFT)
					break;
				lv_test_mouse_move_to((int32_t)ev.button.x, (int32_t)ev.button.y);
				if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
					lv_test_mouse_press();
				else
					lv_test_mouse_release();
				break;
			case SDL_EVENT_MOUSE_WHEEL:
				scroll((int32_t)ev.wheel.mouse_x, (int32_t)ev.wheel.mouse_y, ev.wheel.integer_y);
				break;
			case SDL_EVENT_TEXT_INPUT:
				type(ev.text.text);
				break;
			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_KEY_UP: {
				bool down = ev.type == SDL_EVENT_KEY_DOWN;
				SDL_Keycode key = ev.key.key;
				unsigned button = key == SDLK_END ? KEY_POWER : key == SDLK_PAGEUP ? KEY_VOLUMEUP :
				                  key == SDLK_PAGEDOWN ? KEY_VOLUMEDOWN : 0;
				if (button) {
					if (!ev.key.repeat)
						ui_test_button(button, down);
				} else if (down && key == SDLK_ESCAPE) {
					back();
				} else if (down && key == SDLK_F5 && !ev.key.repeat) {
					ui_test_modem_sms("+15550100", "Hello from the simulator");
				} else if (down && key == SDLK_F6 && !ev.key.repeat) {
					charger();
				} else if (down && key == SDLK_F12 && !ev.key.repeat) {
					screenshot();
				} else if (down && (ev.key.mod & SDL_KMOD_CTRL) &&
				           (key == SDLK_EQUALS || key == SDLK_PLUS || key == SDLK_MINUS)) {
					scale *= key == SDLK_MINUS ? 1 / 1.1f : 1.1f;
					SDL_SetWindowSize(win, (int)(WIDTH * scale + 0.5f), (int)(HEIGHT * scale + 0.5f));
				} else if (down && (ev.key.mod & SDL_KMOD_CTRL) && key == SDLK_0) {
					scale = real;
					SDL_SetWindowSize(win, (int)(WIDTH * scale + 0.5f), (int)(HEIGHT * scale + 0.5f));
				} else if (down && (ev.key.mod & SDL_KMOD_CTRL) && key == SDLK_V) {
					/* The host's clipboard into the focused field (no text input event for it) */
					char *text = SDL_GetClipboardText();
					if (text && text[0])
						type(text);
					SDL_free(text);
				} else if (down) {
					text_key(key);
				}
				break;
			}
			default:
				if (ev.type >= SDL_EVENT_WINDOW_FIRST && ev.type <= SDL_EVENT_WINDOW_LAST)
					rendered = true; /* shown again, resized */
				break;
			}
		}

		/* phone-ui's clock follows the real one, a frame at a time */
		uint64_t now = real_ms();
		uint32_t ms = (uint32_t)(now - last);
		last = now;
		ui_test_frame(ms > 250 ? 250 : ms ? ms : 1);

		if (rendered) {
			rendered = false;
			/* On Wayland SDL shows the window through a GL context of its own, which it believes
			 * still current after LVGL made its own current: forgotten, SDL makes it current
			 * again, and LVGL gets its own back after (its pixels read in its own first) */
			const uint8_t *pixels = ui_test_pixels();
			EGLDisplay lv_dpy = eglGetCurrentDisplay();
			EGLContext lv_ctx = eglGetCurrentContext();
			SDL_GL_MakeCurrent(win, NULL);
			/* the window's pixels the display covers (letterboxed) */
			int w, h;
			SDL_GetCurrentRenderOutputSize(ren, &w, &h);
			if ((int64_t)w * HEIGHT > (int64_t)h * WIDTH)
				w = h * WIDTH / HEIGHT;
			else
				h = w * HEIGHT / WIDTH;
			SDL_RenderClear(ren);
			if (w > 0 && h > 0 && w < WIDTH) {
				if (!small || small->w != w || small->h != h) {
					SDL_DestroyTexture(small);
					small = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
					                          SDL_TEXTUREACCESS_STREAMING, w, h);
					SDL_SetTextureScaleMode(small, SDL_SCALEMODE_NEAREST);
					small_pixels = realloc(small_pixels, (size_t)w * h * 4);
				}
				shrink(pixels, small_pixels, w, h);
				SDL_UpdateTexture(small, NULL, small_pixels, w * 4);
				SDL_RenderTexture(ren, small, NULL, NULL);
			} else {
				SDL_UpdateTexture(tex, NULL, pixels, WIDTH * 4);
				SDL_RenderTextureRotated(ren, tex, NULL, NULL, 0, NULL, SDL_FLIP_VERTICAL); /* bottom-up */
			}
			SDL_RenderPresent(ren);
			if (lv_ctx != EGL_NO_CONTEXT)
				eglMakeCurrent(lv_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, lv_ctx);
		}
		uint64_t spent = real_ms() - now;
		if (spent < FRAME_MS)
			SDL_Delay((uint32_t)(FRAME_MS - spent));
	}
}
