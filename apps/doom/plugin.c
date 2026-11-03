/*
 * Doom, a plugin app (see external.c in phone-ui): the screen around the engine
 * (lib/doomengine.so: doomgeneric and its platform layer, engine.c), which it
 * loads when the app first opens and runs in a thread. What they share: doom.h.
 *
 * Upright, the game twice its size below the header with the controls under it;
 * turned sideways (phone-ui turns the display, ui_screen_turns(): full screen),
 * three times its size in the middle with the controls around it, as Doom ports
 * have them. The engine finds what the fingers touch in the same layouts (it
 * reads the touchscreen: LVGL only sees one finger), and says which buttons are
 * held for them to be drawn so.
 *
 * The app opens on a list of the games: the IWADs in /var/lib/doom/wads (`just
 * wads`) and the shareware Doom it comes with. Leaving a game pauses Doom (opening
 * it again goes on, the list says which one is paused); picking another ends it.
 * Quitting from Doom's menu ends the thread too. Ended, the library is unloaded
 * (doomgeneric cannot start over) and loaded again for the next game. Settings and
 * saves go in /var/lib/app-data/doom/<IWAD's name>/.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <GLES2/gl2.h>

#include "abi.h"
#include "doom.h"
#include "ui.h"

#define DATA "/var/lib/app-data/doom"
#define ENGINE "doomengine.so"
#define WADS "/var/lib/doom/wads"
#define BUNDLED "doom1.wad"      /* shareware Doom, next to the plugin */

#define COLOR_BUTTON 0x2a2a2a
#define COLOR_PRESSED 0x6a1a12
#define COLOR_TEXT 0xd0d0d0
#define COLOR_HINT 0x606060
#define COLOR_PAD 0x1a1a1a

static struct {
	/* The engine */
	void *lib;
	doom_run_fn *run;
	pthread_t thread;
	bool running;
	char iwad[PATH_MAX];     /* playing (paused or not), "" if none */
	char configdir[PATH_MAX];
	char title[128];        /* of the game's screen */
	struct doom_host host;

	/* The screen (LVGL's thread), laid out again when the display turns */
	lv_obj_t *scr, *header, *body, *view, *controls;
	lv_obj_t *buttons[10];
	lv_obj_t *knob[2];       /* the pads' (move, turn); NULL: no pad */
	int knob_x[2], knob_y[2]; /* where they rest */
	int shown_knob[2][2];
	lv_timer_t *timer;
	GLuint tex;
	unsigned shown_seq, shown_pressed;
	uint32_t boosted;
} dm = { .host = { .lock = PTHREAD_MUTEX_INITIALIZER } };

/* The engine, from lib/ next to this plugin; its directory into `dir` */
static bool engine_load(char *dir, size_t size)
{
	Dl_info info;
	if (!dladdr((void *)engine_load, &info) || !info.dli_fname)
		return false;
	snprintf(dir, size, "%s", info.dli_fname);
	char *slash = strrchr(dir, '/');
	if (slash)
		*slash = '\0';
	if (dm.lib)
		return true;
	char path[PATH_MAX + 32];
	snprintf(path, sizeof(path), "%s/lib/" ENGINE, dir);
	dm.lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!dm.lib) {
		printf("doom: %s\n", dlerror());
		return false;
	}
	dm.run = (doom_run_fn *)dlsym(dm.lib, "doom_run");
	if (!dm.run) {
		printf("doom: %s has no doom_run\n", ENGINE);
		dlclose(dm.lib);
		dm.lib = NULL;
		return false;
	}
	return true;
}

static void *doom_main(void *arg)
{
	(void)arg;
	dm.run(&dm.host);
	return NULL;
}

/* Doom with the IWAD at `iwad`. Each has its own settings and saves (Doom keeps
 * them all in one folder): DATA/<its name>/, Doom joining its file names to
 * configdir (DATA/<name>/.default.cfg, DATA/<name>/./.savegame/). */
