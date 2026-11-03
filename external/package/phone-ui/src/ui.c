/*
 * Themes, wallpaper, screens and navigation, status bar, cached label texts,
 * app icons, vibration and settings.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ui.h"

#define BATTERY "/sys/class/power_supply/axp20x-battery"
#define SETTINGS_DIR "/var/lib/phone-ui"
#define SETTINGS SETTINGS_DIR "/settings"

uint32_t ui_anim_ms = 220;
bool ui_haptics = true;

static int32_t screen_w, screen_h;
static lv_obj_t *status_bar, *status_time, *status_battery;

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

static lv_draw_buf_t *wallpaper[2];                       /* as computed, by ui_light */
static lv_draw_buf_t *wallpapers[2][LV_OPA_COVER + 1]; /* by dim level, made when needed */

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

static const lv_draw_buf_t *wallpaper_get(lv_opa_t dim)
{
	lv_draw_buf_t **cached = &wallpapers[ui_light][dim];
	if (!*cached) {
		if (!wallpaper[ui_light])
			wallpaper[ui_light] = wallpaper_make(screen_w, screen_h, ui_light);
		lv_draw_buf_t *buf = wallpaper[ui_light];
		if (dim) {
			/* What a fill of the shade color (black or white) with opacity `dim`
			 * over it would look like */
			buf = lv_draw_buf_dup(buf);
			uint32_t keep = LV_OPA_COVER - dim, add = (ui_light ? 255 : 0) * dim + 127;
			for (int32_t y = 0; y < screen_h; y++) {
				uint8_t *p = buf->data + y * buf->header.stride;
				for (int32_t x = 0; x < screen_w * 4; x += 4)
					for (int i = 0; i < 3; i++)
						p[x + i] = (uint8_t)((p[x + i] * keep + add) / LV_OPA_COVER);
			}
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

void ui_set_screens(lv_obj_t *home, lv_obj_t *menu)
{
	stack[0] = home;
	depth = 1;
	menu_screen = menu;
	lv_screen_load(home);
}

lv_obj_t *ui_screen_create(lv_opa_t dim, lv_obj_t **content)
{
	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_set_scrollable(scr, false);

	lv_obj_t *img = lv_image_create(scr);
	lv_image_set_src(img, wallpaper_get(dim));

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
	lv_obj_set_width(label, screen_w - 2 * 104);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
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
	return button;
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
	lv_screen_load_anim(screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, ui_anim_ms, 0, false);
}

void ui_open_menu(void)
{
	stack[depth++] = menu_screen;
	lv_screen_load_anim(menu_screen, LV_SCREEN_LOAD_ANIM_MOVE_TOP, ui_anim_ms, 0, false);
}

void ui_pop(void)
{
	if (depth <= 1)
		return;
	lv_obj_t *top = stack[--depth];
	if (top == menu_screen)
		lv_screen_load_anim(stack[0], LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM, ui_anim_ms, 0, false);
	else
		lv_screen_load_anim(stack[depth - 1], LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, ui_anim_ms, 0, true);
}

void ui_home(void)
{
	if (depth <= 1)
		return;
	lv_obj_t *top = stack[depth - 1];
	for (unsigned i = 1; i < depth - 1; i++)
		if (stack[i] != menu_screen)
			lv_obj_delete_async(stack[i]);
	depth = 1;
	lv_screen_load_anim(stack[0], LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM, ui_anim_ms, 0,
	                    top != menu_screen);
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
		lv_obj_set_style_bg_color(status_bar, lv_color_hex(UI_COLOR_SHADE), 0);
		lv_obj_set_style_text_color(status_bar, lv_color_hex(UI_COLOR_TEXT), 0);
		ui_status_update();
	}
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

lv_obj_t *ui_app_icon_create(lv_obj_t *parent, const struct ui_app *app, int32_t size)
{
	lv_obj_t *icon = lv_obj_create(parent);
	lv_obj_remove_style_all(icon);
	lv_obj_set_size(icon, size, size);
	lv_obj_set_style_radius(icon, size / 4, 0);
	lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(icon, lv_color_hex(app->color), 0);
	lv_obj_set_clickable(icon, false);

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

/*
 * Keyboard: LVGL's keyboard widget with a phone layout and its own key handler
 * (LVGL's only knows its own mode keys). Keys type on release, except backspace,
 * which repeats while held. Shift lasts one character, twice quickly locks it;
 * sentences start in upper case.
 */

enum kb_mode { KB_LOWER, KB_UPPER, KB_LOCK, KB_SYMBOLS, KB_MORE };

#define KB_SHIFT LV_SYMBOL_UP
#define KB_LOCKED LV_SYMBOL_UPLOAD /* an arrow on a bar: caps lock */
#define KB_TO_SYMBOLS "?123"
#define KB_TO_LETTERS "ABC"
#define KB_TO_MORE "=\\<"
#define KB_DOUBLE_MS 400           /* shift twice within this: caps lock */

#define K(w) (LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT | (w))
#define KC(w) (K(w) | LV_BUTTONMATRIX_CTRL_CHECKED) /* control keys, gray */
#define KB_GAP (LV_BUTTONMATRIX_CTRL_HIDDEN | 1)   /* half a key, row 2 */
#define KB_BACKSPACE (LV_BUTTONMATRIX_CTRL_CHECKED | 3)

/* 20 units per row; the bottom row is the same in every mode except its first key */
#define KB_BOTTOM(mode_key) mode_key, LV_SYMBOL_KEYBOARD, ",", " ", ".", LV_SYMBOL_NEW_LINE, ""
#define KB_BOTTOM_CTRL KC(3), KC(2), K(2), K(8), K(2), KC(3)

static const char *const kb_lower[] = {
	"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
	" ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
	KB_SHIFT, "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
	KB_BOTTOM(KB_TO_SYMBOLS),
};

static const char *const kb_upper[] = {
	"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
	" ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
	KB_SHIFT, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
	KB_BOTTOM(KB_TO_SYMBOLS),
};

static const char *const kb_lock[] = {
	"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
	" ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
	KB_LOCKED, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
	KB_BOTTOM(KB_TO_SYMBOLS),
};

/* Shift unchecked (as light as the letters) while it is on */
static const lv_buttonmatrix_ctrl_t kb_letters_ctrl[2][36] = {
	{
		K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2),
		KB_GAP, K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), KB_GAP,
		KC(3), K(2), K(2), K(2), K(2), K(2), K(2), K(2), KB_BACKSPACE,
		KB_BOTTOM_CTRL,
	},
	{
		K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2),
		KB_GAP, K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), KB_GAP,
		K(3), K(2), K(2), K(2), K(2), K(2), K(2), K(2), KB_BACKSPACE,
		KB_BOTTOM_CTRL,
	},
};

