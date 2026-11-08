/*
 * Transitions: both screens are rendered once by the GPU into textures, which then
 * slide on a screen of their own: two quads per frame, whatever the screens hold.
 * Drawing the screens themselves on every frame costs the CPU per object (style
 * lookups, a GL draw call each): a busy screen (Calendar) missed frames.
 * The textures are rendered at the start of the next refresh, once the screens'
 * layout is up to date (and the GPU draw unit only takes tasks during a refresh).
 * Without GL, LVGL's screen animations (and no back gesture).
 *
 * Sideways, as on the iPhone: the pushed screen slides over the one below, which
 * moves a third as much and darkens (the black background through it).
 * Going home (LV_SCREEN_LOAD_ANIM_OUT_TOP): the screen is lifted away over the home
 * screen, which stays and brightens.
 */

#include "ui_internal.h"
#include "src/display/lv_display_private.h" /* the active screen, set by transitions */

#define SLIDE_DIM LV_OPA_40 /* the screen below, when covered */

static struct {
	lv_obj_t *screen, *view[2]; /* lv_3dtexture of `from` and `to` */
	unsigned tex[2];
	lv_obj_t *from, *to;        /* to: NULL when there is no transition */
	lv_screen_load_anim_t anim;
	bool del;                   /* delete `from` at the end */
	bool rendered;              /* sliding, else waiting for the next refresh */
	bool dragging;              /* by a gesture, which sets `pos` */
	bool cancel;                /* back to `from` at the end (gesture let go early) */
	int32_t pos;                /* 0: `from` shown, slide_end(): `to` shown */
} slide;

static bool slide_sideways(void)
{
	return slide.anim == LV_SCREEN_LOAD_ANIM_MOVE_LEFT || slide.anim == LV_SCREEN_LOAD_ANIM_MOVE_RIGHT;
}

static int32_t slide_end(void)
{
	return slide_sideways() ? ui_screen_w : ui_screen_h;
}

void transition_finish(void)
{
	if (!slide.to)
		return;
	lv_obj_t *from = slide.from, *to = slide.to;
	/* A gesture still dragging is interrupted (another transition, theme change) */
	bool cancel = slide.cancel || slide.dragging;
	slide.to = NULL;
	if (slide.rendered)
		lv_anim_delete(&slide, NULL);
	slide.rendered = slide.dragging = slide.cancel = false;

	/* Not lv_screen_load(): the LOAD_START and UNLOAD_START events were sent at the start */
	lv_display_t *disp = lv_obj_get_display(to);
	if (cancel) {
		disp->act_scr = from;
		lv_obj_invalidate(from);
		return;
	}
	disp->act_scr = to;
	lv_obj_invalidate(to);
	lv_obj_send_event(to, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_send_event(from, LV_EVENT_SCREEN_UNLOADED, NULL);
	/* Possibly from one of its own events, while it was waiting for the refresh */
	if (slide.del)
		lv_obj_delete_async(from);
}

static void slide_done_cb(lv_anim_t *a)
{
	LV_UNUSED(a);
	slide.rendered = false; /* the animation is over */
	transition_finish();
}

static void slide_exec_cb(void *var, int32_t v)
{
	LV_UNUSED(var);
	int32_t screen_w = ui_screen_w, screen_h = ui_screen_h;
	slide.pos = v;
	if (slide_sideways()) {
		/* `shown`: how far the upper screen is in */
		bool push = slide.anim == LV_SCREEN_LOAD_ANIM_MOVE_LEFT;
		lv_obj_t *upper = slide.view[push], *lower = slide.view[!push];
		int32_t shown = push ? v : screen_w - v;
		lv_obj_set_pos(upper, screen_w - shown, 0);
		lv_obj_set_pos(lower, -shown / 3, 0);
		lv_obj_set_style_opa(lower, LV_OPA_COVER - SLIDE_DIM * shown / screen_w, 0);
		return;
	}
	if (slide.anim == LV_SCREEN_LOAD_ANIM_OUT_TOP) {
		lv_obj_set_pos(slide.view[0], 0, -v);
		lv_obj_set_style_opa(slide.view[1], LV_OPA_COVER - SLIDE_DIM * (screen_h - v) / screen_h, 0);
		return;
	}
	int32_t dy = slide.anim == LV_SCREEN_LOAD_ANIM_MOVE_TOP ? -1 : 1;
	lv_obj_set_pos(slide.view[0], 0, dy * v);
	lv_obj_set_pos(slide.view[1], 0, dy * (v - screen_h));
}

#if LV_USE_3DTEXTURE
/* Draws `scr` into `tex` through LVGL's draw units, as a refresh would */
static void slide_render(lv_obj_t *scr, unsigned tex)
{
	lv_area_t area = { 0, 0, ui_screen_w - 1, ui_screen_h - 1 };
	lv_layer_t layer;
	lv_draw_layer_init(&layer, NULL, LV_COLOR_FORMAT_ARGB8888, &area);
	layer.user_data = (void *)(uintptr_t)tex; /* the GPU draw unit renders into it */
	lv_obj_update_layout(scr);
	lv_obj_redraw(&layer, scr);
	layer.all_tasks_added = true;
	while (layer.draw_task_head) {
		lv_draw_dispatch_wait_for_request();
		lv_draw_dispatch();
	}
	lv_draw_layer_deinit(&layer);
}

static void slide_refr_start_cb(lv_event_t *e)
{
	if (!slide.to || slide.rendered)
		return;
	slide.rendered = true;
	slide_render(slide.from, slide.tex[0]);
	slide_render(slide.to, slide.tex[1]);

	lv_display_t *disp = lv_event_get_target(e);
	disp->act_scr = slide.screen;
	lv_obj_invalidate(slide.screen);
	/* The pushed screen on top, the one going away when going back or home */
	bool from_on_top = slide.anim == LV_SCREEN_LOAD_ANIM_MOVE_RIGHT ||
	                   slide.anim == LV_SCREEN_LOAD_ANIM_OUT_TOP;
	lv_obj_move_to_index(slide.view[!from_on_top], -1);
	for (int i = 0; i < 2; i++) {
		lv_obj_set_pos(slide.view[i], 0, 0);
		lv_obj_set_style_opa(slide.view[i], LV_OPA_COVER, 0);
	}
	slide_exec_cb(&slide, slide.pos);
}

/* The rendering took a while (the GPU too): the first frame shows the screens as they
 * were, the slide starts after it rather than skipping that time. After a gesture,
 * from where it was let go, slowing down. */
static void slide_refr_ready_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!slide.to || !slide.rendered || slide.dragging || lv_anim_get(&slide, NULL))
		return;
	int32_t target = slide.cancel ? 0 : slide_end();
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &slide);
	lv_anim_set_exec_cb(&a, slide_exec_cb);
	lv_anim_set_values(&a, slide.pos, target);
	lv_anim_set_duration(&a, ui_anim_ms * LV_ABS(target - slide.pos) / slide_end());
	if (slide.pos)
		lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, slide_done_cb);
	lv_anim_start(&a);
}

