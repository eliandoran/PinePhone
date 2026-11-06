/*
 * Doom: the app's first screen (plugin.h), a grid of the games: the IWADs in WADS
 * (games, not mods: PWADs need one to go with) and the shareware Doom that comes
 * with the app, each with its title screen. The one Doom has paused says so.
 */

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "plugin.h"

#define WADS "/var/lib/doom/wads"
#define BUNDLED "doom1.wad" /* shareware Doom, next to the plugin */

struct wad {
	char *path, *name;
	unsigned tex;            /* its title screen, 0: none */
	lv_obj_t *tile, *pic;
	lv_obj_t *state;         /* "Paused" when it is Doom's */
};

static struct {
	struct wad *wads;
	unsigned count;
} list;

static void wad_add(const char *path, const char *file)
{
	struct wad *w = &list.wads[list.count++];
	const char *name = wad_known_name(file);
	w->path = strdup(path);
	w->name = strdup(name ? name : file);
	char *dot = name ? NULL : strrchr(w->name, '.');
	if (dot && dot != w->name)
		*dot = '\0';
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
		if (e->d_name[0] == '.' || !dot || strcasecmp(dot, ".wad") != 0 || !wad_is_game(path, e->d_name))
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
			lv_obj_set_hidden(list.wads[i].state, !engine.running || strcmp(engine.iwad, list.wads[i].path) != 0);
}

void list_game_left(void)
{
	list_update();
}

/* The tiles: 2 to a row upright, 4 sideways; their pictures 320:200 */
static int32_t tile_w(void)
{
	lv_display_t *disp = lv_display_get_default();
	bool sideways = lv_display_get_horizontal_resolution(disp) > lv_display_get_vertical_resolution(disp);
	return ui_tile_width(sideways ? 4 : 2);
}

static void tiles_fit(void)
{
	int32_t w = tile_w();
	for (unsigned i = 0; i < list.count; i++)
		if (list.wads[i].tile)
			ui_tile_set_size(list.wads[i].tile, w, w * WAD_PIC_H / WAD_PIC_W);
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

/* A game's tile: its title screen (or a black one), then its name and whether it
 * is paused */
static void tile_create(lv_obj_t *grid, unsigned i)
{
	struct wad *w = &list.wads[i];
	int32_t width = tile_w();
	lv_obj_t *text;
	w->tile = ui_tile_create(grid, width, width * WAD_PIC_H / WAD_PIC_W, wad_cb, (void *)(uintptr_t)i,
	                         &w->pic, &text);
	uint8_t *px = wad_title(w->path);
	w->tex = ui_tile_texture(px, WAD_PIC_W, WAD_PIC_H, width, 0x000000);
	free(px);
	lv_3dtexture_set_src(w->pic, w->tex);

	lv_obj_t *line = ui_box(text);
	lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(line, 8, 0);
	lv_obj_t *name = ui_one_line_label(line, w->name, &ui_font_20);
	lv_obj_set_flex_grow(name, 1);
	w->state = ui_dim_label(line, _("Paused"), &ui_font_16);
}

static void wad_cb(lv_event_t *e)
{
	struct wad *w = &list.wads[(uintptr_t)lv_event_get_user_data(e)];
	game_open(w->path, w->name);
}

void list_open(void)
{
	char bundled[PATH_MAX + 16];
	snprintf(bundled, sizeof(bundled), "%s/" BUNDLED, plugin_dir());
	wads_scan(access(bundled, R_OK) == 0 ? bundled : NULL);

	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Doom", NULL, &body); /* NON-NLS: app.json's name */
	lv_obj_add_event_cb(scr, list_delete_cb, LV_EVENT_DELETE, NULL);
	ui_screen_turns(scr, false);

	lv_obj_t *grid = ui_tile_grid_create(body);

	for (unsigned i = 0; i < list.count; i++)
		tile_create(grid, i);
	if (!list.count)
		ui_empty_label(grid, _("No games"));
	char copy[160];
	snprintf(copy, sizeof(copy), _("Copy IWADs (doom2.wad, freedoom1.wad...) to %s"), WADS);
	lv_obj_t *hint = ui_dim_label(grid, copy, &ui_font_20);
	lv_obj_set_width(hint, LV_PCT(100));
	lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
	lv_display_add_event_cb(lv_display_get_default(), list_resolution_cb, LV_EVENT_RESOLUTION_CHANGED, NULL);
	list_update();
	ui_push(scr);
}
