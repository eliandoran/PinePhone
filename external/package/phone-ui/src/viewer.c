/*
 * Photo viewer (ui_photo_view), for the Gallery and apps: one photo fitted on
 * the screen, swipe left or right for the next or previous one, a caption and
 * the position under it, delete it from the header if the caller can.
 *
 * Photos are decoded scaled down to the screen (ui_image_read) into GL textures
 * shown by lv_3dtexture. The photos on each side of the current one are kept
 * decoded, so that they follow the finger with it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

#if LV_USE_3DTEXTURE

#define PHOTO_GAP 32 /* between photos while sliding */
#define SLIDE_MS 250 /* a whole photo's width */
#define FLICK_MS 300 /* a swipe shorter than that turns the photo... */
#define FLICK 40     /* ...once it moved that far, else it takes a quarter of the width */

/* A photo in the viewer, decoded when needed */
struct slot {
	int index;     /* in view.paths, out of range: none */
	bool loaded;
	unsigned tex;
	lv_obj_t *quad;
};

static struct {
	lv_obj_t *scr, *stage, *info, *caption, *pos, *confirm;
	char **paths;
	int count;
	struct ui_photo_view ops;
	struct slot slots[3]; /* previous, current and next photo, side by side */
	int index;
	int32_t pitch;        /* from one photo to the next */
	int32_t offset;       /* of the row of photos, while dragged or sliding */
	int32_t start;        /* offset when the finger went down */
	lv_point_t press;
	uint32_t press_time;
	bool dragging, sliding;
	lv_timer_t *preload;
} view;

static bool photo_exists(int index)
{
	return index >= 0 && index < view.count;
}

static void slot_set(struct slot *s, int index)
{
	ui_texture_delete(s->tex);
	s->tex = 0;
	s->index = index;
	s->loaded = false;
	lv_obj_set_hidden(s->quad, true);
}

/* Decodes the slot's photo fitted into the stage, if not done yet */
static void slot_load(struct slot *s)
{
	if (s->loaded)
		return;
	s->loaded = true;
	if (!photo_exists(s->index))
		return;
	int32_t bw = lv_obj_get_content_width(view.stage);
	int32_t bh = lv_obj_get_content_height(view.stage);
	int w, h;
	uint8_t *pixels = ui_image_read(view.paths[s->index], bw, bh, false, &w, &h);
	if (!pixels)
		return;
	s->tex = ui_texture_create(pixels, w, h);
	free(pixels);
	float scale = ui_fit_scale(w, h, bw, bh, false);
	lv_obj_set_size(s->quad, (int32_t)(w * scale), (int32_t)(h * scale));
	lv_3dtexture_set_src(s->quad, s->tex);
	lv_obj_set_hidden(s->quad, false);
	lv_obj_invalidate(s->quad);
}

static void view_layout(void)
{
	for (int k = 0; k < 3; k++)
		lv_obj_align(view.slots[k].quad, LV_ALIGN_CENTER, (k - 1) * view.pitch + view.offset, 0);
}

static void slide_exec_cb(void *var, int32_t v)
{
	LV_UNUSED(var);
	view.offset = v;
	view_layout();
}

static void view_info(void)
{
	char buf[128] = "";
	if (view.ops.caption)
		view.ops.caption(view.index, buf, sizeof(buf), view.ops.user_data);
	ui_set_text(view.caption, buf);
	lv_obj_set_hidden(view.caption, !buf[0]);
	if (view.count > 1)
		ui_set_text_fmt(view.pos, "%d of %d", view.index + 1, view.count);
	lv_obj_set_hidden(view.pos, view.count <= 1);
	/* Nothing to say: the photo takes the room */
	lv_obj_set_hidden(view.info, !buf[0] && view.count <= 1);
}

/* Shows photo `index` now, its neighbors are decoded after */
static void view_show(int index)
{
	lv_anim_delete(&view, slide_exec_cb);
	view.sliding = false;
	view.index = index;
	view_info();
	/* The stage's size depends on the info shown */
	lv_obj_update_layout(view.scr);
	for (int k = 0; k < 3; k++)
		slot_set(&view.slots[k], index + k - 1);
	slot_load(&view.slots[1]);
	view.offset = 0;
	view_layout();
	if (view.confirm)
		lv_obj_set_hidden(view.confirm, true);
	lv_timer_resume(view.preload);
}

