/*
 * Doom: the game's screen (plugin.h). Upright, the game twice its size below the
 * header with the controls under it; turned sideways (phone-ui turns the display,
 * ui_screen_turns(): full screen), three times its size in the middle with the
 * controls around it, as Doom ports have them. The layouts are doom.h's: the
 * engine finds what the fingers touch in them (it reads the touchscreen: LVGL only
 * sees one finger) and says which buttons are held and where the pads' knobs are,
 * for them to be drawn so. Doom's frames come into a texture.
 */

#include <stdio.h>
#include <string.h>

#include <GLES2/gl2.h>

#include "plugin.h"

#define COLOR_BUTTON 0x2a2a2a
#define COLOR_PRESSED 0x6a1a12
#define COLOR_TEXT 0xd0d0d0
#define COLOR_HINT 0x606060
#define COLOR_PAD 0x1a1a1a

/* LVGL's thread; laid out again when the display turns */
static struct {
	char title[128];
	lv_obj_t *scr, *header, *body, *view, *controls;
	lv_obj_t *buttons[10];
	lv_obj_t *knob[2];       /* the pads' (move, turn); NULL: no pad */
	int knob_x[2], knob_y[2]; /* where they rest */
	int shown_knob[2][2];
	lv_timer_t *timer;
	GLuint tex;
	unsigned shown_seq, shown_pressed;
	uint32_t boosted;
} dm;

static void button_set_pressed(lv_obj_t *obj, bool pressed)
{
	lv_obj_set_style_bg_color(obj, lv_color_hex(pressed ? COLOR_PRESSED : COLOR_BUTTON), 0);
}

static void back_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_pop();
}

static lv_obj_t *label_create(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	return label;
}

/* A filled circle of `d` centered on (x, y) in dm.controls */
static lv_obj_t *disc_create(int x, int y, int d, uint32_t color, int origin)
{
	lv_obj_t *obj = ui_box(dm.controls);
	lv_obj_set_pos(obj, x - d / 2, y - d / 2 - origin);
	lv_obj_set_size(obj, d, d);
	lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	return obj;
}

/* A thumb's pad: a disc (opaque fills: no border, see ui.h) with a knob that
 * follows the thumb (dm.knob[pad], at its center at rest), named at its bottom */
static void pad_create(int pad, const struct doom_pad *p, const char *name, int origin)
{
	dm.knob[pad] = NULL;
	if (!p->r)
		return;
	disc_create(p->x, p->y, 2 * p->r, COLOR_PAD, origin);
	lv_obj_t *label = label_create(dm.controls, name, &ui_font_20, COLOR_HINT);
	lv_obj_align(label, LV_ALIGN_TOP_MID, p->x - lv_obj_get_width(dm.body) / 2,
	             p->y + p->r - 40 - origin);
	dm.knob[pad] = disc_create(p->x, p->y, 2 * DOOM_KNOB_R, COLOR_BUTTON, origin);
	dm.knob_x[pad] = p->x - DOOM_KNOB_R;
	dm.knob_y[pad] = p->y - DOOM_KNOB_R - origin;
	dm.shown_knob[pad][0] = dm.shown_knob[pad][1] = 0;
}

/* The pads, buttons and hints of a layout, in dm.controls (made again), whose top
 * is at y `origin` of the layout */