/* Symbols as on Android (Gboard): ?123, then =\< for the second page. The pages
 * have the same key sizes: switching does not move them. */

static const char *const kb_symbols[] = {
	"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
	"@", "#", "$", "_", "&", "-", "+", "(", ")", "/", "\n",
	KB_TO_MORE, "*", "\"", "'", ":", ";", "!", "?", LV_SYMBOL_BACKSPACE, "\n",
	KB_BOTTOM(KB_TO_LETTERS),
};

static const lv_buttonmatrix_ctrl_t kb_symbols_ctrl[] = {
	K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2),
	K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2),
	KC(3), K(2), K(2), K(2), K(2), K(2), K(2), K(2), KB_BACKSPACE,
	KB_BOTTOM_CTRL,
};

static const char *const kb_more[] = {
	"~", "`", "|", "•", "√", "π", "÷", "×", "¶", "∆", "\n",
	"£", "¢", "€", "¥", "^", "°", "=", "{", "}", "\\", "\n",
	KB_TO_SYMBOLS, "%", "©", "®", "™", "✓", "[", "]", LV_SYMBOL_BACKSPACE, "\n",
	KB_TO_LETTERS, LV_SYMBOL_KEYBOARD, "<", " ", ">", LV_SYMBOL_NEW_LINE, "",
};

static uint32_t kb_shift_time;

static void kb_set_mode(lv_obj_t *kb, enum kb_mode mode)
{
	static const char *const *const maps[] = { kb_lower, kb_upper, kb_lock, kb_symbols, kb_more };
	static const lv_buttonmatrix_ctrl_t *const ctrls[] = {
		kb_letters_ctrl[0], kb_letters_ctrl[1], kb_letters_ctrl[1], kb_symbols_ctrl, kb_symbols_ctrl,
	};
	lv_obj_set_user_data(kb, (void *)(uintptr_t)mode);
	lv_buttonmatrix_set_map(kb, maps[mode]);
	lv_buttonmatrix_set_ctrl_map(kb, ctrls[mode]);
}