static bool engine_start(const char *iwad)
{
	char dir[PATH_MAX];
	if (!engine_load(dir, sizeof(dir)))
		return false;
	snprintf(dm.iwad, sizeof(dm.iwad), "%s", iwad);
	const char *base = strrchr(iwad, '/');
	snprintf(dm.configdir, sizeof(dm.configdir), DATA "/%s", base ? base + 1 : iwad);
	char *dot = strrchr(dm.configdir, '.');
	if (dot && dot > dm.configdir + sizeof(DATA))
		*dot = '\0';
	mkdir("/var/lib/app-data", 0755);
	mkdir(DATA, 0755);
	mkdir(dm.configdir, 0755);
	strcat(dm.configdir, "/.");
	dm.host.iwad = dm.iwad;
	dm.host.configdir = dm.configdir;
	dm.host.paused = dm.host.exited = dm.host.quit = false;
	dm.host.pressed = 0;
	if (pthread_create(&dm.thread, NULL, doom_main, NULL) != 0)
		return false;
	dm.running = true;
	printf("doom: playing %s\n", iwad);
	return true;
}

/* After Doom quit: the library cannot run it again */
static void engine_end(void)
{
	pthread_join(dm.thread, NULL);
	dm.running = false;
	dm.iwad[0] = '\0';
	dlclose(dm.lib);
	dm.lib = NULL;
	dm.run = NULL;
}

/* Doom paused with another IWAD: ended, for another to start */
static void engine_stop(void)
{
	if (!dm.running)
		return;
	__atomic_store_n(&dm.host.quit, true, __ATOMIC_RELEASE);
	engine_end();
}

