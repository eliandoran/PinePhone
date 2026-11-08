/*
 * Boot splash: the kernel draws splash.svg as its boot logo (fbcon, in the middle of
 * the screen) from the moment the display is up. phone-ui's first frame shows the
 * same pixels at the same place over the home screen, then fades them out: the
 * display goes from the console to phone-ui without a visible change.
 *
 * Only the first start of a boot (/run is in RAM): a restart (`just ui`) does not.
 */

#include <fcntl.h>
#include <png.h>
#include <stdlib.h>
#include <unistd.h>

#include "ui.h"

#ifndef MARKER
#define MARKER "/run/phone-ui.splash"
#endif
#define HOLD_MS 150 /* on screen as the kernel left it, then */
#define FADE_MS 400

extern const uint8_t ui_splash_png[];
extern const unsigned ui_splash_png_size;

/* The texture does not take its parent's opacity: both fade */
static void opa_cb(void *obj, int32_t v)
{
	lv_obj_set_style_opa(obj, v, 0);
	lv_obj_set_style_opa(lv_obj_get_child(obj, 0), v, 0);
}

static void done_cb(lv_anim_t *a)
{
	lv_obj_delete(a->var);
}

void ui_splash(void)
{
	int fd = open(MARKER, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	close(fd);

	png_image img = { .version = PNG_IMAGE_VERSION };
	if (!png_image_begin_read_from_memory(&img, ui_splash_png, ui_splash_png_size))
		return;
	/* B, G, R, A: the texture shader swaps red and blue back */
	img.format = PNG_FORMAT_BGRA;
	uint8_t *pixels = malloc(PNG_IMAGE_SIZE(img));
	unsigned id = 0;
	if (pixels && png_image_finish_read(&img, NULL, pixels, 0, NULL))
		id = ui_texture_create(pixels, img.width, img.height);
	else
		png_image_free(&img);
	free(pixels);
	if (!id)
		return;

	/* Black, as the console behind the logo; it takes the touches until it is gone */
	lv_obj_t *splash = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(splash);
	lv_obj_set_size(splash, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_color(splash, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
	lv_obj_set_clickable(splash, true);

	/* fbcon puts it at ((width - w) / 2, (height - h) / 2), as lv_obj_center() */
	lv_obj_t *logo = lv_3dtexture_create(splash);
	lv_obj_set_size(logo, img.width, img.height);
	lv_obj_center(logo);
	lv_obj_set_clickable(logo, false);
	lv_3dtexture_set_src(logo, id);

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, splash);
	lv_anim_set_exec_cb(&a, opa_cb);
	lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
	lv_anim_set_delay(&a, HOLD_MS);
	lv_anim_set_duration(&a, FADE_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
	lv_anim_set_completed_cb(&a, done_cb);
	lv_anim_start(&a);
}