/* The neighbors, one per tick, never during a swipe (the UI waits for them) */
static void preload_cb(lv_timer_t *timer)
{
	if (view.dragging || view.sliding)
		return;
	for (int k = 2; k >= 0; k -= 2) {
		if (!view.slots[k].loaded) {
			slot_load(&view.slots[k]);
			return;
		}
	}
	lv_timer_pause(timer);
}

/* Slid to a neighbor: it becomes the current photo, the one beyond it is next to
 * decode */
static void slide_done_cb(lv_anim_t *a)
{
	LV_UNUSED(a);
	view.sliding = false;
	int dir = view.offset < 0 ? 1 : view.offset > 0 ? -1 : 0;
	if (!dir)
		return;
	struct slot *s = view.slots, old;
	if (dir > 0) {
		old = s[0];
		s[0] = s[1];
		s[1] = s[2];
		s[2] = old;
		slot_set(&s[2], view.index + 2);
	} else {
		old = s[2];
		s[2] = s[1];
		s[1] = s[0];
		s[0] = old;
		slot_set(&s[0], view.index - 2);
	}
	view.index += dir;
	view.offset = 0;
	view_layout();
	view_info();
	if (view.confirm)
		lv_obj_set_hidden(view.confirm, true);
	lv_timer_resume(view.preload);
}

static void slide_to(int32_t target)
{
	int32_t dist = LV_ABS(target - view.offset);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &view);
	lv_anim_set_values(&a, view.offset, target);
	lv_anim_set_duration(&a, LV_MAX(SLIDE_MS * dist / view.pitch, 100));
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_exec_cb(&a, slide_exec_cb);
	lv_anim_set_completed_cb(&a, slide_done_cb);
	view.sliding = true;
	lv_anim_start(&a);
}

/* The photos follow the finger; let go, they slide to the nearest one, or the
 * next in the direction of a flick */
static void stage_cb(lv_event_t *e)
{
	lv_point_t pt;
	lv_indev_get_point(lv_indev_active(), &pt);
	bool prev = photo_exists(view.index - 1), next = photo_exists(view.index + 1);

	switch (lv_event_get_code(e)) {
	case LV_EVENT_PRESSED:
		/* Caught while sliding: carry on from there */
		if (view.sliding) {
			lv_anim_delete(&view, slide_exec_cb);
			view.sliding = false;
		}
		/* Not decoded yet (a quick second swipe): now rather than mid-swipe */
		slot_load(&view.slots[0]);
		slot_load(&view.slots[2]);
		view.press = pt;
		view.press_time = lv_tick_get();
		view.start = view.offset;
		view.dragging = true;
		break;
	case LV_EVENT_PRESSING: {
		int32_t offset = view.start + pt.x - view.press.x;
		/* Nothing on that side: it resists */
		if ((offset > 0 && !prev) || (offset < 0 && !next))
			offset /= 3;
		view.offset = offset;
		view_layout();
		break;
	}
	case LV_EVENT_RELEASED:
	case LV_EVENT_PRESS_LOST: {
		if (!view.dragging)
			break;
		view.dragging = false;
		int32_t dx = pt.x - view.press.x;
		bool flick = lv_tick_elaps(view.press_time) < FLICK_MS && LV_ABS(dx) > FLICK;
		int32_t target = 0;
		if (next && (view.offset < -view.pitch / 4 || (flick && dx < 0)))
			target = -view.pitch;
		else if (prev && (view.offset > view.pitch / 4 || (flick && dx > 0)))
			target = view.pitch;
		if (target != view.offset)
			slide_to(target);
		else
			slide_done_cb(NULL);
		break;
	}
	default:
		break;
	}
}

static void trash_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_set_hidden(view.confirm, !lv_obj_is_hidden(view.confirm));
}

static void cancel_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_set_hidden(view.confirm, true);
}

/* Deleted by the caller: shows the next one (or the previous, the last one was
 * deleted), or goes back once there is none left */
static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!view.ops.remove(view.index, view.ops.user_data))
		return;
	ui_vibrate(UI_VIBRATE_TAP);
	free(view.paths[view.index]);
	memmove(view.paths + view.index, view.paths + view.index + 1,
	        (view.count - view.index - 1) * sizeof(*view.paths));
	view.count--;
	if (!view.count) {
		ui_pop();
		return;
	}
	view_show(view.index < view.count ? view.index : view.count - 1);
}