void transition_init(lv_display_t *disp)
{
	slide.screen = lv_obj_create(NULL);
	lv_obj_remove_style_all(slide.screen);
	lv_obj_set_scrollable(slide.screen, false);
	lv_obj_set_style_bg_color(slide.screen, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(slide.screen, LV_OPA_COVER, 0);
	for (int i = 0; i < 2; i++) {
		slide.tex[i] = ui_texture_create(NULL, ui_screen_w, ui_screen_h);
		slide.view[i] = lv_3dtexture_create(slide.screen);
		lv_obj_set_size(slide.view[i], ui_screen_w, ui_screen_h);
		lv_3dtexture_set_src(slide.view[i], slide.tex[i]);
		lv_3dtexture_set_flip(slide.view[i], false, true); /* rendered bottom-up */
	}
	lv_display_add_event_cb(disp, slide_refr_start_cb, LV_EVENT_REFR_START, NULL);
	lv_display_add_event_cb(disp, slide_refr_ready_cb, LV_EVENT_REFR_READY, NULL);
}

/* The display turned: textures of its new size */
void transition_resize(void)
{
	transition_finish();
	for (int i = 0; slide.screen && i < 2; i++) {
		ui_texture_delete(slide.tex[i]);
		slide.tex[i] = ui_texture_create(NULL, ui_screen_w, ui_screen_h);
		lv_obj_set_size(slide.view[i], ui_screen_w, ui_screen_h);
		lv_3dtexture_set_src(slide.view[i], slide.tex[i]);
	}
}
#endif

/* Shows `to` with a sliding `anim` (LV_SCREEN_LOAD_ANIM_MOVE_*) */
void transition_start(lv_obj_t *to, lv_screen_load_anim_t anim, bool del)
{
	ui_turn_for(to); /* upright first, unless it turns too */
	transition_finish();
	ui_chrome_update();
	if (!slide.screen || !ui_anim_ms) {
		lv_screen_load_anim(to, anim, ui_anim_ms, 0, del);
		return;
	}
	slide.from = lv_screen_active();
	slide.to = to;
	slide.anim = anim;
	slide.del = del;
	slide.pos = 0;
	slide.rendered = false;
	/* Before the rendering: screens update their content on these */
	lv_obj_send_event(slide.from, LV_EVENT_SCREEN_UNLOAD_START, NULL);
	lv_obj_send_event(to, LV_EVENT_SCREEN_LOAD_START, NULL);
}

bool transition_drag_begin(lv_obj_t *to, lv_screen_load_anim_t anim)
{
	if (slide.to || !slide.screen || !ui_anim_ms)
		return false;
	transition_start(to, anim, false);
	slide.dragging = true;
	return true;
}

bool transition_dragging(void)
{
	return slide.dragging;
}

void transition_drag(int32_t pos)
{
	slide.pos = LV_CLAMP(0, pos, slide_end());
	if (slide.rendered)
		slide_exec_cb(&slide, slide.pos);
}

int32_t transition_pos(void)
{
	return slide.pos;
}

/* Released: the slide finishes from where it is (slide_refr_ready_cb) */
void transition_drag_end(bool done, bool del)
{
	slide.dragging = false;
	if (done)
		slide.del = del;
	else
		slide.cancel = true;
}