/* The screen */

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
	__atomic_store_n(&dm.host.orient, o, __ATOMIC_RELAXED);
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
	if (dm.running && __atomic_load_n(&dm.host.exited, __ATOMIC_ACQUIRE)) {
		engine_end();
		if (ui_current() == dm.scr)
			ui_pop();
		return;
	}

	unsigned pressed = __atomic_load_n(&dm.host.pressed, __ATOMIC_RELAXED);
	if (pressed != dm.shown_pressed) {
		for (unsigned i = 0; i < sizeof(dm.buttons) / sizeof(dm.buttons[0]); i++)
			if (dm.buttons[i])
				button_set_pressed(dm.buttons[i], pressed >> i & 1);
		dm.shown_pressed = pressed;
	}
	for (int p = 0; p < 2; p++) {
		int x = __atomic_load_n(&dm.host.knob[p][0], __ATOMIC_RELAXED);
		int y = __atomic_load_n(&dm.host.knob[p][1], __ATOMIC_RELAXED);
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

	if (__atomic_load_n(&dm.host.frame_seq, __ATOMIC_RELAXED) == dm.shown_seq)
		return;
	pthread_mutex_lock(&dm.host.lock);
	bool created = !dm.tex;
	if (created) {
		glGenTextures(1, &dm.tex);
		glBindTexture(GL_TEXTURE_2D, dm.tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, DOOM_W, DOOM_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, dm.host.frame);
	} else {
		glBindTexture(GL_TEXTURE_2D, dm.tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DOOM_W, DOOM_H, GL_RGBA, GL_UNSIGNED_BYTE, dm.host.frame);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	dm.shown_seq = dm.host.frame_seq;
	pthread_mutex_unlock(&dm.host.lock);

	if (created) {
		lv_3dtexture_set_src(dm.view, dm.tex);
		lv_obj_set_hidden(dm.view, false);
	}
	lv_obj_invalidate(dm.view);
}

static void list_game_left(void);

/* Left (or ended): Doom waits for the app to open again */
static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (dm.running)
		__atomic_store_n(&dm.host.paused, true, __ATOMIC_RELEASE);
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
static void game_open(const char *iwad, const char *title)
{
	if (dm.running && strcmp(dm.iwad, iwad) != 0)
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
	dm.shown_seq = dm.host.frame_seq - 1; /* Doom's last frame shown again if paused */

	/* Turns with the phone, sideways without the status bar */
	ui_screen_turns(dm.scr, true);
	layout();
	lv_display_add_event_cb(lv_display_get_default(), resolution_cb, LV_EVENT_RESOLUTION_CHANGED, NULL);

	bool ok = dm.running || engine_start(iwad);
	if (!ok) {
		lv_obj_t *label = ui_empty_label(dm.body, "Cannot start Doom");
		lv_obj_set_pos(label, 0, 0);
	} else {
		__atomic_store_n(&dm.host.paused, false, __ATOMIC_RELEASE);
		dm.timer = lv_timer_create(frame_cb, 4, NULL);
		ui_cpu_boost(true);
		dm.boosted = lv_tick_get();
	}
	ui_push(dm.scr);
}

/* The list: the IWADs in WADS (games, not mods: PWADs need one to go with), and
 * the shareware Doom that comes with the app */

/* Names of the games doomgeneric knows; `pwad`: its file says PWAD but it is the
 * whole game (Chex Quest's), Doom takes it by its name (d_iwad.c) */
static const struct {
	const char *file, *name;
	bool pwad;
} known[] = {
	{ "doom1.wad", "Doom (shareware)", false },
	{ "doom.wad", "Doom", false },
	{ "doomu.wad", "The Ultimate Doom", false },
	{ "doom2.wad", "Doom II", false },
	{ "doom2f.wad", "Doom II (French)", false },
	{ "plutonia.wad", "Final Doom: Plutonia", false },
	{ "tnt.wad", "Final Doom: TNT", false },
	{ "freedoom1.wad", "Freedoom: Phase 1", false },
	{ "freedoom2.wad", "Freedoom: Phase 2", false },
	{ "freedm.wad", "FreeDM", false },
	{ "chex.wad", "Chex Quest", true },
	{ "hacx.wad", "Hacx", true },
};

/* IWADs of the other games on Doom's engine, which doomgeneric does not run */
static const char *const other_games[] = { "heretic", "hexen", "hexdd", "strife", "voices" };

struct wad {
	char *path, *name;
	unsigned tex;            /* its title screen, 0: none */
	lv_obj_t *tile, *pic;
	lv_obj_t *state;         /* "Paused" when it is Doom's */
};

/* Title screens: TITLEPIC, in the WAD's palette (PLAYPAL's first), as BGRA */

#define PIC_W 320
#define PIC_H 200

static void *lump_read(FILE *f, int32_t pos, int32_t size)
{
	void *data = size > 0 && size < (1 << 24) ? malloc(size) : NULL;
	if (data && (fseek(f, pos, SEEK_SET) != 0 || fread(data, 1, size, f) != (size_t)size)) {
		free(data);
		data = NULL;
	}
	return data;
}

static int16_t le16(const uint8_t *p)
{
	return (int16_t)(p[0] | p[1] << 8);
}

static int32_t le32(const uint8_t *p)
{
	return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

/* Malloc'd PIC_W x PIC_H BGRA, NULL if the WAD has none. TITLEPIC is a patch
 * (columns of runs of palette indices), or a raw 320 x 200 block in some old WADs. */
static uint8_t *wad_title(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint8_t head[12], *dir = NULL, *pal = NULL, *pic = NULL, *out = NULL;
	int32_t pic_size = 0;
	if (fread(head, 1, 12, f) != 12)
		goto done;
	int32_t count = le32(head + 4), table = le32(head + 8);
	if (count <= 0 || count > 65536 || !(dir = lump_read(f, table, count * 16)))
		goto done;
	for (int32_t i = 0; i < count; i++) {
		const uint8_t *e = dir + i * 16;
		if (!pal && strncmp((const char *)e + 8, "PLAYPAL", 8) == 0 && le32(e + 4) >= 768)
			pal = lump_read(f, le32(e), 768);
		else if (!pic && strncmp((const char *)e + 8, "TITLEPIC", 8) == 0)
			pic = lump_read(f, le32(e), pic_size = le32(e + 4));
	}
	if (!pal || !pic)
		goto done;

	out = calloc(PIC_W * PIC_H, 4);
	for (int i = 0; i < PIC_W * PIC_H; i++)
		out[i * 4 + 3] = 0xff;
	if (pic_size == PIC_W * PIC_H) {
		for (int i = 0; i < PIC_W * PIC_H; i++)
			memcpy(out + i * 4, (uint8_t[]){ pal[pic[i] * 3 + 2], pal[pic[i] * 3 + 1], pal[pic[i] * 3] }, 3);
		goto done;
	}
	int w = pic_size >= 8 ? le16(pic) : 0, h = pic_size >= 8 ? le16(pic + 2) : 0;
	if (w <= 0 || h <= 0 || 8 + 4 * w > pic_size)
		goto done;
	for (int x = 0; x < w && x < PIC_W; x++) {
		int32_t p = le32(pic + 8 + 4 * x);
		/* Posts: top, length, a padding byte, the pixels, a padding byte; 0xff ends */
		while (p >= 0 && p + 3 <= pic_size && pic[p] != 0xff) {
			int top = pic[p], len = pic[p + 1];
			const uint8_t *src = pic + p + 3;
			if (p + 3 + len > pic_size)
				break;
			for (int y = 0; y < len; y++) {
				int row = top + y;
				if (row >= PIC_H)
					break;
				uint8_t *d = out + (row * PIC_W + x) * 4;
				d[0] = pal[src[y] * 3 + 2];
				d[1] = pal[src[y] * 3 + 1];
				d[2] = pal[src[y] * 3];
			}
			p += len + 4;
		}
	}
done:
	free(dir);
	free(pal);
	free(pic);
	fclose(f);
	return out;
}

static struct {
	struct wad *wads;
	unsigned count;
} list;

/* A game Doom can run: an IWAD not of another game, or one known by its name */
static bool is_game(const char *path, const char *file)
{
	for (unsigned i = 0; i < sizeof(other_games) / sizeof(other_games[0]); i++)
		if (strncasecmp(file, other_games[i], strlen(other_games[i])) == 0)
			return false;
	char magic[4];
	FILE *f = fopen(path, "rb");
	bool read = f && fread(magic, 1, 4, f) == 4;
	if (f)
		fclose(f);
	if (read && memcmp(magic, "IWAD", 4) == 0)
		return true;
	for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++)
		if (known[i].pwad && strcasecmp(file, known[i].file) == 0)
			return read && memcmp(magic, "PWAD", 4) == 0;
	return false;
}

static void wad_add(const char *path, const char *file)
{
	struct wad *w = &list.wads[list.count++];
	w->path = strdup(path);
	w->name = NULL;
	for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]) && !w->name; i++)
		if (strcasecmp(file, known[i].file) == 0)
			w->name = strdup(known[i].name);
	if (!w->name) {
		w->name = strdup(file);
		char *dot = strrchr(w->name, '.');
		if (dot && dot != w->name)
			*dot = '\0';
	}
	w->state = w->tile = w->pic = NULL;
	w->tex = 0;
}

