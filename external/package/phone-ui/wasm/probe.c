/*
 * What smoke.mjs (`just wasm-smoke`) asks the page's phone-ui, through Module.ccall:
 * opening an app as `just sim --open` does, where a text is shown (it taps there with
 * the page's mouse, as a user), how many images are drawn, text put in the focused
 * field. Looking only: phone-ui runs as it does for anyone.
 */

#include <emscripten.h>
#include <string.h>

#include "ui.h"

#define WIDTH 720
#define HEIGHT 1440

/* The objects shown, depth first: the screen, then the top layer */
typedef bool (*visit_cb)(lv_obj_t *obj, void *data); /* true: stop there */

static lv_obj_t *visit_tree(lv_obj_t *obj, visit_cb cb, void *data)
{
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

/* A label with that text, its middle on the display */
static bool text_cb(lv_obj_t *obj, void *data)
{
	if (!lv_obj_check_type(obj, &lv_label_class) || strcmp(lv_label_get_text(obj), data) != 0)
		return false;
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	int32_t x = (a.x1 + a.x2) / 2, y = (a.y1 + a.y2) / 2;
	return x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT;
}

static bool texture_cb(lv_obj_t *obj, void *data)
{
	if (lv_obj_check_type(obj, &lv_3dtexture_class))
		(*(int *)data)++;
	return false; /* all of them */
}

static bool textarea_cb(lv_obj_t *obj, void *data)
{
	LV_UNUSED(data);
	return lv_obj_check_type(obj, &lv_textarea_class) && lv_obj_has_state(obj, LV_STATE_FOCUSED);
}

/* An app by its name in the menu, or "Menu"; false if there is none */
EMSCRIPTEN_KEEPALIVE bool probe_open(const char *app)
{
	if (strcmp(app, "Menu") == 0) {
		ui_open_menu();
		return true;
	}
	const struct ui_app *a = ui_app_find(app);
	if (a)
		ui_app_open(a);
	return a != NULL;
}

/* Where the text is shown, in the display's pixels: y * 65536 + x; -1 if it is not */
EMSCRIPTEN_KEEPALIVE int probe_find(const char *text)
{
	lv_obj_t *label = visit(text_cb, (void *)text);
	if (!label)
		return -1;
	lv_area_t a;
	lv_obj_get_coords(label, &a);
	return (a.y1 + a.y2) / 2 * 65536 + (a.x1 + a.x2) / 2;
}

/* Images drawn on the GPU: Gallery's thumbnails, covers */
EMSCRIPTEN_KEEPALIVE int probe_count_textures(void)
{
	int count = 0;
	visit(texture_cb, &count);
	return count;
}

/* Text added to the focused field, as if typed; false without one */
EMSCRIPTEN_KEEPALIVE bool probe_type(const char *text)
{
	lv_obj_t *ta = visit(textarea_cb, NULL);
	if (ta)
		lv_textarea_add_text(ta, text);
	return ta != NULL;
}
