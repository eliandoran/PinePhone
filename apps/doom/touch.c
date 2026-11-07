/*
 * Doom's touch controls, as the usual ones of Doom ports (Delta Touch, the official
 * one), in the layout shown (doom.h): left of its middle a stick wherever the thumb
 * lands (moving from there walks and strafes, far runs), right of it dragging sideways
 * turns (as a mouse does); FIRE turns too, so the same thumb fires and aims. Each finger
 * keeps what it first touched: the stick, turning, or the buttons (it can slide from one
 * to another). The fingers are phone-ui's (ui_touch_*(): LVGL only sees one), read in the
 * engine's thread through its host's touch_read(), which tells it the keys held.
 */

#include <math.h>

#include "plugin.h"

#define STICK_DEAD 36 /* px from where the thumb landed before it moves */
#define RUN_PX 110    /* px: running */

static struct ui_touch *touch;
enum owner { NONE, STICK, TURN, BUTTONS };
static struct {
	enum owner owner;
	bool fire;        /* turning from FIRE: fires too */
	bool turn_pad;    /* turning, landed in the turning pad: its knob follows */
	int ox, oy;       /* where it landed in the layout (the stick's center); turning: last x */
} fingers[UI_TOUCH_FINGERS];

static int button_at(const struct doom_layout *l, int x, int y)
{
	for (unsigned i = 0; i < l->count; i++) {
		const struct doom_button *b = &l->buttons[i];
		if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h)
			return i;
	}
	return -1;
}

static bool in_pad(const struct doom_pad *p, int x, int y)
{
	int dx = x - p->x, dy = y - p->y;
	return p->r && dx * dx + dy * dy <= p->r * p->r;
}

/* A pad's knob at (dx, dy) from its center, kept within the pad */
static void knob_set(int pad, const struct doom_pad *p, int dx, int dy)
{
	int max = p->r - DOOM_KNOB_R;
	if (max <= 0)
		dx = dy = 0; /* no pad (sideways) */
	float len = sqrtf((float)(dx * dx + dy * dy));
	if (len > max && len > 0) {
		dx = (int)(dx * max / len);
		dy = (int)(dy * max / len);
	}
	__atomic_store_n(&engine.host.knob[pad][0], dx, __ATOMIC_RELAXED);
	__atomic_store_n(&engine.host.knob[pad][1], dy, __ATOMIC_RELAXED);
}

struct reading {
	int orient;
	void (*held)(const bool keys[256], int turn);
};

/* After a report: the keys the fingers hold now, the buttons and knobs drawn so */
static void report(const struct ui_touch_finger f[UI_TOUCH_FINGERS], void *user_data)
{
	const struct reading *r = user_data;
	const struct doom_layout *l = doom_layout_of(r->orient);
	unsigned fire = l->count - 1;
	bool want[256] = { false };
	unsigned pressed = 0;
	int turn = 0;
	int knob[2][2] = { { 0, 0 }, { 0, 0 } };

	for (int i = 0; i < UI_TOUCH_FINGERS; i++) {
		if (f[i].id < 0)
			continue;
		int x = f[i].x, y = f[i].y;
		if (f[i].landed) {
			int b = button_at(l, x, y);
			fingers[i].fire = b == (int)fire;
			fingers[i].owner = b == (int)fire ? TURN : b >= 0 ? BUTTONS : x < l->middle ? STICK : TURN;
			fingers[i].ox = x;
			fingers[i].oy = y;
			/* In the drawn pad: from its center, as a joystick */
			if (fingers[i].owner == STICK && in_pad(&l->move, x, y)) {
				fingers[i].ox = l->move.x;
				fingers[i].oy = l->move.y;
			}
			fingers[i].turn_pad = fingers[i].owner == TURN && !fingers[i].fire && in_pad(&l->turn, x, y);
		}
		if (fingers[i].owner == STICK) {
			int dx = x - fingers[i].ox, dy = y - fingers[i].oy;
			want[DOOM_KEY_UPARROW] |= dy < -STICK_DEAD;
			want[DOOM_KEY_DOWNARROW] |= dy > STICK_DEAD;
			want[DOOM_KEY_STRAFE_L] |= dx < -STICK_DEAD;
			want[DOOM_KEY_STRAFE_R] |= dx > STICK_DEAD;
			want[DOOM_KEY_RSHIFT] |= dx * dx + dy * dy > RUN_PX * RUN_PX;
			knob[0][0] = dx;
			knob[0][1] = dy;
		} else if (fingers[i].owner == TURN) {
			turn += x - fingers[i].ox;
			fingers[i].ox = x;
			if (fingers[i].fire) {
				want[DOOM_KEY_FIRE] = true;
				pressed |= 1u << fire;
			}
			if (fingers[i].turn_pad) {
				knob[1][0] = x - l->turn.x;
				knob[1][1] = y - l->turn.y;
			}
		} else {
			int b = button_at(l, x, y);
			if (b >= 0) {
				/* The back button is the plugin's (LVGL takes its taps) */
				if (l->buttons[b].key != DOOM_KEY_BACK)
					want[l->buttons[b].key] = true;
				pressed |= 1u << b;
			}
		}
	}
	r->held(want, turn);
	__atomic_store_n(&engine.host.pressed, pressed, __ATOMIC_RELAXED);
	knob_set(0, &l->move, knob[0][0], knob[0][1]);
	knob_set(1, &l->turn, knob[1][0], knob[1][1]);
}

static int rotation_of(int orient)
{
	return orient == DOOM_LEFT ? LV_DISPLAY_ROTATION_90 :
	       orient == DOOM_RIGHT ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_0;
}

static void touch_read(void (*held)(const bool keys[256], int turn))
{
	struct reading r = { __atomic_load_n(&engine.host.orient, __ATOMIC_RELAXED), held };
	ui_touch_read(touch, rotation_of(r.orient), report, &r);
}

static void touch_drop(void)
{
	ui_touch_drop(touch);
	__atomic_store_n(&engine.host.pressed, 0, __ATOMIC_RELAXED);
	for (int p = 0; p < 2; p++)
		for (int c = 0; c < 2; c++)
			__atomic_store_n(&engine.host.knob[p][c], 0, __ATOMIC_RELAXED);
}

void touch_start(void)
{
	touch = ui_touch_open("doom");
	engine.host.touch_read = touch_read;
	engine.host.touch_drop = touch_drop;
	touch_drop();
}

void touch_stop(void)
{
	ui_touch_close(touch);
	touch = NULL;
}