static void controls_create(const struct doom_layout *l, int origin)
{
	if (dm.controls)
		lv_obj_delete(dm.controls);
	dm.controls = ui_box(dm.body);
	lv_obj_set_size(dm.controls, LV_PCT(100), LV_PCT(100));
	memset(dm.buttons, 0, sizeof(dm.buttons));
	pad_create(0, &l->move, "MOVE", origin);
	pad_create(1, &l->turn, "TURN", origin);
	for (unsigned i = 0; i < l->count; i++) {
		const struct doom_button *b = &l->buttons[i];
		/* A gap of 4 px around each */
		lv_obj_t *obj = ui_box(dm.controls);
		lv_obj_set_pos(obj, b->x + 4, b->y + 4 - origin);
		lv_obj_set_size(obj, b->w - 8, b->h - 8);
		/* Rounded squares (as squircles: 30% of their side), pills for the low ones */
		int32_t side = LV_MIN(b->w, b->h) - 8;
		lv_obj_set_style_radius(obj, b->round ? side * 3 / 10 : b->h < 100 ? LV_RADIUS_CIRCLE : 12, 0);
		lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
		button_set_pressed(obj, false);
		lv_obj_t *label = label_create(obj, b->key == DOOM_KEY_BACK ? LV_SYMBOL_LEFT : b->label,
		                               b->h >= 100 ? &ui_font_32 : &ui_font_24, COLOR_TEXT);
		lv_obj_center(label);
		if (b->key == DOOM_KEY_BACK) {
			lv_obj_set_clickable(obj, true);
			lv_obj_add_event_cb(obj, back_cb, LV_EVENT_CLICKED, NULL);
		}
		dm.buttons[i] = obj;
	}
	const int hint_x[2] = { l->hint_move_x, l->hint_turn_x };
	static const char *const hints[2] = { "MOVE", "TURN" };
	for (int i = 0; i < 2 && l->hint_y; i++) {
		if (!hint_x[i])
			continue;
		lv_obj_t *hint = label_create(dm.controls, hints[i], &ui_font_24, COLOR_HINT);
		lv_obj_align(hint, LV_ALIGN_TOP_MID, hint_x[i] - lv_obj_get_width(dm.body) / 2,
		             l->hint_y - 12 - origin);
	}
	dm.shown_pressed = ~0u; /* drawn as held or not on the next frame */
}

static int orient_now(void)
{
	switch (lv_display_get_rotation(lv_display_get_default())) {
	case LV_DISPLAY_ROTATION_90: return DOOM_LEFT;
	case LV_DISPLAY_ROTATION_270: return DOOM_RIGHT;
	default: return DOOM_PORTRAIT;
	}
}

/* As the display is turned: upright below the status bar and the header, sideways
 * all of the screen (phone-ui hides the status bar) */
static void layout(void)
{
	int o = orient_now();
	bool side = o != DOOM_PORTRAIT;
	const struct doom_layout *l = doom_layout_of(o);
	int origin = side ? 0 : DOOM_BODY_Y;
	lv_obj_set_hidden(dm.header, side);
	lv_obj_set_style_pad_top(lv_obj_get_parent(dm.body), side ? 0 : UI_STATUS_H, 0);
	lv_obj_update_layout(dm.scr); /* the body's size, for the hints */
	controls_create(l, origin);
	lv_obj_set_pos(dm.view, l->game_x, l->game_y - origin);
	lv_obj_set_size(dm.view, DOOM_W * l->scale, DOOM_H * l->scale);
	__atomic_store_n(&engine.host.orient, o, __ATOMIC_RELAXED);
}

static void resolution_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (dm.scr)
		layout();
}

