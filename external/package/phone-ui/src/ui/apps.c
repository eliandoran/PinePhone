/*
 * Apps: finding one by name, and its icon (the built-in ones' PNGs from icons.c, an
 * external app's icon.png, or its color and symbol).
 */

#include <png.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ui.h"

const struct ui_app *ui_app_find(const char *name)
{
	for (unsigned i = 0; i < ui_app_count; i++)
		if (strcmp(ui_apps[i].name, name) == 0)
			return &ui_apps[i];
	return NULL;
}

#if LV_USE_3DTEXTURE
/* A built-in app's icon as a texture, decoded the first time and kept; 0: none.
 * The one rendered at `size`, otherwise the largest (scaled). */
static unsigned builtin_icon(const char *name, int32_t size)
{
	static unsigned *textures;
	int best = -1;
	for (unsigned i = 0; i < ui_icon_count; i++)
		if (strcasecmp(ui_icons[i].name, name) == 0 &&
		    (best < 0 || ui_icons[i].size == size ||
		     (ui_icons[best].size != size && ui_icons[i].size > ui_icons[best].size)))
			best = i;
	if (best < 0)
		return 0;
	if (!textures)
		textures = calloc(ui_icon_count, sizeof(*textures));
	if (textures && !textures[best]) {
		const struct ui_icon *ic = &ui_icons[best];
		png_image img = { .version = PNG_IMAGE_VERSION };
		if (!png_image_begin_read_from_memory(&img, ic->png, ic->png_size))
			return 0;
		/* B, G, R, A: the texture shader swaps red and blue back */
		img.format = PNG_FORMAT_BGRA;
		uint8_t *pixels = malloc(PNG_IMAGE_SIZE(img));
		if (pixels && png_image_finish_read(&img, NULL, pixels, 0, NULL))
			textures[best] = ui_texture_create(pixels, img.width, img.height);
		else
			png_image_free(&img);
		free(pixels);
	}
	return textures ? textures[best] : 0;
}
#endif

lv_obj_t *ui_app_icon_create(lv_obj_t *parent, const struct ui_app *app, int32_t size)
{
	lv_obj_t *icon = ui_box(parent);
	lv_obj_set_size(icon, size, size);
#if LV_USE_3DTEXTURE
	/* An external app's icon.png, never a built-in one by the same name */
	unsigned id = app->dir ? app->icon : builtin_icon(app->name, size);
	if (id) {
		/* Its own shape: corners transparent in the PNG */
		lv_obj_t *tex = lv_3dtexture_create(icon);
		lv_obj_set_size(tex, size, size);
		lv_obj_set_clickable(tex, false);
		lv_3dtexture_set_src(tex, id);
		return icon;
	}
#endif
	lv_obj_set_style_radius(icon, size / 4, 0);
	lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(icon, lv_color_hex(app->color), 0);

	lv_obj_t *symbol = lv_label_create(icon);
	lv_label_set_text_static(symbol, app->symbol);
	lv_obj_set_style_text_font(symbol, size >= 96 ? &ui_font_48 : &ui_font_32, 0);
	lv_obj_set_style_text_color(symbol, lv_color_white(), 0);
	lv_obj_center(symbol);
	return icon;
}