static enum kb_mode kb_get_mode(lv_obj_t *kb)
{
	return (enum kb_mode)(uintptr_t)lv_obj_get_user_data(kb);
}

/* The cursor is at the start of the text, of a line, or after ". " (or ! ?) */
static bool kb_sentence_start(lv_obj_t *ta)
{
	const char *text = lv_textarea_get_text(ta);
	/* The cursor position counts characters, not bytes */
	size_t i = 0;
	for (uint32_t n = lv_textarea_get_cursor_pos(ta); n && text[i]; n--)
		for (i++; ((unsigned char)text[i] & 0xc0) == 0x80; i++)
			;
	size_t end = i;
	while (i && text[i - 1] == ' ')
		i--;
	return i == 0 || text[i - 1] == '\n' ||
	       (i < end && (text[i - 1] == '.' || text[i - 1] == '!' || text[i - 1] == '?'));
}

/* Letters in the case for the cursor position, unless caps lock or symbols are on */
static void kb_auto_shift(lv_obj_t *kb)
{
	enum kb_mode mode = kb_get_mode(kb);
	if (mode != KB_LOWER && mode != KB_UPPER)
		return;
	enum kb_mode want = kb_sentence_start(lv_keyboard_get_textarea(kb)) ? KB_UPPER : KB_LOWER;
	if (want != mode)
		kb_set_mode(kb, want);
}

static void kb_key_cb(lv_event_t *e)
{
	lv_obj_t *kb = lv_event_get_target_obj(e);
	lv_obj_t *ta = lv_keyboard_get_textarea(kb);
	uint32_t id = lv_buttonmatrix_get_selected_button(kb);
	if (id == LV_BUTTONMATRIX_BUTTON_NONE || !ta)
		return;
	const char *key = lv_buttonmatrix_get_button_text(kb, id);
	ui_vibrate(15);

	if (strcmp(key, KB_SHIFT) == 0) {
		if (kb_get_mode(kb) == KB_LOWER) {
			kb_set_mode(kb, KB_UPPER);
			kb_shift_time = lv_tick_get();
		} else {
			kb_set_mode(kb, lv_tick_elaps(kb_shift_time) < KB_DOUBLE_MS ? KB_LOCK : KB_LOWER);
		}
	} else if (strcmp(key, KB_LOCKED) == 0 || strcmp(key, KB_TO_LETTERS) == 0) {
		kb_set_mode(kb, KB_LOWER);
		kb_auto_shift(kb);
	} else if (strcmp(key, KB_TO_SYMBOLS) == 0) {
		kb_set_mode(kb, KB_SYMBOLS);
	} else if (strcmp(key, KB_TO_MORE) == 0) {
		kb_set_mode(kb, KB_MORE);
	} else if (strcmp(key, LV_SYMBOL_KEYBOARD) == 0) {
		lv_obj_send_event(ta, LV_EVENT_CANCEL, NULL);
	} else if (strcmp(key, LV_SYMBOL_NEW_LINE) == 0 && lv_textarea_get_one_line(ta)) {
		lv_obj_send_event(ta, LV_EVENT_READY, NULL);
	} else {
		if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0)
			lv_textarea_delete_char(ta);
		else if (strcmp(key, LV_SYMBOL_NEW_LINE) == 0)
			lv_textarea_add_char(ta, '\n');
		else
			lv_textarea_add_text(ta, key);
		kb_auto_shift(kb);
	}
}

/* While a finger is on the keyboard, the next one types the key under it as it lands
 * (fast typing: fingers overlap), elsewhere a second finger is not the pointer (pinch) */
static void kb_rollover_cb(lv_event_t *e)
{
	for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev))
		if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER)
			lv_evdev_set_rollover(indev, lv_event_get_code(e) == LV_EVENT_PRESSED);
}