/* Doom's frames into the texture, the buttons held; Doom quitting closes the app */
static void frame_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	if (engine.running && __atomic_load_n(&engine.host.exited, __ATOMIC_ACQUIRE)) {
		engine_end();
		if (ui_current() == dm.scr)
			ui_pop();
		return;
	}

	unsigned pressed = __atomic_load_n(&engine.host.pressed, __ATOMIC_RELAXED);
	if (pressed != dm.shown_pressed) {
		for (unsigned i = 0; i < sizeof(dm.buttons) / sizeof(dm.buttons[0]); i++)
			if (dm.buttons[i])
				button_set_pressed(dm.buttons[i], pressed >> i & 1);
		dm.shown_pressed = pressed;
	}
	for (int p = 0; p < 2; p++) {
		int x = __atomic_load_n(&engine.host.knob[p][0], __ATOMIC_RELAXED);
		int y = __atomic_load_n(&engine.host.knob[p][1], __ATOMIC_RELAXED);
		if (dm.knob[p] && (x != dm.shown_knob[p][0] || y != dm.shown_knob[p][1])) {
			lv_obj_set_pos(dm.knob[p], dm.knob_x[p] + x, dm.knob_y[p] + y);
			dm.shown_knob[p][0] = x;
			dm.shown_knob[p][1] = y;
		}
	}

	/* main.c lowers the CPU's speed a second after the last touch: up again */
	if (lv_tick_elaps(dm.boosted) >= 500) {
		ui_cpu_boost(true);
		dm.boosted = lv_tick_get();
	}

	if (__atomic_load_n(&engine.host.frame_seq, __ATOMIC_RELAXED) == dm.shown_seq)
		return;
	pthread_mutex_lock(&engine.host.lock);
	bool created = !dm.tex;
	if (created) {
		glGenTextures(1, &dm.tex);
		glBindTexture(GL_TEXTURE_2D, dm.tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, DOOM_W, DOOM_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, engine.host.frame);
	} else {
		glBindTexture(GL_TEXTURE_2D, dm.tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DOOM_W, DOOM_H, GL_RGBA, GL_UNSIGNED_BYTE, engine.host.frame);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	dm.shown_seq = engine.host.frame_seq;
	pthread_mutex_unlock(&engine.host.lock);

	if (created) {
		lv_3dtexture_set_src(dm.view, dm.tex);
		lv_obj_set_hidden(dm.view, false);
	}
	lv_obj_invalidate(dm.view);
}


/* Left (or ended): Doom waits for the app to open again */
static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (engine.running)
		__atomic_store_n(&engine.host.paused, true, __ATOMIC_RELEASE);
	lv_display_remove_event_cb_with_user_data(lv_display_get_default(), resolution_cb, NULL);
	if (dm.timer)
		lv_timer_delete(dm.timer);
	dm.timer = NULL;
	if (dm.tex)
		glDeleteTextures(1, &dm.tex);
	dm.tex = 0;
	dm.scr = dm.header = dm.body = dm.view = dm.controls = NULL;
	memset(dm.buttons, 0, sizeof(dm.buttons));
	dm.knob[0] = dm.knob[1] = NULL;
	ui_cpu_boost(false);
	list_game_left();
}


/* The game's screen, Doom with `iwad` started (another one paused ended first) or
 * resumed */
void game_open(const char *iwad, const char *title)
{
	if (engine.running && strcmp(engine.iwad, iwad) != 0)
		engine_stop();
	snprintf(dm.title, sizeof(dm.title), "%s", title);

	dm.scr = ui_app_screen_create(dm.title, &dm.header, &dm.body);
	lv_obj_add_event_cb(dm.scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_layout(dm.body, LV_LAYOUT_NONE);
	lv_obj_set_scrollable(dm.body, false);
	lv_obj_set_style_bg_color(dm.body, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(dm.body, LV_OPA_COVER, 0);

	dm.view = lv_3dtexture_create(dm.body);
	lv_obj_set_clickable(dm.view, false);
	lv_obj_set_hidden(dm.view, true); /* until the first frame */
	dm.controls = NULL;
	dm.shown_seq = engine.host.frame_seq - 1; /* Doom's last frame shown again if paused */

	/* Turns with the phone, sideways without the status bar */
	ui_screen_turns(dm.scr, true);
	layout();
	lv_display_add_event_cb(lv_display_get_default(), resolution_cb, LV_EVENT_RESOLUTION_CHANGED, NULL);

	bool ok = engine.running || engine_start(iwad);
	if (!ok) {
		lv_obj_t *label = ui_empty_label(dm.body, _("Cannot start Doom"));
		lv_obj_set_pos(label, 0, 0);
	} else {
		__atomic_store_n(&engine.host.paused, false, __ATOMIC_RELEASE);
		dm.timer = lv_timer_create(frame_cb, 4, NULL);
		ui_cpu_boost(true);
		dm.boosted = lv_tick_get();
	}
	ui_push(dm.scr);
}
