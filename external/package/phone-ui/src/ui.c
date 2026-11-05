/*
 * Themes, wallpaper, screens and navigation, status bar, cached label texts,
 * app icons, vibration and settings.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <netinet/in.h>
#include <png.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "modem.h"
#include "ui.h"
#include "src/display/lv_display_private.h" /* the active screen, set by transitions */
#include "src/core/lv_obj_event_private.h"   /* the back gesture strip's hit test */

#define SETTINGS_DIR "/var/lib/phone-ui"
#define SETTINGS SETTINGS_DIR "/settings"

uint32_t ui_anim_ms = 220;
bool ui_haptics = true;

static int32_t screen_w, screen_h;
static lv_obj_t *status_bar, *status_time, *status_battery, *status_usb, *status_wifi;
static lv_obj_t *status_signal, *status_tech; /* mobile signal bars, "4G" while data is up */
static lv_obj_t *status_roaming; /* "R" after the bars while roaming */
static lv_obj_t *status_modem;   /* a symbol after the bars when something is wrong */
static lv_obj_t *status_battery_icon; /* drawn, right of status_battery (the percentage) */

/*
 * Labels: one copy of each distinct text, never freed. The GPU draw unit only
 * caches the texture of labels with static text, keyed by the text's address:
 * labels whose text changes (clock, battery, names) get an address per text this
 * way. There are only so many of them (times of day, battery levels...).
 */

/* Slot of `text` in an open addressing table: where it is, or the free slot for it */
static size_t intern_slot(char **table, size_t size, const char *text)
{
	size_t h = 2166136261u;
	for (const char *c = text; *c; c++)
		h = (h ^ (unsigned char)*c) * 16777619u;
	for (h &= size - 1; table[h] && strcmp(table[h], text) != 0; h = (h + 1) & (size - 1))
		;
	return h;
}

const char *ui_intern(const char *text)
{
	static char **table;
	static size_t size, count;

	if (count * 2 >= size) {
		size_t old_size = size;
		char **old = table;
		size = size ? size * 2 : 256;
		table = calloc(size, sizeof(*table));
		for (size_t i = 0; i < old_size; i++)
			if (old[i])
				table[intern_slot(table, size, old[i])] = old[i];
		free(old);
	}

	size_t i = intern_slot(table, size, text);
	if (!table[i]) {
		table[i] = strdup(text);
		count++;
	}
	return table[i];
}

/* Setting the same text still redraws the label: only set it when it changed */
void ui_set_text(lv_obj_t *label, const char *text)
{
	const char *s = ui_intern(text);
	if (lv_label_get_text(label) != s)
		lv_label_set_text_static(label, s);
}

void ui_set_text_fmt(lv_obj_t *label, const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	ui_set_text(label, buf);
}

/* Themes */

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

/*
 * Wallpaper: a blue to teal gradient with three soft glows (dark or pastel),
 * computed once per theme. A placeholder until wallpapers are image files (no
 * image decoder is built yet). Screens that dim it get a dimmed copy: drawing a
 * full screen translucent layer over it costs the GPU another full screen pass
 * on every frame. They are never freed: the GPU draw unit caches textures by
 * buffer address, a new buffer at a freed one's address would show the old one.
 */

#define WALL_STEP 8 /* the colors are computed every 8 pixels, interpolated in between */

/* By orientation (the display turned sideways: the home screen), then ui_light */
static lv_draw_buf_t *wallpaper[2][2];                       /* as computed */
static lv_draw_buf_t *wallpapers[2][2][LV_OPA_COVER + 1]; /* by dim level, made when needed */

static void wallpaper_color(int32_t x, int32_t y, int32_t w, int32_t h, bool light, float c[3])
{
	static const struct { float x, y, r, s; float c[3]; } glows[] = {
		{ 0.15f, 0.22f, 0.45f, 0.55f, { 255, 110, 170 } },
		{ 0.95f, 0.55f, 0.50f, 0.45f, {  70, 190, 255 } },
		{ 0.35f, 0.95f, 0.40f, 0.40f, { 110, 255, 200 } },
	};
	static const float base[2][3][3] = {
		{ { 16, 18, 52 }, { 52, 26, 96 }, { 8, 70, 96 } },
		{ { 196, 208, 248 }, { 226, 206, 246 }, { 192, 232, 236 } },
	};
	const float *top = base[light][0], *mid = base[light][1], *bot = base[light][2];

	float nx = (float)x / w, ny = (float)y / w;
	float t = (float)y / h * 0.85f + nx * 0.15f;
	for (int i = 0; i < 3; i++)
		c[i] = t < 0.5f ? top[i] + (mid[i] - top[i]) * t * 2
		                : mid[i] + (bot[i] - mid[i]) * (t - 0.5f) * 2;

	for (size_t g = 0; g < sizeof(glows) / sizeof(glows[0]); g++) {
		float dx = nx - glows[g].x, dy = ny - glows[g].y * h / w;
		float q = 1 + (dx * dx + dy * dy) / (glows[g].r * glows[g].r) * 6;
		float f = glows[g].s / (q * q);
		/* Light: tinted towards the glow's color, dark: lightened by it */
		for (int i = 0; i < 3; i++)
			c[i] += light ? (glows[g].c[i] - c[i]) * f * 0.5f
			              : (255 - c[i]) * f * glows[g].c[i] / 255;
	}
}

static lv_draw_buf_t *wallpaper_make(int32_t w, int32_t h, bool light)
{
	int32_t gw = w / WALL_STEP + 2, gh = h / WALL_STEP + 2;
	float (*grid)[3] = malloc(sizeof(*grid) * gw * gh);
	for (int32_t gy = 0; gy < gh; gy++)
		for (int32_t gx = 0; gx < gw; gx++)
			wallpaper_color(gx * WALL_STEP, gy * WALL_STEP, w, h, light, grid[gy * gw + gx]);

	lv_draw_buf_t *buf = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0);
	uint32_t seed = 1;

	for (int32_t y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(buf->data + y * buf->header.stride);
		const float (*g0)[3] = &grid[y / WALL_STEP * gw], (*g1)[3] = g0 + gw;
		float fy = (float)(y % WALL_STEP) / WALL_STEP;
		for (int32_t x = 0; x < w; x++) {
			int32_t gx = x / WALL_STEP;
			float fx = (float)(x % WALL_STEP) / WALL_STEP;

			/* A little noise against banding */
			seed = seed * 1103515245 + 12345;
			float n = (float)((seed >> 16) & 3) - 1.5f;
			uint32_t px = 0xff000000;
			for (int i = 0; i < 3; i++) {
				float top = g0[gx][i] + (g0[gx + 1][i] - g0[gx][i]) * fx;
				float bottom = g1[gx][i] + (g1[gx + 1][i] - g1[gx][i]) * fx;
				float v = top + (bottom - top) * fy + n;
				px |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << (16 - 8 * i);
			}
			row[x] = px;
		}
	}
	free(grid);
	return buf;
}

