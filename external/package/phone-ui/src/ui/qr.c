/*
 * A QR code: the modules encoded once, kept in the object, and drawn each frame as
 * rectangles, a run of dark modules in a row as one (filled rectangles are what the
 * GLES draw unit draws best; an image of them would be a texture to upload).
 */

#include <stdlib.h>
#include <string.h>

#include "qr.h"
#include "qrcodegen.h"

static void draw_cb(lv_event_t *e)
{
	lv_obj_t *obj = lv_event_get_current_target_obj(e);
	const uint8_t *code = lv_obj_get_user_data(obj);
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(obj, &a);

	int n = qrcodegen_getSize(code);
	int32_t px = lv_area_get_width(&a) / (n + 2 * UI_QR_QUIET);
	int32_t x0 = a.x1 + (lv_area_get_width(&a) - px * n) / 2;
	int32_t y0 = a.y1 + (lv_area_get_height(&a) - px * n) / 2;

	lv_draw_rect_dsc_t rect;
	lv_draw_rect_dsc_init(&rect);
	rect.bg_color = lv_color_black();
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++) {
			if (!qrcodegen_getModule(code, x, y))
				continue;
			int end = x;
			while (end + 1 < n && qrcodegen_getModule(code, end + 1, y))
				end++;
			lv_area_t r = { x0 + x * px, y0 + y * px, x0 + (end + 1) * px - 1, y0 + (y + 1) * px - 1 };
			lv_draw_rect(layer, &rect, &r);
			x = end;
		}
}

static void delete_cb(lv_event_t *e)
{
	free(lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
}

lv_obj_t *ui_qr_create(lv_obj_t *parent, const char *text, int32_t size)
{
	uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
	uint8_t *code = malloc(qrcodegen_BUFFER_LEN_MAX);
	if (!code)
		return NULL;
	if (!qrcodegen_encodeText(text, temp, code, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
	                          qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
		free(code);
		return NULL;
	}
	lv_obj_t *obj = ui_box(parent);
	lv_obj_set_size(obj, size, size);
	lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_user_data(obj, code);
	lv_obj_add_event_cb(obj, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_event_cb(obj, delete_cb, LV_EVENT_DELETE, NULL);
	return obj;
}