lv_obj_t *ui_keyboard_create(lv_obj_t *parent, lv_obj_t *ta)
{
	lv_obj_t *kb = lv_keyboard_create(parent);
	lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
	lv_obj_add_event_cb(kb, kb_key_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(kb, kb_rollover_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(kb, kb_rollover_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(kb, kb_rollover_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_add_event_cb(kb, kb_rollover_cb, LV_EVENT_DELETE, NULL); /* while pressed */
	lv_keyboard_set_textarea(kb, ta);
	lv_obj_remove_state(ta, LV_STATE_FOCUSED); /* set by lv_keyboard_set_textarea() */
	kb_set_mode(kb, KB_LOWER);

	lv_obj_set_size(kb, LV_PCT(100), 440);
	lv_obj_set_style_pad_all(kb, 10, 0);
	lv_obj_set_style_pad_gap(kb, 10, 0);
	lv_obj_set_style_radius(kb, 0, 0);
	lv_obj_set_style_border_width(kb, 0, 0);
	lv_obj_set_style_shadow_width(kb, 0, 0);
	lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_BAR), 0);

	lv_obj_set_style_text_font(kb, &ui_font_32, LV_PART_ITEMS);
	lv_obj_set_style_text_color(kb, lv_color_hex(UI_COLOR_TEXT), LV_PART_ITEMS);
	lv_obj_set_style_radius(kb, 12, LV_PART_ITEMS);
	lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
	lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
	lv_obj_set_style_outline_width(kb, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
	lv_obj_set_style_color_filter_opa(kb, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
	lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_CARD), LV_PART_ITEMS);
	lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_PRESSED), LV_PART_ITEMS | LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_BUTTON), LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_BUTTON_PRESSED),
	                          LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
	lv_obj_set_style_text_color(kb, lv_color_hex(UI_COLOR_TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);

	lv_obj_set_hidden(kb, true);
	return kb;
}

void ui_keyboard_show(lv_obj_t *kb, bool show)
{
	lv_obj_t *ta = lv_keyboard_get_textarea(kb);
	lv_obj_set_hidden(kb, !show);
	lv_obj_set_state(ta, LV_STATE_FOCUSED, show);
	if (show) {
		kb_set_mode(kb, KB_LOWER);
		kb_auto_shift(kb);
	}
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

void ui_status_update(void)
{
	char buf[64];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(buf, sizeof(buf), "%H:%M", &tm);
	ui_set_text(status_time, buf);

	int capacity = -1;
	char status[32] = "";
	FILE *f = fopen(BATTERY "/capacity", "r");
	if (f) {
		if (fscanf(f, "%d", &capacity) != 1)
			capacity = -1;
		fclose(f);
	}
	f = fopen(BATTERY "/status", "r");
	if (f) {
		if (!fgets(status, sizeof(status), f))
			status[0] = '\0';
		fclose(f);
	}
	bool charging = strncmp(status, "Charging", 8) == 0;

	if (capacity < 0) {
		ui_set_text(status_battery, LV_SYMBOL_BATTERY_EMPTY " ?");
		return;
	}
	const char *icon = capacity >= 90 ? LV_SYMBOL_BATTERY_FULL :
	                   capacity >= 65 ? LV_SYMBOL_BATTERY_3 :
	                   capacity >= 40 ? LV_SYMBOL_BATTERY_2 :
	                   capacity >= 15 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
	snprintf(buf, sizeof(buf), "%s%d%% %s", charging ? LV_SYMBOL_CHARGE " " : "", capacity, icon);
	ui_set_text(status_battery, buf);
	lv_obj_set_style_text_color(status_battery, capacity < 15 ?
	                            lv_palette_main(LV_PALETTE_RED) : lv_color_hex(UI_COLOR_TEXT), 0);
}

void ui_init(lv_display_t *disp)
{
	screen_w = lv_display_get_horizontal_resolution(disp);
	screen_h = lv_display_get_vertical_resolution(disp);
	theme_apply(ui_setting_get_int("light", 0) != 0);

	lv_obj_t *bar = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, LV_PCT(100), UI_STATUS_H);
	lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_SHADE), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);
	lv_obj_set_style_pad_hor(bar, 20, 0);
	lv_obj_set_style_text_color(bar, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_text_font(bar, &ui_font_24, 0);

	/* The modem is not driven yet */
	lv_obj_t *network = lv_label_create(bar);
	lv_label_set_text_static(network, "No SIM");
	lv_obj_align(network, LV_ALIGN_LEFT_MID, 0, 0);

	status_time = lv_label_create(bar);
	ui_set_text(status_time, "");
	lv_obj_align(status_time, LV_ALIGN_CENTER, 0, 0);

	status_battery = lv_label_create(bar);
	ui_set_text(status_battery, "");
	lv_obj_align(status_battery, LV_ALIGN_RIGHT_MID, 0, 0);

	status_bar = bar; /* recolored by theme_apply() from now on */
}