/* The status bar has no background: its texts stand on the screen's. The top of
 * every wallpaper is shaded for them as the bar was (40%), fading out over twice
 * its height; screens dimmed more than that are left as they are, the bar takes
 * their background. */
#define SCRIM LV_OPA_40
#define SCRIM_H (UI_STATUS_H * 2)

static lv_opa_t row_dim(lv_opa_t dim, int32_t y)
{
	if (y >= SCRIM_H)
		return dim;
	float t = (float)y / SCRIM_H, s = SCRIM * (1 - t * t * (3 - 2 * t));
	return s > dim ? (lv_opa_t)(s + 0.5f) : dim;
}

static const lv_draw_buf_t *wallpaper_get(lv_opa_t dim)
{
	bool side = screen_w > screen_h;
	lv_draw_buf_t **cached = &wallpapers[side][ui_light][dim];
	if (!*cached) {
		if (!wallpaper[side][ui_light])
			wallpaper[side][ui_light] = wallpaper_make(screen_w, screen_h, ui_light);
		/* What a fill of the shade color (black or white) with opacity `dim` (more
		 * at the top) over it would look like */
		lv_draw_buf_t *buf = lv_draw_buf_dup(wallpaper[side][ui_light]);
		for (int32_t y = 0; y < screen_h; y++) {
			lv_opa_t d = row_dim(dim, y);
			if (!d)
				continue;
			uint32_t keep = LV_OPA_COVER - d, add = (ui_light ? 255 : 0) * d + 127;
			uint8_t *p = buf->data + y * buf->header.stride;
			for (int32_t x = 0; x < screen_w * 4; x += 4)
				for (int i = 0; i < 3; i++)
					p[x + i] = (uint8_t)((p[x + i] * keep + add) / LV_OPA_COVER);
		}
		/* Never changes: the GPU draw unit only caches the texture of unmodifiable
		 * images, it renders and uploads modifiable ones again on every frame */
		lv_draw_buf_clear_flag(buf, LV_IMAGE_FLAGS_MODIFIABLE);
		*cached = buf;
	}
	return *cached;
}

/* Screens and navigation */

static lv_obj_t *stack[32]; /* stack[0]: home screen; Files pushes a screen per folder */
static unsigned depth;
static lv_obj_t *menu_screen;

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
	return slide_sideways() ? screen_w : screen_h;
}

static void edge_update(void);

static void slide_finish(void)
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
	slide_finish();
}