static int wad_compare(const void *a, const void *b)
{
	return strcasecmp(((const struct wad *)a)->name, ((const struct wad *)b)->name);
}

static void wads_scan(const char *bundled)
{
	DIR *d = opendir(WADS);
	struct dirent *e;
	unsigned cap = 8;
	list.wads = calloc(cap, sizeof(*list.wads));
	list.count = 0;
	if (bundled)
		wad_add(bundled, BUNDLED);
	while (d && (e = readdir(d))) {
		const char *dot = strrchr(e->d_name, '.');
		char path[PATH_MAX];
		snprintf(path, sizeof(path), WADS "/%s", e->d_name);
		if (e->d_name[0] == '.' || !dot || strcasecmp(dot, ".wad") != 0 || !is_game(path, e->d_name))
			continue;
		if (list.count == cap) {
			cap *= 2;
			list.wads = realloc(list.wads, cap * sizeof(*list.wads));
		}
		wad_add(path, e->d_name);
	}
	if (d)
		closedir(d);
	qsort(list.wads, list.count, sizeof(*list.wads), wad_compare);
}

/* Which one is Doom's (paused), after a game or when the list is made */
static void list_update(void)
{
	for (unsigned i = 0; i < list.count; i++)
		if (list.wads[i].state)
			lv_obj_set_hidden(list.wads[i].state, !dm.running || strcmp(dm.iwad, list.wads[i].path) != 0);
}

static void list_game_left(void)
{
	list_update();
}

#define GRID_PAD 16 /* around the grid and between its tiles */
#define TILE_PAD 10 /* inside a tile, around its picture */

/* The tiles: 2 to a row upright, 4 sideways; their pictures 320:200 */
static void tiles_fit(void)
{
	lv_display_t *disp = lv_display_get_default();
	int32_t w = lv_display_get_horizontal_resolution(disp);
	int32_t cols = w > lv_display_get_vertical_resolution(disp) ? 4 : 2;
	int32_t tile_w = (w - (cols + 1) * GRID_PAD) / cols;
	int32_t pic_w = tile_w - 2 * TILE_PAD;
	for (unsigned i = 0; i < list.count; i++) {
		if (!list.wads[i].tile)
			continue;
		lv_obj_set_width(list.wads[i].tile, tile_w);
		lv_obj_set_size(list.wads[i].pic, pic_w, pic_w * PIC_H / PIC_W);
	}
}