static void view_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_anim_delete(&view, NULL);
	lv_timer_delete(view.preload);
	for (int k = 0; k < 3; k++)
		ui_texture_delete(view.slots[k].tex);
	for (int i = 0; i < view.count; i++)
		free(view.paths[i]);
	free(view.paths);
	memset(&view, 0, sizeof(view));
}

void ui_photo_view(const char *title, const char *const *paths, int count, int index,
                   const struct ui_photo_view *ops)
{
	if (view.scr || count <= 0 || index < 0 || index >= count)
		return;
	view.paths = calloc(count, sizeof(*view.paths));
	if (!view.paths)
		return;
	for (int i = 0; i < count; i++)
		view.paths[i] = strdup(paths[i]);
	view.count = count;
	if (ops)
		view.ops = *ops;

	lv_obj_t *header, *body;
	view.scr = ui_app_screen_create(title, &header, &body);
	lv_obj_add_event_cb(view.scr, view_delete_cb, LV_EVENT_DELETE, NULL);
	if (view.ops.remove)
		ui_header_button(header, LV_SYMBOL_TRASH, trash_cb, NULL);
	lv_obj_set_scrollable(body, false);

	/* The photo, fitted and centered, its neighbors on each side; swipes
	 * anywhere on it */
	view.stage = lv_obj_create(body);
	lv_obj_remove_style_all(view.stage);
	lv_obj_set_width(view.stage, LV_PCT(100));
	lv_obj_set_flex_grow(view.stage, 1);
	lv_obj_set_scrollable(view.stage, false);
	lv_obj_set_clickable(view.stage, true);
	lv_obj_add_event_cb(view.stage, stage_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(view.stage, stage_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(view.stage, stage_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(view.stage, stage_cb, LV_EVENT_PRESS_LOST, NULL);
	for (int k = 0; k < 3; k++) {
		view.slots[k].quad = lv_3dtexture_create(view.stage);
		lv_obj_set_clickable(view.slots[k].quad, false);
	}
	view.preload = lv_timer_create(preload_cb, 30, NULL);

	view.info = ui_box(body);
	lv_obj_set_size(view.info, LV_PCT(100), 112);
	lv_obj_set_flex_flow(view.info, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(view.info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(view.info, 4, 0);
	view.caption = lv_label_create(view.info);
	lv_obj_set_style_text_color(view.caption, lv_color_hex(UI_COLOR_TEXT), 0);
	view.pos = lv_label_create(view.info);
	lv_obj_set_style_text_font(view.pos, &ui_font_20, 0);
	lv_obj_set_style_text_color(view.pos, lv_color_hex(UI_COLOR_DIM), 0);

	/* Delete confirmation, shown by the trash button, in place of the info */
	if (view.ops.remove) {
		view.confirm = ui_confirm_bar(body, LV_SYMBOL_TRASH "  Delete photo", delete_cb, cancel_cb, NULL);
		lv_obj_set_floating(view.confirm, true);
		lv_obj_align(view.confirm, LV_ALIGN_BOTTOM_MID, 0, 0);
		/* Presses on it do not reach the photos below */
		lv_obj_set_clickable(view.confirm, true);
	}

	lv_obj_update_layout(view.scr);
	view.pitch = lv_obj_get_width(view.stage) + PHOTO_GAP;
	view_show(index);
	ui_push(view.scr);
}

void ui_photo_view_replace(const char *old_path, const char *new_path)
{
	for (int i = 0; i < view.count; i++) {
		if (strcmp(view.paths[i], old_path))
			continue;
		char *path = strdup(new_path);
		if (!path)
			return;
		free(view.paths[i]);
		view.paths[i] = path;
		/* Decoded again: now if shown, else with the neighbors */
		for (int k = 0; k < 3; k++) {
			struct slot *s = &view.slots[k];
			if (s->index != i || !s->loaded)
				continue;
			slot_set(s, i);
			if (k == 1)
				slot_load(s);
			else
				lv_timer_resume(view.preload);
		}
	}
}

#else

void ui_photo_view(const char *title, const char *const *paths, int count, int index,
                   const struct ui_photo_view *ops)
{
	LV_UNUSED(paths);
	LV_UNUSED(count);
	LV_UNUSED(index);
	LV_UNUSED(ops);
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(title, NULL, &body);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *label = lv_label_create(body);
	lv_label_set_text_static(label, "Photos need the GPU renderer");
	ui_push(scr);
}

void ui_photo_view_replace(const char *old_path, const char *new_path)
{
	LV_UNUSED(old_path);
	LV_UNUSED(new_path);
}

#endif