static void slide_exec_cb(void *var, int32_t v)
{
	LV_UNUSED(var);
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
	lv_area_t area = { 0, 0, screen_w - 1, screen_h - 1 };
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
	lv_obj_move_foreground(slide.view[!from_on_top]);
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

static void slide_init(lv_display_t *disp)
{
	slide.screen = lv_obj_create(NULL);
	lv_obj_remove_style_all(slide.screen);
	lv_obj_set_scrollable(slide.screen, false);
	lv_obj_set_style_bg_color(slide.screen, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(slide.screen, LV_OPA_COVER, 0);
	for (int i = 0; i < 2; i++) {
		slide.tex[i] = ui_texture_create(NULL, screen_w, screen_h);
		slide.view[i] = lv_3dtexture_create(slide.screen);
		lv_obj_set_size(slide.view[i], screen_w, screen_h);
		lv_3dtexture_set_src(slide.view[i], slide.tex[i]);
		lv_3dtexture_set_flip(slide.view[i], false, true); /* rendered bottom-up */
	}
	lv_display_add_event_cb(disp, slide_refr_start_cb, LV_EVENT_REFR_START, NULL);
	lv_display_add_event_cb(disp, slide_refr_ready_cb, LV_EVENT_REFR_READY, NULL);
}
#endif

/* Shows `to` with a sliding `anim` (LV_SCREEN_LOAD_ANIM_MOVE_*) */
static void slide_start(lv_obj_t *to, lv_screen_load_anim_t anim, bool del)
{
	ui_turn_for(to); /* upright first, unless it turns too */
	slide_finish();
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

/*
 * Back gesture, as on the iPhone: a swipe from the left edge drags the screen away,
 * uncovering the previous one. Let go past half way or flicked to the right, it goes
 * back; else it returns. A strip along the left edge, on the top layer, takes the
 * touches while there is a screen to go back to (not on the home screen or the menu),
 * except over a keyboard: its left keys reach under it.
 */

#define EDGE_W 28     /* px: the strip (the back buttons start at 12) */
#define EDGE_SLOP 12  /* px moved before the screen follows */
#define EDGE_FLICK 400 /* px/s */

static lv_obj_t *edge, *home_edge; /* home_edge: the home gesture's (below) */
static struct {
	int32_t x0;    /* where the screen started following */
	int32_t x, v;  /* last position, speed (px/s), along the gesture (y for home_edge) */
	uint32_t t;    /* of the last position */
} swipe;

/* Sideways, a full screen screen on top (ui_screen_turns()): no status bar, no back
 * gesture */
static bool chrome_hidden(void)
{
	return depth && ui_screen_fullscreen(stack[depth - 1]) && ui_sideways();
}

static void edge_update(void)
{
	bool none = depth <= 1 || stack[depth - 1] == menu_screen || chrome_hidden();
	if (edge)
		lv_obj_set_hidden(edge, none);
	if (home_edge)
		lv_obj_set_hidden(home_edge, none);
}

void ui_chrome_update(void)
{
	if (status_bar) {
		lv_obj_set_hidden(status_bar, chrome_hidden());
		/* The home and lock screens show the time big: not twice */
		lv_obj_set_hidden(status_time, depth <= 1 || ui_locked());
	}
	edge_update();
}

static void edge_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);

	if (code == LV_EVENT_PRESSED) {
		swipe.x0 = swipe.x = p.x;
		swipe.v = 0;
		swipe.t = lv_tick_get();
		return;
	}
	if (code == LV_EVENT_PRESSING) {
		if (!slide.dragging) {
			/* Not while a transition runs, or before the finger moved enough */
			if (slide.to || !slide.screen || !ui_anim_ms || depth <= 1 ||
			    p.x - swipe.x0 < EDGE_SLOP)
				return;
			slide_start(stack[depth - 2], LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, false);
			slide.dragging = true;
			swipe.x0 = p.x;
		}
		uint32_t dt = lv_tick_elaps(swipe.t);
		if (dt) {
			swipe.v = (swipe.v + (p.x - swipe.x) * 1000 / (int32_t)dt) / 2;
			swipe.x = p.x;
			swipe.t = lv_tick_get();
		}
		slide.pos = LV_CLAMP(0, p.x - swipe.x0, screen_w);
		if (slide.rendered)
			slide_exec_cb(&slide, slide.pos);
		return;
	}
	/* Released or lost: the slide finishes from where it is (slide_refr_ready_cb) */
	if (!slide.dragging)
		return;
	slide.dragging = false;
	if (lv_tick_elaps(swipe.t) > 100) /* held still before letting go */
		swipe.v = 0;
	if (swipe.v > EDGE_FLICK || (swipe.v > -EDGE_FLICK && slide.pos > screen_w / 2)) {
		depth--; /* as ui_pop() */
		slide.del = true;
		ui_chrome_update();
	} else {
		slide.cancel = true;
	}
}

static void edge_hit_test_cb(lv_event_t *e)
{
	lv_hit_test_info_t *hit = lv_event_get_hit_test_info(e);
	lv_point_t p = *hit->point;
	for (lv_obj_t *o = lv_indev_search_obj(lv_screen_active(), &p); o; o = lv_obj_get_parent(o))
		if (lv_obj_check_type(o, &lv_keyboard_class)) {
			hit->res = false;
			return;
		}
}

static void edge_init(void)
{
	edge = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(edge);
	lv_obj_set_size(edge, EDGE_W, screen_h - UI_STATUS_H);
	lv_obj_set_pos(edge, 0, UI_STATUS_H);
	lv_obj_set_scrollable(edge, false);
	lv_obj_set_scroll_chain(edge, false);
	lv_obj_set_gesture_bubble(edge, false);
	lv_obj_set_adv_hittest(edge, true);
	lv_obj_add_event_cb(edge, edge_hit_test_cb, LV_EVENT_HIT_TEST, NULL);
	lv_obj_add_event_cb(edge, edge_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(edge, edge_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(edge, edge_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(edge, edge_cb, LV_EVENT_PRESS_LOST, NULL);
	edge_update();
}

/*
 * Home gesture, as on the iPhone: a swipe up from the bottom edge lifts the screen
 * away, uncovering the home screen; the screens between are closed. Let go past a
 * quarter of the way or flicked up, it goes home; else it comes back. A strip along
 * the bottom edge with a handle bar, on the top layer, shown with the back gesture's,
 * except over a keyboard as that one (its bottom row reaches the edge).
 */

#define HOME_EDGE_H 24 /* px: the strip */
#define HANDLE_W 140
#define HANDLE_H 6

/* The screens between the home screen and the top one deleted, the top one left to
 * the transition going home */
static void unstack_to_home(void)
{
	for (unsigned i = 1; i < depth - 1; i++)
		if (stack[i] != menu_screen)
			lv_obj_delete_async(stack[i]);
	depth = 1;
}

static void home_edge_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);

	if (code == LV_EVENT_PRESSED) {
		swipe.x0 = swipe.x = p.y;
		swipe.v = 0;
		swipe.t = lv_tick_get();
		return;
	}
	if (code == LV_EVENT_PRESSING) {
		if (!slide.dragging) {
			if (slide.to || !slide.screen || !ui_anim_ms || depth <= 1 ||
			    swipe.x0 - p.y < EDGE_SLOP)
				return;
			slide_start(stack[0], LV_SCREEN_LOAD_ANIM_OUT_TOP, false);
			slide.dragging = true;
			swipe.x0 = p.y;
		}
		uint32_t dt = lv_tick_elaps(swipe.t);
		if (dt) {
			swipe.v = (swipe.v + (swipe.x - p.y) * 1000 / (int32_t)dt) / 2; /* up */
			swipe.x = p.y;
			swipe.t = lv_tick_get();
		}
		slide.pos = LV_CLAMP(0, swipe.x0 - p.y, screen_h);
		if (slide.rendered)
			slide_exec_cb(&slide, slide.pos);
		return;
	}
	if (!slide.dragging)
		return;
	slide.dragging = false;
	if (lv_tick_elaps(swipe.t) > 100)
		swipe.v = 0;
	if (swipe.v > EDGE_FLICK || (swipe.v > -EDGE_FLICK && slide.pos > screen_h / 4)) {
		unstack_to_home();
		slide.del = true; /* the top one: never the menu, which has no strip */
		ui_chrome_update();
	} else {
		slide.cancel = true;
	}
}

static void home_edge_init(void)
{
	home_edge = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(home_edge);
	lv_obj_set_size(home_edge, LV_PCT(100), HOME_EDGE_H);
	lv_obj_align(home_edge, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_scrollable(home_edge, false);
	lv_obj_set_scroll_chain(home_edge, false);
	lv_obj_set_gesture_bubble(home_edge, false);
	lv_obj_set_adv_hittest(home_edge, true);
	lv_obj_add_event_cb(home_edge, edge_hit_test_cb, LV_EVENT_HIT_TEST, NULL);
	lv_obj_add_event_cb(home_edge, home_edge_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(home_edge, home_edge_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(home_edge, home_edge_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(home_edge, home_edge_cb, LV_EVENT_PRESS_LOST, NULL);

	lv_obj_t *handle = ui_box(home_edge); /* colored by theme_apply() */
	lv_obj_set_size(handle, HANDLE_W, HANDLE_H);
	lv_obj_align(handle, LV_ALIGN_BOTTOM_MID, 0, -8);
	lv_obj_set_style_radius(handle, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(handle, LV_OPA_50, 0);
	lv_obj_set_style_bg_color(handle, lv_color_hex(UI_COLOR_TEXT), 0);
	edge_update();
}

void ui_set_screens(lv_obj_t *home, lv_obj_t *menu)
{
	slide_finish(); /* the screens it shows are deleted next (theme change) */
	stack[0] = home;
	depth = 1;
	menu_screen = menu;
	lv_screen_load(home);
	ui_chrome_update();
}

const lv_draw_buf_t *ui_wallpaper(lv_opa_t dim)
{
	return wallpaper_get(dim);
}

lv_obj_t *ui_screen_create(lv_opa_t dim, lv_obj_t **content)
{
	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_set_scrollable(scr, false);

	lv_obj_t *img = lv_image_create(scr);
	lv_image_set_src(img, wallpaper_get(dim));
	lv_obj_set_user_data(img, (void *)(uintptr_t)dim); /* for another when the display turns */

	lv_obj_t *c = lv_obj_create(scr);
	lv_obj_remove_style_all(c);
	lv_obj_set_size(c, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_pad_top(c, UI_STATUS_H, 0);
	lv_obj_set_style_text_color(c, lv_color_hex(UI_COLOR_TEXT), 0);
	if (content)
		*content = c;
	return scr;
}

static void back_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_pop();
}

/* A header's title w wide: centered, or from its start if it does not fit (cut at
 * the end, not on both sides) */
static void title_fit(lv_obj_t *label, int32_t w)
{
	lv_point_t size;
	lv_text_get_size(&size, lv_label_get_text(label), lv_obj_get_style_text_font(label, 0), 0, 0,
	                 LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	lv_obj_set_width(label, w);
	lv_obj_set_style_text_align(label, size.x > w ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
}

lv_obj_t *ui_app_screen_create(const char *title, lv_obj_t **header, lv_obj_t **body)
{
	lv_obj_t *content;
	lv_obj_t *scr = ui_screen_create(LV_OPA_70, &content);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *h = lv_obj_create(content);
	lv_obj_remove_style_all(h);
	lv_obj_set_size(h, LV_PCT(100), UI_HEADER_H);

	lv_obj_t *back = lv_button_create(h);
	lv_obj_set_size(back, 80, 72);
	lv_obj_align(back, LV_ALIGN_LEFT_MID, 12, 0);
	lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(back, 0, 0);
	lv_obj_set_style_text_color(back, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *icon = lv_label_create(back);
	lv_label_set_text_static(icon, LV_SYMBOL_LEFT);
	lv_obj_set_style_text_font(icon, &ui_font_32, 0);
	lv_obj_center(icon);

	lv_obj_t *label = lv_label_create(h);
	lv_label_set_text_static(label, title);
	lv_obj_set_style_text_font(label, &ui_font_32, 0);
	title_fit(label, screen_w - 2 * 104);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

	lv_obj_t *b = lv_obj_create(content);
	lv_obj_remove_style_all(b);
	lv_obj_set_width(b, LV_PCT(100));
	lv_obj_set_flex_grow(b, 1);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);

	if (header)
		*header = h;
	*body = b;
	return scr;
}

lv_obj_t *ui_header_button(lv_obj_t *header, const char *symbol, lv_event_cb_t cb, void *user_data)
{
	/* Further buttons go left of the previous ones */
	int32_t n = (int32_t)lv_obj_get_child_count(header) - 2;

	lv_obj_t *button = lv_button_create(header);
	lv_obj_set_size(button, 80, 72);
	lv_obj_align(button, LV_ALIGN_RIGHT_MID, -12 - 88 * n, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_text_color(button, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);
	lv_obj_t *icon = lv_label_create(button);
	lv_label_set_text_static(icon, symbol);
	lv_obj_set_style_text_font(icon, &ui_font_32, 0);
	lv_obj_center(icon);
	/* The title (centered, after the back button) clear of them */
	if (n > 0)
		title_fit(lv_obj_get_child(header, 1), screen_w - 2 * (104 + 88 * n));
	return button;
}

bool ui_at_home(void)
{
	return depth <= 1 || stack[depth - 1] == menu_screen;
}

lv_obj_t *ui_current(void)
{
	return stack[depth - 1];
}

void ui_push(lv_obj_t *screen)
{
	if (depth == sizeof(stack) / sizeof(stack[0])) {
		lv_obj_delete(screen);
		return;
	}
	stack[depth++] = screen;
	slide_start(screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, false);
}

void ui_open_menu(void)
{
	stack[depth++] = menu_screen;
	slide_start(menu_screen, LV_SCREEN_LOAD_ANIM_MOVE_TOP, false);
}

/* The menu dragged up from the home screen, or down from the menu: the slide follows
 * the finger, as for the back gesture; once let go, it finishes from there */
bool ui_menu_drag_begin(bool open)
{
	if (slide.to || !slide.screen || !ui_anim_ms ||
	    (open ? depth != 1 : (depth != 2 || stack[1] != menu_screen)))
		return false;
	if (open)
		slide_start(menu_screen, LV_SCREEN_LOAD_ANIM_MOVE_TOP, false);
	else
		slide_start(stack[0], LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM, false);
	slide.dragging = true;
	return true;
}

void ui_menu_drag(int32_t pos)
{
	if (!slide.dragging)
		return;
	slide.pos = LV_CLAMP(0, pos, screen_h);
	if (slide.rendered)
		slide_exec_cb(&slide, slide.pos);
}

void ui_menu_drag_end(bool done)
{
	if (!slide.dragging)
		return;
	slide.dragging = false;
	if (done && slide.anim == LV_SCREEN_LOAD_ANIM_MOVE_TOP)
		stack[depth++] = menu_screen; /* as ui_open_menu() */
	else if (done)
		depth--; /* as ui_pop() */
	else
		slide.cancel = true;
	ui_chrome_update();
}

void ui_pop(void)
{
	if (depth <= 1)
		return;
	lv_obj_t *top = stack[--depth];
	if (top == menu_screen)
		slide_start(stack[0], LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM, false);
	else
		slide_start(stack[depth - 1], LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, true);
}

void ui_home(void)
{
	if (depth <= 1)
		return;
	lv_obj_t *top = stack[depth - 1];
	unstack_to_home();
	if (top == menu_screen)
		slide_start(stack[0], LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM, false);
	else
		slide_start(stack[0], LV_SCREEN_LOAD_ANIM_OUT_TOP, true); /* as the home gesture */
}

/* Theme switching */

static void theme_apply(bool light)
{
	ui_light = light;
	ui_colors = light ? &light_colors : &dark_colors;
	lv_display_t *disp = lv_display_get_default();
	lv_display_set_theme(disp, lv_theme_default_init(disp, lv_color_hex(UI_COLOR_ACCENT),
	                                                 lv_palette_main(LV_PALETTE_TEAL), !light,
	                                                 &ui_font_24));
	if (status_bar) {
		lv_obj_set_style_text_color(status_bar, lv_color_hex(UI_COLOR_TEXT), 0);
		ui_status_update();
	}
	if (home_edge)
		lv_obj_set_style_bg_color(lv_obj_get_child(home_edge, 0), lv_color_hex(UI_COLOR_TEXT), 0);
}

static void (*rebuild_reopen)(void);

static void rebuild_cb(void *data)
{
	theme_apply((uintptr_t)data != 0);

	lv_obj_t *old[sizeof(stack) / sizeof(stack[0]) + 1];
	unsigned count = 0;
	bool menu_open = false;
	for (unsigned i = 0; i < depth; i++) {
		menu_open |= stack[i] == menu_screen;
		if (stack[i] != menu_screen)
			old[count++] = stack[i];
	}
	old[count++] = menu_screen;

	/* Without transitions: the new screens replace the old ones at once */
	uint32_t anim_ms = ui_anim_ms;
	ui_anim_ms = 0;
	home_create();
	if (menu_open)
		ui_open_menu();
	if (rebuild_reopen)
		rebuild_reopen();
	ui_anim_ms = anim_ms;

	for (unsigned i = 0; i < count; i++)
		lv_obj_delete(old[i]);
}

void ui_set_light(bool light, void (*reopen)(void))
{
	if (light == ui_light)
		return;
	ui_setting_set_int("light", light);
	rebuild_reopen = reopen;
	lv_async_call(rebuild_cb, (void *)(uintptr_t)light);
}

/* Apps */

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

/* Vibration: the gpio-vibrator input device, as a force feedback rumble effect */

void ui_vibrate(uint32_t ms)
{
	static int fd = -2;
	static struct ff_effect effect = { .type = FF_RUMBLE, .id = -1 };

	if (!ui_haptics)
		return;
	if (fd == -2) {
		fd = -1;
		DIR *dir = opendir("/dev/input");
		struct dirent *ent;
		while (dir && fd < 0 && (ent = readdir(dir))) {
			char path[300], name[64] = "";
			if (strncmp(ent->d_name, "event", 5) != 0)
				continue;
			snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
			int f = open(path, O_RDWR | O_CLOEXEC);
			if (f >= 0 && ioctl(f, EVIOCGNAME(sizeof(name)), name) >= 0 &&
			    strcmp(name, "gpio-vibrator") == 0)
				fd = f;
			else if (f >= 0)
				close(f);
		}
		if (dir)
			closedir(dir);
		if (fd < 0)
			printf("phone-ui: no vibrator\n");
	}
	if (fd < 0)
		return;

	effect.u.rumble.strong_magnitude = 0xffff;
	effect.replay.length = (uint16_t)ms;
	if (ioctl(fd, EVIOCSFF, &effect) < 0)
		return;
	struct input_event play = { .type = EV_FF, .code = (uint16_t)effect.id, .value = 1 };
	if (write(fd, &play, sizeof(play)) < 0)
		printf("phone-ui: vibrate: %s\n", strerror(errno));
}

/* Small files: sysfs attributes */

bool ui_read_line(const char *path, char *buf, size_t size)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	bool ok = fgets(buf, (int)size, f) != NULL;
	fclose(f);
	if (ok)
		buf[strcspn(buf, "\n")] = '\0';
	return ok;
}

bool ui_read_long(const char *path, long *v)
{
	char buf[32];
	char *end;
	if (!ui_read_line(path, buf, sizeof(buf)))
		return false;
	*v = strtol(buf, &end, 10);
	return end != buf;
}

void ui_write_long(const char *path, long v)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "%ld\n", v);
	fclose(f);
}

/* Settings: loaded once, written back on every change (they change rarely) */

static struct setting {
	char *key, *value;
} *settings;
static unsigned setting_count;
static bool settings_loaded;

static struct setting *setting_find(const char *key)
{
	if (!settings_loaded) {
		settings_loaded = true;
		FILE *f = fopen(SETTINGS, "r");
		char line[512];
		while (f && fgets(line, sizeof(line), f)) {
			char *eq = strchr(line, '=');
			if (!eq)
				continue;
			*eq = '\0';
			eq[strcspn(eq + 1, "\n") + 1] = '\0';
			settings = realloc(settings, sizeof(*settings) * (setting_count + 1));
			settings[setting_count].key = strdup(line);
			settings[setting_count++].value = strdup(eq + 1);
		}
		if (f)
			fclose(f);
	}
	for (unsigned i = 0; i < setting_count; i++)
		if (strcmp(settings[i].key, key) == 0)
			return &settings[i];
	return NULL;
}

const char *ui_setting_get(const char *key, const char *fallback)
{
	struct setting *s = setting_find(key);
	return s ? s->value : fallback;
}

int ui_setting_get_int(const char *key, int fallback)
{
	const char *v = ui_setting_get(key, NULL);
	return v ? atoi(v) : fallback;
}

void ui_setting_set(const char *key, const char *value)
{
	struct setting *s = setting_find(key);
	if (s && strcmp(s->value, value) == 0)
		return;
	if (s) {
		free(s->value);
		s->value = strdup(value);
	} else {
		settings = realloc(settings, sizeof(*settings) * (setting_count + 1));
		settings[setting_count].key = strdup(key);
		settings[setting_count++].value = strdup(value);
	}

	mkdir(SETTINGS_DIR, 0755);
	FILE *f = fopen(SETTINGS ".new", "w");
	if (!f) {
		printf("phone-ui: cannot save settings: %s\n", strerror(errno));
		return;
	}
	for (unsigned i = 0; i < setting_count; i++)
		fprintf(f, "%s=%s\n", settings[i].key, settings[i].value);
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	rename(SETTINGS ".new", SETTINGS);
}

void ui_setting_set_int(const char *key, int value)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", value);
	ui_setting_set(key, buf);
}

/* Status bar, on the top layer: it stays put while screens slide under it */

/*
 * The AXP803 reports a dead cell as present (and charging, at 0%): a Li-ion cell
 * never runs below ~2.5 V in use, its protection circuit cuts off around there.
 */
bool ui_battery_present(void)
{
	long present, uv;
	if (!ui_read_long(UI_BATTERY "/present", &present))
		present = 1;
	if (!ui_read_long(UI_BATTERY "/voltage_now", &uv))
		uv = -1;
	return present && (uv < 0 || uv >= 2500000);
}

/*
 * Time to full: the charge missing to 100% of the design capacity at the charging
 * current, averaged over the last minutes (exponentially, sampled by ui_status_update()):
 * above about 80% the charger holds the voltage and the current falls, which the average
 * follows. Just plugged in, the charger ramps up from a few mA: the average starts once
 * it has charged for CHARGE_SETTLE_MS, with no estimate before (it read days).
 */
#define CHARGE_AVG_MS 120000
#define CHARGE_SETTLE_MS 20000

static double charge_ua = -1; /* the average, -1 while not charging or settling */
static uint32_t charge_tick, charge_start;
static bool charge_on;

static void battery_sample(bool charging)
{
	long ua;
	if (!charging || !ui_read_long(UI_BATTERY "/current_now", &ua)) {
		charge_ua = -1;
		charge_on = false;
		return;
	}
	uint32_t now = lv_tick_get();
	if (!charge_on) {
		charge_on = true;
		charge_start = now;
	}
	if (lv_tick_diff(now, charge_start) < CHARGE_SETTLE_MS)
		charge_ua = -1;
	else if (charge_ua < 0)
		charge_ua = ua;
	else
		charge_ua += (ua - charge_ua) * (1 - exp(-(double)lv_tick_diff(now, charge_tick) / CHARGE_AVG_MS));
	charge_tick = now;
}

bool ui_battery_time_to_full(char *buf, size_t size)
{
	long capacity, full;
	if (charge_ua <= 0 || !ui_read_long(UI_BATTERY "/capacity", &capacity) || capacity >= 100 ||
	    !ui_read_long(UI_BATTERY "/charge_full_design", &full) || full <= 0)
		return false;
	long minutes = lround((100 - capacity) / 100.0 * full / charge_ua * 60);
	if (minutes < 1)
		minutes = 1;
	if (minutes < 60)
		snprintf(buf, size, "%ld min", minutes);
	else if (minutes % 60 == 0)
		snprintf(buf, size, "%ld h", minutes / 60);
	else
		snprintf(buf, size, "%ld h %ld min", minutes / 60, minutes % 60);
	return true;
}

/*
 * The battery icon: its state in the user data, the level + 2 (0 unknown, 1 absent) and
 * the charging bit above, drawn by battery_draw_cb() at the object's size. The body's
 * outline is half opaque, the fill and nub whole: the level reads first, as the signal's
 * lit bars do.
 */
#define BATTERY_CHARGING 0x100

static void battery_draw_cb(lv_event_t *e)
{
	lv_obj_t *obj = lv_event_get_current_target_obj(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	uintptr_t s = (uintptr_t)lv_obj_get_user_data(obj);
	int level = (int)(s & 0xff) - 2;
	bool charging = s & BATTERY_CHARGING;
	lv_color_t color = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	int32_t h = lv_area_get_height(&a);
	int32_t stroke = LV_MAX(h / 10, 2), gap = LV_MAX(stroke / 2, 1);
	int32_t nub = LV_MAX(h / 8, 2);

	/* The body, then the nub on the right, 1 px apart */
	lv_area_t body = { a.x1, a.y1, a.x2 - nub - 1, a.y2 };
	lv_draw_rect_dsc_t rect;
	lv_draw_rect_dsc_init(&rect);
	rect.radius = h / 4;
	rect.bg_opa = LV_OPA_TRANSP;
	rect.border_color = color;
	rect.border_width = stroke;
	rect.border_opa = LV_OPA_50;
	lv_draw_rect(layer, &rect, &body);

	lv_draw_rect_dsc_init(&rect);
	rect.bg_color = color;
	rect.bg_opa = LV_OPA_50;
	rect.radius = nub / 2;
	lv_area_t tip = { a.x2 - nub + 1, a.y1 + h * 3 / 10, a.x2, a.y2 - h * 3 / 10 };
	lv_draw_rect(layer, &rect, &tip);

	lv_area_t in = { body.x1 + stroke + gap, body.y1 + stroke + gap,
	                 body.x2 - stroke - gap, body.y2 - stroke - gap };
	int32_t in_w = lv_area_get_width(&in);

	if (level == UI_BATTERY_ABSENT) {
		/* Crossed out in red, corner to corner */
		lv_draw_line_dsc_t line;
		lv_draw_line_dsc_init(&line);
		line.color = lv_color_hex(UI_COLOR_RED);
		line.width = stroke + 1;
		line.round_start = line.round_end = 1;
		line.p1 = (lv_point_precise_t){ body.x1 + stroke, body.y2 - h / 6 };
		line.p2 = (lv_point_precise_t){ body.x2 - stroke, body.y1 + h / 6 };
		lv_draw_line(layer, &line);
		return;
	}
	/* A "?" or the bolt, centered on the inside */
	lv_draw_label_dsc_t label;
	lv_draw_label_dsc_init(&label);
	label.color = color;
	label.font = h < 40 ? &ui_font_16 : &ui_font_32;
	label.align = LV_TEXT_ALIGN_CENTER;
	lv_area_t text = in;
	int32_t font_h = lv_font_get_line_height(label.font);
	text.y1 = (in.y1 + in.y2 + 1 - font_h) / 2;
	text.y2 = text.y1 + font_h - 1;
	if (level == UI_BATTERY_UNKNOWN) {
		label.text = "?";
		lv_draw_label(layer, &label, &text);
		return;
	}

	int32_t fill_w = level <= 0 ? 0 : LV_MAX((in_w * level + 50) / 100, 2);
	if (fill_w > 0) {
		lv_draw_rect_dsc_init(&rect);
		rect.bg_color = charging ? lv_color_hex(UI_COLOR_GREEN) :
		                level < 15 ? lv_color_hex(UI_COLOR_RED) : color;
		rect.radius = LV_MAX(h / 4 - stroke - gap, 1);
		lv_area_t fill = { in.x1, in.y1, in.x1 + fill_w - 1, in.y2 };
		lv_draw_rect(layer, &rect, &fill);
	}

	/* The bolt in the text color: it reads over the fill and the empty part alike */
	if (charging) {
		label.text = LV_SYMBOL_CHARGE;
		lv_draw_label(layer, &label, &text);
	}
}

lv_obj_t *ui_battery_icon_create(lv_obj_t *parent, int32_t w, int32_t h)
{
	lv_obj_t *icon = ui_box(parent);
	lv_obj_set_size(icon, w, h);
	lv_obj_set_user_data(icon, (void *)(uintptr_t)(UI_BATTERY_UNKNOWN + 2));
	lv_obj_add_event_cb(icon, battery_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
	return icon;
}

void ui_battery_icon_set(lv_obj_t *icon, int level, bool charging)
{
	level = LV_CLAMP(UI_BATTERY_ABSENT, level, 100);
	uintptr_t s = (uintptr_t)(level + 2) | (charging && level >= 0 ? BATTERY_CHARGING : 0);
	if ((uintptr_t)lv_obj_get_user_data(icon) == s)
		return;
	lv_obj_set_user_data(icon, (void *)s);
	lv_obj_invalidate(icon);
}

void ui_battery_icon_update(lv_obj_t *icon)
{
	long capacity;
	char status[32];
	if (!ui_read_long(UI_BATTERY "/capacity", &capacity))
		ui_battery_icon_set(icon, UI_BATTERY_UNKNOWN, false);
	else if (!ui_battery_present())
		ui_battery_icon_set(icon, UI_BATTERY_ABSENT, false);
	else
		ui_battery_icon_set(icon, (int)capacity, ui_read_line(UI_BATTERY "/status", status, sizeof(status)) &&
		                    strncmp(status, "Charging", 8) == 0);
}

bool ui_battery_icon_get(lv_obj_t *obj, int *level, bool *charging)
{
	uint32_t n = lv_obj_get_event_count(obj);
	bool found = false;
	for (uint32_t i = 0; i < n && !found; i++)
		found = lv_event_dsc_get_cb(lv_obj_get_event_dsc(obj, i)) == battery_draw_cb;
	if (!found)
		return false;
	uintptr_t s = (uintptr_t)lv_obj_get_user_data(obj);
	if (level)
		*level = (int)(s & 0xff) - 2;
	if (charging)
		*charging = s & BATTERY_CHARGING;
	return true;
}

/*
 * Internet over the USB cable: the NCM link is up (usb0's carrier) and the host
 * forwards our traffic (`just share-internet`). That is probed with a TCP connect
 * to the DNS server, non-blocking: started every PROBE_EVERY s, its result read by
 * the following updates, given up after PROBE_TIMEOUT s.
 */
#define USB_NET "/sys/class/net/usb0"
#define PROBE_EVERY 10
#define PROBE_TIMEOUT 5

static int usb_probe_fd = -1;
static time_t usb_probe_time;
static bool usb_online;
static int usb_state; /* usb_net_state() at the last update */
static int wifi_bars = -1; /* wifi_level() at the last update */

int ui_wifi_bars(void)
{
	return wifi_bars;
}

int ui_usb_net(void)
{
	return usb_state;
}

static void usb_probe_end(bool online)
{
	close(usb_probe_fd);
	usb_probe_fd = -1;
	usb_online = online;
}

static void usb_probe_start(time_t now)
{
	usb_probe_time = now;
	usb_probe_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (usb_probe_fd < 0)
		return;
	/* Through the host only: the default route may be a Wi-Fi network's */
	setsockopt(usb_probe_fd, SOL_SOCKET, SO_BINDTODEVICE, "usb0", sizeof("usb0"));
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(53),
		.sin_addr.s_addr = htonl(0x09090909), /* 9.9.9.9, the first nameserver (S05usbgadget) */
	};
	if (connect(usb_probe_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
		usb_probe_end(true);
	else if (errno != EINPROGRESS)
		usb_probe_end(false);
}

/* 0: no USB network, 1: USB network without internet, 2: internet */
static int usb_net_state(time_t now)
{
	long carrier;
	if (!ui_read_long(USB_NET "/carrier", &carrier) || !carrier) {
		if (usb_probe_fd >= 0)
			usb_probe_end(false);
		usb_online = false;
		usb_probe_time = 0; /* probe as soon as it comes back */
		return 0;
	}

	if (usb_probe_fd >= 0) {
		struct pollfd pfd = { .fd = usb_probe_fd, .events = POLLOUT };
		if (poll(&pfd, 1, 0) > 0) {
			int err = 1;
			socklen_t len = sizeof(err);
			getsockopt(usb_probe_fd, SOL_SOCKET, SO_ERROR, &err, &len);
			usb_probe_end(err == 0);
		} else if (now - usb_probe_time >= PROBE_TIMEOUT) {
			usb_probe_end(false);
		}
	} else if (now - usb_probe_time >= PROBE_EVERY || now < usb_probe_time) {
		usb_probe_start(now); /* also when the clock went back (NTP) */
	}
	return usb_online ? 2 : 1;
}

/* Wi-Fi: a dot and three arcs fanning up from it, in the text color as far as the
 * signal's bars (user data, 0 to 4) reach, faint beyond */
static void wifi_draw_cb(lv_event_t *e)
{
	lv_obj_t *obj = lv_event_get_current_target_obj(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	int bars = (int)(intptr_t)lv_obj_get_user_data(obj);
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	int32_t h = lv_area_get_height(&a);
	int32_t w = LV_MAX(h / 8, 2); /* stroke */
	int32_t r = h - w * 2;        /* the outer arc's outer edge, 2 px below the top: not clipped */

	lv_draw_arc_dsc_t arc;
	lv_draw_arc_dsc_init(&arc);
	arc.color = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
	arc.center = (lv_point_t){ (a.x1 + a.x2) / 2, a.y2 - w };
	arc.rounded = 1;
	for (int i = 0; i < 4; i++) {
		arc.opa = i < bars ? LV_OPA_COVER : LV_OPA_30;
		arc.radius = i == 0 ? w + 1 : w * 2 + i * (r - w * 2) / 3;
		arc.width = i == 0 ? arc.radius : w;
		arc.start_angle = i == 0 ? 0 : 225; /* clockwise from the right: the top quarter */
		arc.end_angle = i == 0 ? 360 : 315;
		lv_draw_arc(layer, &arc);
	}
}

/* Mobile signal: four bars rising left to right, in the text color as far as the
 * bars (user data, -1 to 4: none lit while not registered) reach, faint beyond */
static void signal_draw_cb(lv_event_t *e)
{
	lv_obj_t *obj = lv_event_get_current_target_obj(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	int bars = (int)(intptr_t)lv_obj_get_user_data(obj);
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	int32_t gap = 3, w = (lv_area_get_width(&a) - 3 * gap) / 4, h = lv_area_get_height(&a);

	lv_draw_rect_dsc_t rect;
	lv_draw_rect_dsc_init(&rect);
	rect.bg_color = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
	rect.radius = 2;
	for (int i = 0; i < 4; i++) {
		rect.bg_opa = i < bars ? LV_OPA_COVER : LV_OPA_30;
		lv_area_t bar = { a.x1 + i * (w + gap), a.y2 - h * (i + 1) / 4 + 1, 0, a.y2 };
		bar.x2 = bar.x1 + w - 1;
		lv_draw_rect(layer, &rect, &bar);
	}
}

/* The left of the status bar, in order: the mobile signal, roaming's "R", its
 * technology, the modem's state, Wi-Fi, USB; 16 px apart (the "R", the technology or
 * the state 6 px from the bars or the "R"), what is hidden or empty skipped */
static void status_layout(void)
{
	lv_obj_t *items[] = { status_signal, status_roaming, status_tech, status_modem, status_wifi, status_usb };
	lv_obj_t *prev = NULL;
	int32_t x = 0;
	for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
		lv_obj_t *o = items[i];
		if (lv_obj_is_hidden(o) || (lv_obj_check_type(o, &lv_label_class) && !lv_label_get_text(o)[0]))
			continue;
		if (prev)
			x += (prev == status_signal || prev == status_roaming) &&
			     (o == status_roaming || o == status_tech || o == status_modem) ? 6 : 16;
		lv_obj_update_layout(o);
		lv_obj_align(o, LV_ALIGN_LEFT_MID, x, 0);
		x += lv_obj_get_width(o);
		prev = o;
	}
}

void ui_status_update(void)
{
	char buf[64];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(buf, sizeof(buf), "%H:%M", &tm);
	ui_set_text(status_time, buf);

	/* What the left side shows; laid out again when that changes */
	bool relayout = false;

	/* The mobile signal while there is a modem: bars lit while registered */
	int signal = modem_signal_bars();
	bool signal_hidden = modem_state() == MODEM_NONE;
	if (signal_hidden != lv_obj_is_hidden(status_signal) ||
	    signal != (int)(intptr_t)lv_obj_get_user_data(status_signal)) {
		lv_obj_set_hidden(status_signal, signal_hidden);
		lv_obj_set_user_data(status_signal, (void *)(intptr_t)signal);
		lv_obj_invalidate(status_signal);
		relayout = true;
	}
	/* Roaming: an "R", as Android; information, in the text color */
	if (modem_roaming() == lv_obj_is_hidden(status_roaming)) {
		lv_obj_set_hidden(status_roaming, !modem_roaming());
		relayout = true;
	}
	/* Mobile data while its session is up: the technology ("4G"), as Android;
	 * otherwise the technology is the shade's */
	bool data = modem_data_state() == MODEM_DATA_CONNECTED;
	const char *tech = data ? modem_technology_text() : "";
	if (strcmp(lv_label_get_text(status_tech), tech) != 0) {
		lv_label_set_text_static(status_tech, tech);
		relayout = true;
	}

	/* The modem's state when something is wrong: a symbol (no SIM, no service), red
	 * when waiting will not help (no modem, SIM blocked) */
	const char *state = modem_status_icon();
	if (strcmp(lv_label_get_text(status_modem), state) != 0) {
		lv_label_set_text_static(status_modem, state);
		relayout = true;
	}
	lv_color_t state_color = lv_color_hex(modem_status_alarm() ? UI_COLOR_RED : UI_COLOR_TEXT);
	if (!lv_color_eq(lv_obj_get_style_text_color(status_modem, 0), state_color))
		lv_obj_set_style_text_color(status_modem, state_color, 0);

	/* Wi-Fi while connected, its bars the signal (all dim until it has an address) */
	int bars = wifi_level();
	if (bars != wifi_bars) {
		wifi_bars = bars;
		lv_obj_set_hidden(status_wifi, bars < 0);
		lv_obj_set_user_data(status_wifi, (void *)(intptr_t)bars);
		lv_obj_invalidate(status_wifi);
		relayout = true;
	}

	/* USB symbol while plugged into the host, dim until it shares its internet */
	int usb = usb_state = usb_net_state(now);
	if ((usb == 0) != lv_obj_is_hidden(status_usb)) {
		lv_obj_set_hidden(status_usb, usb == 0);
		relayout = true;
	}
	if (relayout)
		status_layout();
	lv_obj_set_style_text_color(status_usb, usb == 2 ?
	                            lv_color_hex(UI_COLOR_TEXT) : lv_color_hex(UI_COLOR_DIM), 0);

	long capacity;
	char status[32];
	if (!ui_read_long(UI_BATTERY "/capacity", &capacity))
		capacity = -1;
	if (!ui_read_line(UI_BATTERY "/status", status, sizeof(status)))
		status[0] = '\0';
	bool charging = strncmp(status, "Charging", 8) == 0;
	battery_sample(charging);

	/* Only the icon without a driver (a "?") or a usable battery (crossed out) */
	if (capacity < 0 || !ui_battery_present()) {
		ui_battery_icon_set(status_battery_icon, capacity < 0 ? UI_BATTERY_UNKNOWN : UI_BATTERY_ABSENT, false);
		ui_set_text(status_battery, "");
		return;
	}
	battery_log_sample((int)capacity, charging);
	ui_battery_icon_set(status_battery_icon, (int)capacity, charging);
	/* The percentage only if asked (Settings, Battery): the fill shows the level */
	if (ui_setting_get_int("battery_percent", 0)) {
		snprintf(buf, sizeof(buf), "%ld%%", capacity);
		ui_set_text(status_battery, buf);
	} else {
		ui_set_text(status_battery, "");
	}
}

void ui_status_bar_foreground(void)
{
	lv_obj_move_foreground(status_bar);
}

/* A screen's wallpaper (ui_screen_create()) for the screen's size now */
static void wallpaper_refit(lv_obj_t *scr)
{
	lv_obj_t *img = scr ? lv_obj_get_child(scr, 0) : NULL;
	if (img && lv_obj_check_type(img, &lv_image_class))
		lv_image_set_src(img, wallpaper_get((lv_opa_t)(uintptr_t)lv_obj_get_user_data(img)));
}

/* The display turned (home.c): what was made for its size, made again for the new one */
static void resolution_cb(lv_event_t *e)
{
	lv_display_t *disp = lv_event_get_target(e);
	screen_w = lv_display_get_horizontal_resolution(disp);
	screen_h = lv_display_get_vertical_resolution(disp);
#if LV_USE_3DTEXTURE
	slide_finish();
	for (int i = 0; slide.screen && i < 2; i++) {
		ui_texture_delete(slide.tex[i]);
		slide.tex[i] = ui_texture_create(NULL, screen_w, screen_h);
		lv_obj_set_size(slide.view[i], screen_w, screen_h);
		lv_3dtexture_set_src(slide.view[i], slide.tex[i]);
	}
	if (edge)
		lv_obj_set_size(edge, EDGE_W, screen_h - UI_STATUS_H);
#endif
	for (unsigned i = 0; i < depth; i++)
		wallpaper_refit(stack[i]);
	wallpaper_refit(menu_screen);
}

void ui_fps_counter_set(bool on)
{
#if LV_USE_PERF_MONITOR
	if (on)
		lv_sysmon_show_performance(lv_display_get_default());
	else
		lv_sysmon_hide_performance(lv_display_get_default());
#else
	LV_UNUSED(on);
#endif
}

void ui_init(lv_display_t *disp)
{
	screen_w = lv_display_get_horizontal_resolution(disp);
	screen_h = lv_display_get_vertical_resolution(disp);
	theme_apply(ui_setting_get_int("light", 0) != 0);
	ui_fps_counter_set(ui_setting_get_int("fps_counter", 0) != 0);

	lv_obj_t *bar = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, LV_PCT(100), UI_STATUS_H);
	/* No background: the wallpapers are shaded at the top (wallpaper_get()) */
	lv_obj_set_style_pad_hor(bar, 20, 0);
	lv_obj_set_style_text_color(bar, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_text_font(bar, &ui_font_24, 0);

	/* The left side, placed by status_layout() */
	modem_init();
	status_signal = ui_box(bar);
	lv_obj_set_size(status_signal, 30, 24);
	lv_obj_set_user_data(status_signal, (void *)(intptr_t)modem_signal_bars());
	lv_obj_add_event_cb(status_signal, signal_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_set_hidden(status_signal, modem_state() == MODEM_NONE);

	status_roaming = lv_label_create(bar);
	lv_obj_set_style_text_font(status_roaming, &ui_font_20, 0);
	lv_label_set_text_static(status_roaming, "R");
	lv_obj_set_hidden(status_roaming, !modem_roaming());

	status_tech = lv_label_create(bar);
	lv_obj_set_style_text_font(status_tech, &ui_font_20, 0);
	lv_label_set_text_static(status_tech, "");

	status_modem = lv_label_create(bar);
	lv_obj_set_style_text_font(status_modem, &ui_font_20, 0);
	lv_label_set_text_static(status_modem, modem_status_icon());

	status_wifi = ui_box(bar);
	lv_obj_set_size(status_wifi, 36, 26);
	lv_obj_add_event_cb(status_wifi, wifi_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_set_hidden(status_wifi, true);

	status_usb = lv_label_create(bar);
	lv_label_set_text_static(status_usb, LV_SYMBOL_USB);
	lv_obj_set_hidden(status_usb, true);
	status_layout();

	status_time = lv_label_create(bar);
	ui_set_text(status_time, "");
	lv_obj_align(status_time, LV_ALIGN_CENTER, 0, 0);

	/* The percentage right-aligned 6 px left of the battery: it grows to the left */
	status_battery_icon = ui_battery_icon_create(bar, 40, 20);
	lv_obj_align(status_battery_icon, LV_ALIGN_RIGHT_MID, 0, 0);
	status_battery = lv_label_create(bar);
	ui_set_text(status_battery, "");
	lv_obj_align(status_battery, LV_ALIGN_RIGHT_MID, -(40 + 6), 0);

	status_bar = bar; /* recolored by theme_apply() from now on */
	ui_shade_init(bar);
#if LV_USE_3DTEXTURE
	slide_init(disp);
	edge_init();
	home_edge_init();
#endif
	lv_display_add_event_cb(disp, resolution_cb, LV_EVENT_RESOLUTION_CHANGED, NULL);
}