static void list_resolution_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	tiles_fit();
}

static void list_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_display_remove_event_cb_with_user_data(lv_display_get_default(), list_resolution_cb, NULL);
	for (unsigned i = 0; i < list.count; i++) {
		free(list.wads[i].path);
		free(list.wads[i].name);
		ui_texture_delete(list.wads[i].tex);
	}
	free(list.wads);
	list.wads = NULL;
	list.count = 0;
}

static void wad_cb(lv_event_t *e);

/* A game's tile: its title screen (or its name on a gray one), then its name and
 * whether it is paused */
static void tile_create(lv_obj_t *grid, unsigned i)
{
	struct wad *w = &list.wads[i];
	lv_obj_t *tile = w->tile = ui_box(grid);
	lv_obj_set_height(tile, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(tile, TILE_PAD, 0);
	lv_obj_set_style_pad_row(tile, 8, 0);
	lv_obj_set_style_radius(tile, 20, 0);
	lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_clickable(tile, true);
	lv_obj_add_event_cb(tile, wad_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

	uint8_t *px = wad_title(w->path);
	w->tex = px ? ui_texture_create(px, PIC_W, PIC_H) : 0;
	free(px);
	if (w->tex) {
		w->pic = lv_3dtexture_create(tile);
		lv_3dtexture_set_src(w->pic, w->tex);
	} else {
		w->pic = ui_box(tile);
		lv_obj_set_style_bg_opa(w->pic, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(w->pic, lv_color_black(), 0);
	}
	lv_obj_set_clickable(w->pic, false);

	lv_obj_t *line = ui_box(tile);
	lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(line, 8, 0);
	lv_obj_t *name = ui_one_line_label(line, w->name, &ui_font_20);
	lv_obj_set_flex_grow(name, 1);
	w->state = ui_dim_label(line, "Paused", &ui_font_16);
}

static void wad_cb(lv_event_t *e)
{
	struct wad *w = &list.wads[(uintptr_t)lv_event_get_user_data(e)];
	game_open(w->path, w->name);
}

static void list_open(void)
{
	/* The bundled one, next to this plugin */
	char bundled[PATH_MAX + 16];
	Dl_info info;
	bool have_bundled = dladdr((void *)list_open, &info) && info.dli_fname;
	if (have_bundled) {
		snprintf(bundled, sizeof(bundled), "%s", info.dli_fname);
		char *slash = strrchr(bundled, '/');
		snprintf(slash ? slash + 1 : bundled, sizeof(bundled) - (slash ? slash + 1 - bundled : 0), BUNDLED);
		have_bundled = access(bundled, R_OK) == 0;
	}
	wads_scan(have_bundled ? bundled : NULL);

	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Doom", NULL, &body);
	lv_obj_add_event_cb(scr, list_delete_cb, LV_EVENT_DELETE, NULL);
	ui_screen_turns(scr, false);

	lv_obj_t *grid = ui_box(body);
	lv_obj_set_width(grid, LV_PCT(100));
	lv_obj_set_flex_grow(grid, 1);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_all(grid, GRID_PAD, 0);
	lv_obj_set_style_pad_bottom(grid, 32, 0);
	lv_obj_set_style_pad_row(grid, GRID_PAD, 0);
	lv_obj_set_style_pad_column(grid, GRID_PAD, 0);
	lv_obj_set_scrollable(grid, true);
	lv_obj_set_scroll_dir(grid, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);

	for (unsigned i = 0; i < list.count; i++)
		tile_create(grid, i);
	if (!list.count)
		ui_empty_label(grid, "No games");
	lv_obj_t *hint = ui_dim_label(grid, "Copy IWADs (doom2.wad, freedoom1.wad...) to " WADS, &ui_font_20);
	lv_obj_set_width(hint, LV_PCT(100));
	lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
	tiles_fit();
	lv_display_add_event_cb(lv_display_get_default(), list_resolution_cb, LV_EVENT_RESOLUTION_CHANGED, NULL);
	list_update();
	ui_push(scr);
}

/* What phone-ui looks for (see external.c) */

const char app_abi[] = UI_ABI;

void app_open(void)
{
	list_open();
}
