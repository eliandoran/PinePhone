/*
 * phone-ui in a desktop window (`just sim`): the UI tests' phone-ui (ui_test.c, same
 * display, sandbox and fakes) in real time, shown with SDL. The mouse is the finger,
 * the keyboard types into the focused field. libcurl and ALSA are the host's (real
 * network and sound); the modem (a SIM, registered) and Wi-Fi are the tests' fakes.
 * /var/lib is kept between runs (tests/build/sim/var-lib).
 *
 * Keys: End Power, Page Up / Page Down the volume keys, Escape back, F5 an SMS arrives,
 * F6 the charger plugged in or out, F12 a screenshot (in tests/build/sim/, where it runs).
 *
 * sim [--scale f] [--open <app>|Menu]
 */

#include <SDL3/SDL.h>
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

int main(int argc, char **argv)
{
	float scale = 0.5f;
	const char *open = NULL;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (strcmp(argv[i], "--scale") == 0)
			scale = strtof(argv[i + 1], NULL);
		else if (strcmp(argv[i], "--open") == 0)
			open = argv[i + 1];
	}
	if (scale <= 0)
		scale = 0.5f;

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "sim: %s\n", SDL_GetError());
		return EXIT_FAILURE;
	}
	SDL_Window *win = SDL_CreateWindow("PinePhone", (int)(WIDTH * scale), (int)(HEIGHT * scale),
	                                   SDL_WINDOW_RESIZABLE);
	/* Software: the sandbox has no GPU, LVGL draws in llvmpipe anyway */
	SDL_Renderer *ren = win ? SDL_CreateRenderer(win, SDL_SOFTWARE_RENDERER) : NULL;
	SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
	                                           WIDTH, HEIGHT) : NULL;
	if (!tex) {
		fprintf(stderr, "sim: %s\n", SDL_GetError());
		return EXIT_FAILURE;
	}
	SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
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
	printf("sim: End Power, Page Up/Down volume, Escape back, F5 an SMS, F6 the charger, F12 a screenshot\n");

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
			SDL_UpdateTexture(tex, NULL, ui_test_pixels(), WIDTH * 4);
			SDL_RenderClear(ren);
			SDL_RenderTextureRotated(ren, tex, NULL, NULL, 0, NULL, SDL_FLIP_VERTICAL); /* bottom-up */
			SDL_RenderPresent(ren);
		}
		uint64_t spent = real_ms() - now;
		if (spent < FRAME_MS)
			SDL_Delay((uint32_t)(FRAME_MS - spent));
	}
}
