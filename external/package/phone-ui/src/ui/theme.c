/*
 * Themes: the dark and light colors, LVGL's theme, and switching them (or the
 * language) by making every screen again.
 */

#include <string.h>

#include "i18n.h"
#include "ui_internal.h"

static const struct ui_colors dark_colors = {
	.text = 0xffffff,
	.dim = 0xa0a0aa,
	.card = 0x2c2c34,
	.pressed = 0x3c3c46,
	.line = 0x3c3c46,
	.track = 0x48484f,
	.knob_edge = 0xffffff,
	.button = 0x3a3a42,
	.button_pressed = 0x56565e,
	.bar = 0x1c1c22,
	.shade = 0x000000,
};

static const struct ui_colors light_colors = {
	.text = 0x1c1c1e,
	.dim = 0x6c6c74,
	.card = 0xffffff,
	.pressed = 0xd8d8de,
	.line = 0xe2e2e8,
	.track = 0xd4d4da,
	.knob_edge = 0xb4b4bc,
	.button = 0xdcdce2,
	.button_pressed = 0xc0c0c8,
	.bar = 0xf4f4f8,
	.shade = 0xffffff,
};

const struct ui_colors *ui_colors = &dark_colors;
bool ui_light;

void theme_apply(bool light)
{
	ui_light = light;
	ui_colors = light ? &light_colors : &dark_colors;
	lv_display_t *disp = lv_display_get_default();
	lv_display_set_theme(disp, lv_theme_default_init(disp, lv_color_hex(UI_COLOR_ACCENT),
	                                                 lv_palette_main(LV_PALETTE_TEAL), !light,
	                                                 UI_FONT_BODY));
	status_recolor();
	gestures_recolor();
}

static void (*rebuild_reopen)(void);

static void rebuild_cb(void *data)
{
	theme_apply((uintptr_t)data != 0);
	nav_rebuild(rebuild_reopen);
}

void ui_set_light(bool light, void (*reopen)(void))
{
	if (light == ui_light)
		return;
	ui_setting_set_int("light", light);
	rebuild_reopen = reopen;
	lv_async_call(rebuild_cb, (void *)(uintptr_t)light);
}

void ui_language_set(const char *code, void (*reopen)(void))
{
	if (strcmp(code, ui_language()->code) == 0 || !ui_language_use(code))
		return;
	ui_setting_set("language", code);
	rebuild_reopen = reopen;
	lv_async_call(rebuild_cb, (void *)(uintptr_t)ui_light);
}
