/*
 * Maps: OpenStreetMap, panned with a finger, zoomed with two (pinch), the buttons or
 * a double tap. Tiles (256 px PNG) come from tile.openstreetmap.org over HTTPS when the
 * phone has internet (through the host, `just share-internet`), at most two at a
 * time as its tile usage policy asks, and are kept in /var/lib/maps/tiles/Z/X/Y.png.
 * Downloads go through libcurl's multi interface, polled by a timer. The view is
 * saved in the settings.
 *
 * Each tile is a GL texture shown by an lv_3dtexture: a decoded image would go
 * through the draw unit's texture cache, which knows images by their address, and
 * a tile allocated where an evicted one was would show the old one. While a tile
 * is missing, a loaded tile of a lower zoom is shown scaled up in its place.
 *
 * While pinching, the tiles are drawn scaled (1/sqrt 2 to sqrt 2), the zoom level
 * changing halfway; lifting the fingers settles on the nearest level.
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ui.h"

#if LV_USE_3DTEXTURE

#include "src/indev/lv_indev_private.h" /* the pinch center and movement */
#include "src/indev/lv_indev_gesture_private.h"

#include <GLES2/gl2.h>
#include <curl/curl.h>
#include <png.h>

#define TILE 256
#define MIN_ZOOM 2
#define MAX_ZOOM 19
#define COLS 5        /* tile cells: enough to cover the map with a partial tile */
#define ROWS 9        /* on each side, at the smallest pinch scale */
#define TEXTURES 96   /* tiles kept on the GPU, 256 KiB each */
#define DOWNLOADS 2   /* at a time, as the tile usage policy asks */
#define LOADS_PER_TICK 2
#define FAILED 64     /* tiles not retried for RETRY_MS */
#define RETRY_MS 30000
#define FALLBACK_LEVELS 4
#define DOUBLE_TAP_MS 350
#define PINCH_START 1.05f /* finger distance ratio that starts a pinch (LVGL: 1.5) */
#define ZOOM_MS 250     /* zoom buttons and double tap */
#define ZOOM_STEPS 1024 /* animation values */
#define TILES "/var/lib/maps/tiles"
#define URL "https://tile.openstreetmap.org/%d/%d/%d.png"
#define USER_AGENT "phone-ui/1.0 (PinePhone feature phone firmware)"
#define COLOR_LAND 0xf2efe9 /* OpenStreetMap's background, where tiles are missing */

struct texture {
	int z, x, y;
	GLuint id; /* 0: free */
	uint32_t used; /* last update that showed it */
};

struct download {
	CURL *curl; /* NULL: free */
	int z, x, y;
	char *data;
	size_t size;
};

struct key {
	int z, x, y;
};

static struct {
	lv_obj_t *map, *status;
	lv_obj_t *cells[ROWS][COLS], *quads[ROWS][COLS];
	int32_t w, h;
	int z;
	double cx, cy; /* view center, in pixels of the world at zoom z */
	double scale;  /* drawing scale of the tiles, 1 except while pinching */
	struct {
		bool active;
		int z;
		double cx, cy;
		lv_point_t center; /* between the fingers when it started, in the map */
		float dx0, dy0;    /* their movement then, and now (LVGL counts it from */
		float dx, dy;      /* the second finger, before it saw a pinch) */
	} pinch;
	struct texture tex[TEXTURES];
	uint32_t update;
	struct key missing[ROWS * COLS]; /* visible tiles without a texture, nearest first */
	unsigned missing_count;
	struct download dl[DOWNLOADS];
	CURLM *multi;
	struct { struct key key; uint32_t time; } failed[FAILED];
	unsigned failed_next;
	lv_timer_t *timer;
	int32_t moved;
	uint32_t tap_time;
	lv_point_t tap_point;
	bool zooming; /* buttons or double tap, animated */
	int zoom_dz;
} map;

/* Textures */

static struct texture *texture_find(int z, int x, int y)
{
	for (int i = 0; i < TEXTURES; i++)
		if (map.tex[i].id && map.tex[i].z == z && map.tex[i].x == x && map.tex[i].y == y)
			return &map.tex[i];
	return NULL;
}

/* A free slot, or the least recently shown one that the last update did not show */
static struct texture *texture_slot(void)
{
	struct texture *best = NULL;
	for (int i = 0; i < TEXTURES; i++) {
		struct texture *t = &map.tex[i];
		if (!t->id)
			return t;
		if (t->used != map.update && (!best || t->used < best->used))
			best = t;
	}
	if (best) {
		ui_texture_delete(best->id);
		best->id = 0;
	}
	return best;
}

/* Decodes a PNG read from a file (data NULL) or memory into a texture */
static bool texture_load(int z, int x, int y, const char *path, const char *data, size_t size)
{
	png_image img = { .version = PNG_IMAGE_VERSION };
	if (!(data ? png_image_begin_read_from_memory(&img, data, size) :
	             png_image_begin_read_from_file(&img, path)))
		return false;
	if (img.width != TILE || img.height != TILE) {
		png_image_free(&img);
		return false;
	}
	/* LVGL's layout: its texture shader swaps red and blue back (c.bgra) */
	img.format = PNG_FORMAT_BGRA;
	void *pixels = malloc(PNG_IMAGE_SIZE(img));
	if (!png_image_finish_read(&img, NULL, pixels, 0, NULL)) {
		free(pixels);
		return false;
	}

	struct texture *t = texture_slot();
	if (t) {
		t->id = ui_texture_create(pixels, TILE, TILE);
		t->z = z;
		t->x = x;
		t->y = y;
		t->used = 0;
	}
	free(pixels);
	return true;
}

/* View */

static void view_clamp(void)
{
	double world = (double)TILE * (1 << map.z), half_h = map.h / (2 * map.scale);
	map.cx = fmod(map.cx, world);
	if (map.cx < 0)
		map.cx += world;
	if (world <= 2 * half_h)
		map.cy = world / 2;
	else if (map.cy < half_h)
		map.cy = half_h;
	else if (map.cy > world - half_h)
		map.cy = world - half_h;
}

static void missing_add(int z, int x, int y, double dist)
{
	unsigned i = map.missing_count++;
	/* Nearest first: insertion sort, on distances kept alongside */
	static double dists[ROWS * COLS];
	while (i > 0 && dists[i - 1] > dist) {
		map.missing[i] = map.missing[i - 1];
		dists[i] = dists[i - 1];
		i--;
	}
	map.missing[i] = (struct key){ z, x, y };
	dists[i] = dist;
}

/* Shows tile (x, y) in a cell of w x h: its texture, or a lower zoom's scaled up */
static void cell_show(lv_obj_t *quad, int x, int y, int32_t w, int32_t h, double dist)
{
	struct texture *t = texture_find(map.z, x, y);
	int d = 0;
	if (!t) {
		missing_add(map.z, x, y, dist);
		for (d = 1; d <= FALLBACK_LEVELS && map.z - d >= 0 && !t; d++)
			t = texture_find(map.z - d, x >> d, y >> d);
		d--;
	}
	if (!t) {
		lv_obj_set_hidden(quad, true);
		return;
	}
	int mask = (1 << d) - 1;
	t->used = map.update;
	lv_obj_set_size(quad, w << d, h << d);
	lv_obj_set_pos(quad, -(x & mask) * w, -(y & mask) * h);
	lv_3dtexture_set_src(quad, t->id);
	lv_obj_set_hidden(quad, false);
	lv_obj_invalidate(quad);
}

static void view_update(void)
{
	int n = 1 << map.z;
	double s = map.scale;
	double x0 = map.cx - map.w / (2 * s), y0 = map.cy - map.h / (2 * s);
	int tx0 = (int)floor(x0 / TILE), ty0 = (int)floor(y0 / TILE);

	map.update++;
	map.missing_count = 0;
	for (int r = 0; r < ROWS; r++) {
		for (int c = 0; c < COLS; c++) {
			lv_obj_t *cell = map.cells[r][c];
			int tx = tx0 + c, ty = ty0 + r;
			/* Edges rounded on their own: neighbors meet without gaps */
			int32_t px = (int32_t)lround((tx * TILE - x0) * s);
			int32_t py = (int32_t)lround((ty * TILE - y0) * s);
			int32_t w = (int32_t)lround(((tx + 1) * TILE - x0) * s) - px;
			int32_t h = (int32_t)lround(((ty + 1) * TILE - y0) * s) - py;
			if (ty < 0 || ty >= n || px >= map.w || py >= map.h) {
				lv_obj_set_hidden(cell, true);
				continue;
			}
			lv_obj_set_pos(cell, px, py);
			lv_obj_set_size(cell, w, h);
			lv_obj_set_hidden(cell, false);
			double dx = px + w / 2 - map.w / 2, dy = py + h / 2 - map.h / 2;
			cell_show(map.quads[r][c], ((tx % n) + n) % n, ty, w, h, dx * dx + dy * dy);
		}
	}
}

/*
 * Pinch: the fingers' distance ratio s (from LVGL, relative to where they
 * started) zooms by log2(s) levels around the point between them, which follows
 * the fingers as they move: the map pans and zooms together. The level is the
 * nearest one, drawn at the remaining scale; snap draws it at scale 1.
 */
static void pinch_apply(float s, bool snap)
{
	double zoom = map.pinch.z + log2(s);
	if (zoom < MIN_ZOOM)
		zoom = MIN_ZOOM;
	if (zoom > MAX_ZOOM)
		zoom = MAX_ZOOM;
	int z = (int)lround(zoom);
	double f = exp2(z - map.pinch.z);
	/* The point under the fingers at the start (a), where they are now (b) */
	double ax = map.pinch.center.x - map.w / 2.0, ay = map.pinch.center.y - map.h / 2.0;
	double bx = ax + map.pinch.dx - map.pinch.dx0, by = ay + map.pinch.dy - map.pinch.dy0;
	map.z = z;
	map.scale = snap ? 1 : exp2(zoom - z);
	map.cx = (map.pinch.cx + ax) * f - bx / map.scale;
	map.cy = (map.pinch.cy + ay) * f - by / map.scale;
	view_clamp();
	view_update();
}

static void pinch_cb(lv_event_t *e)
{
	if (lv_event_get_gesture_type(e) != LV_INDEV_GESTURE_PINCH)
		return;
	lv_indev_t *indev = lv_event_get_param(e);
	lv_indev_gesture_recognizer_t *r = &indev->recognizers[LV_INDEV_GESTURE_PINCH];
	switch (lv_event_get_gesture_state(e, LV_INDEV_GESTURE_PINCH)) {
	case LV_INDEV_GESTURE_STATE_RECOGNIZED:
		if (!map.pinch.active) {
			lv_area_t a;
			lv_obj_get_coords(map.map, &a);
			lv_indev_get_gesture_center_point(r, &map.pinch.center);
			map.pinch.center.x -= a.x1;
			map.pinch.center.y -= a.y1;
			map.pinch.z = map.z;
			map.pinch.cx = map.cx;
			map.pinch.cy = map.cy;
			map.pinch.dx0 = r->info->delta_x;
			map.pinch.dy0 = r->info->delta_y;
			map.pinch.active = true;
			map.moved = INT32_MAX / 2; /* not a tap when the fingers lift */
		}
		map.pinch.dx = r->info->delta_x;
		map.pinch.dy = r->info->delta_y;
		pinch_apply(lv_event_get_pinch_scale(e), false);
		break;
	case LV_INDEV_GESTURE_STATE_ENDED:
	case LV_INDEV_GESTURE_STATE_CANCELED:
		if (map.pinch.active) {
			/* Where the fingers last were: LVGL may have reset its movement */
			pinch_apply(lv_event_get_pinch_scale(e), true);
			map.pinch.active = false;
		}
		break;
	default:
		break;
	}
}

/* Zoom buttons and double tap: an animated pinch of dz levels around a point */

static void zoom_anim_cb(void *var, int32_t v)
{
	LV_UNUSED(var);
	pinch_apply(exp2f(map.zoom_dz * v / (float)ZOOM_STEPS), v == ZOOM_STEPS);
}

static void zoom_anim_done(lv_anim_t *a)
{
	LV_UNUSED(a);
	map.zooming = false;
}

/* Jumps to the end of a zoom in progress */
static void zoom_finish(void)
{
	if (!map.zooming)
		return;
	lv_anim_delete(&map, zoom_anim_cb);
	zoom_anim_cb(&map, ZOOM_STEPS);
	map.zooming = false;
}

/* Zooms by dz levels, keeping the point (px, py) of the map where it is */
static void view_zoom(int dz, int32_t px, int32_t py)
{
	zoom_finish();
	if (map.z + dz < MIN_ZOOM || map.z + dz > MAX_ZOOM)
		return;
	map.pinch.z = map.z;
	map.pinch.cx = map.cx;
	map.pinch.cy = map.cy;
	map.pinch.center = (lv_point_t){ px, py };
	map.pinch.dx0 = map.pinch.dy0 = map.pinch.dx = map.pinch.dy = 0;
	map.zoom_dz = dz;
	map.zooming = true;

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &map);
	lv_anim_set_exec_cb(&a, zoom_anim_cb);
	lv_anim_set_values(&a, 0, ZOOM_STEPS);
	lv_anim_set_duration(&a, ZOOM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, zoom_anim_done);
	lv_anim_start(&a);
}

/* Downloads */

static bool failed_recently(const struct key *k)
{
	for (int i = 0; i < FAILED; i++)
		if (map.failed[i].time && !memcmp(&map.failed[i].key, k, sizeof(*k)) &&
		    lv_tick_elaps(map.failed[i].time) < RETRY_MS)
			return true;
	return false;
}

static bool downloading(const struct key *k)
{
	for (int i = 0; i < DOWNLOADS; i++)
		if (map.dl[i].curl && map.dl[i].z == k->z && map.dl[i].x == k->x && map.dl[i].y == k->y)
			return true;
	return false;
}

static size_t write_cb(char *data, size_t size, size_t n, void *user)
{
	struct download *d = user;
	char *p = realloc(d->data, d->size + size * n);
	if (!p)
		return 0;
	memcpy(p + d->size, data, size * n);
	d->data = p;
	d->size += size * n;
	return size * n;
}

static void download_start(struct download *d, const struct key *k)
{
	char url[96];
	snprintf(url, sizeof(url), URL, k->z, k->x, k->y);
	d->curl = curl_easy_init();
	if (!d->curl)
		return;
	d->z = k->z;
	d->x = k->x;
	d->y = k->y;
	curl_easy_setopt(d->curl, CURLOPT_URL, url);
	curl_easy_setopt(d->curl, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(d->curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(d->curl, CURLOPT_WRITEDATA, d);
	curl_easy_setopt(d->curl, CURLOPT_PRIVATE, d);
	curl_easy_setopt(d->curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(d->curl, CURLOPT_TIMEOUT, 30L);
	curl_multi_add_handle(map.multi, d->curl);
}

static void download_free(struct download *d)
{
	curl_multi_remove_handle(map.multi, d->curl);
	curl_easy_cleanup(d->curl);
	free(d->data);
	memset(d, 0, sizeof(*d));
}

static void mkdirs(char *path)
{
	for (char *p = strchr(path + 1, '/'); p; p = strchr(p + 1, '/')) {
		*p = '\0';
		if (mkdir(path, 0755) != 0 && errno != EEXIST)
			perror("maps: mkdir");
		*p = '/';
	}
}

/* A cache, so not synced: a tile cut short does not decode and is fetched again */
static void tile_save(const struct download *d)
{
	char path[64], tmp[72];
	snprintf(path, sizeof(path), TILES "/%d/%d/%d.png", d->z, d->x, d->y);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	mkdirs(path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	bool ok = fwrite(d->data, 1, d->size, f) == d->size;
	if (fclose(f) == 0 && ok)
		rename(tmp, path);
	else
		unlink(tmp);
}

static void download_done(struct download *d, CURLcode result)
{
	long code = 0;
	curl_easy_getinfo(d->curl, CURLINFO_RESPONSE_CODE, &code);
	bool ok = result == CURLE_OK && code == 200 &&
	          texture_load(d->z, d->x, d->y, NULL, d->data, d->size);
	if (ok) {
		tile_save(d);
	} else {
		if (result != CURLE_OK)
			printf("maps: %d/%d/%d: %s\n", d->z, d->x, d->y, curl_easy_strerror(result));
		else
			printf("maps: %d/%d/%d: HTTP %ld\n", d->z, d->x, d->y, code);
		map.failed[map.failed_next].key = (struct key){ d->z, d->x, d->y };
		map.failed[map.failed_next].time = lv_tick_get() | 1;
		map.failed_next = (map.failed_next + 1) % FAILED;
	}
	/* Connection problems, not a tile the server lacks */
	if (result == CURLE_OK)
		lv_obj_set_hidden(map.status, true);
	else if (result == CURLE_COULDNT_RESOLVE_HOST || result == CURLE_COULDNT_CONNECT ||
	         result == CURLE_OPERATION_TIMEDOUT)
		lv_obj_set_hidden(map.status, false);
	download_free(d);
}

/* Downloads, then the missing tiles: from the disk, else from the server */
static void tick_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	bool changed = false;

	int running, left;
	curl_multi_perform(map.multi, &running);
	CURLMsg *m;
	while ((m = curl_multi_info_read(map.multi, &left))) {
		if (m->msg != CURLMSG_DONE)
			continue;
		struct download *d;
		curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, (char **)&d);
		download_done(d, m->data.result);
		changed = true;
	}

	int loads = LOADS_PER_TICK;
	for (unsigned i = 0; i < map.missing_count; i++) {
		const struct key *k = &map.missing[i];
		if (texture_find(k->z, k->x, k->y) || downloading(k) || failed_recently(k))
			continue;
		char path[64];
		snprintf(path, sizeof(path), TILES "/%d/%d/%d.png", k->z, k->x, k->y);
		if (access(path, F_OK) == 0) {
			if (loads == 0)
				continue;
			loads--;
			if (texture_load(k->z, k->x, k->y, path, NULL, 0)) {
				changed = true;
				continue;
			}
			unlink(path); /* unreadable: fetched again */
		}
		for (int j = 0; j < DOWNLOADS; j++) {
			if (!map.dl[j].curl) {
				download_start(&map.dl[j], k);
				break;
			}
		}
	}
	if (changed)
		view_update();
}

/* Gestures */

static void map_cb(lv_event_t *e)
{
	lv_indev_t *indev = lv_indev_active();
	switch (lv_event_get_code(e)) {
	case LV_EVENT_PRESSED:
		map.moved = 0;
		zoom_finish();
		/* A pinch whose end was not seen */
		if (map.pinch.active) {
			map.pinch.active = false;
			map.scale = 1;
			view_clamp();
			view_update();
		}
		break;
	case LV_EVENT_PRESSING: {
		lv_point_t v;
		lv_indev_get_vect(indev, &v);
		/* The first finger moves during a pinch too */
		if ((!v.x && !v.y) || map.pinch.active)
			break;
		map.moved += LV_ABS(v.x) + LV_ABS(v.y);
		map.cx -= v.x / map.scale;
		map.cy -= v.y / map.scale;
		view_clamp();
		view_update();
		break;
	}
	case LV_EVENT_SHORT_CLICKED: {
		if (map.moved > 16)
			break;
		lv_point_t p;
		lv_indev_get_point(indev, &p);
		lv_area_t a;
		lv_obj_get_coords(map.map, &a);
		p.x -= a.x1;
		p.y -= a.y1;
		/* Double tap: zoom in on that spot */
		if (map.tap_time && lv_tick_elaps(map.tap_time) < DOUBLE_TAP_MS &&
		    LV_ABS(p.x - map.tap_point.x) < 60 && LV_ABS(p.y - map.tap_point.y) < 60) {
			map.tap_time = 0;
			view_zoom(1, p.x, p.y);
		} else {
			map.tap_time = lv_tick_get() | 1;
			map.tap_point = p;
		}
		break;
	}
	default:
		break;
	}
}

static void zoom_cb(lv_event_t *e)
{
	ui_vibrate(15);
	view_zoom((int)(intptr_t)lv_event_get_user_data(e), map.w / 2, map.h / 2);
}

static lv_obj_t *zoom_button(const char *symbol, int dz, int32_t y)
{
	lv_obj_t *button = lv_button_create(map.map);
	lv_obj_set_size(button, 96, 96);
	lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, -24, y);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, zoom_cb, LV_EVENT_CLICKED, (void *)(intptr_t)dz);
	lv_obj_t *icon = lv_label_create(button);
	lv_label_set_text_static(icon, symbol);
	lv_obj_set_style_text_font(icon, &ui_font_32, 0);
	lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_center(icon);
	return button;
}

/* Saved view, as latitude and longitude (independent of the zoom) */

static void view_save(void)
{
	double world = (double)TILE * (1 << map.z);
	double lon = map.cx / world * 360 - 180;
	double lat = atan(sinh(M_PI * (1 - 2 * map.cy / world))) * 180 / M_PI;
	char buf[32];
	snprintf(buf, sizeof(buf), "%.5f", lat);
	ui_setting_set("maps.lat", buf);
	snprintf(buf, sizeof(buf), "%.5f", lon);
	ui_setting_set("maps.lon", buf);
	ui_setting_set_int("maps.zoom", map.z);
}

static void view_load(void)
{
	map.z = ui_setting_get_int("maps.zoom", MIN_ZOOM);
	if (map.z < MIN_ZOOM || map.z > MAX_ZOOM)
		map.z = MIN_ZOOM;
	double lat = atof(ui_setting_get("maps.lat", "30"));
	double lon = atof(ui_setting_get("maps.lon", "10"));
	double world = (double)TILE * (1 << map.z);
	map.cx = (lon + 180) / 360 * world;
	map.cy = (1 - asinh(tan(lat * M_PI / 180)) / M_PI) / 2 * world;
	view_clamp();
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_anim_delete(&map, zoom_anim_cb);
	view_save();
	lv_timer_delete(map.timer);
	for (int i = 0; i < DOWNLOADS; i++)
		if (map.dl[i].curl)
			download_free(&map.dl[i]);
	curl_multi_cleanup(map.multi);
	for (int i = 0; i < TEXTURES; i++)
		ui_texture_delete(map.tex[i].id);
	memset(&map, 0, sizeof(map));
}

void maps_open(void)
{
	static bool curl_ready;
	if (!curl_ready) {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		curl_ready = true;
	}

	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Maps", NULL, &body);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_scrollable(body, false);

	map.map = lv_obj_create(body);
	lv_obj_remove_style_all(map.map);
	lv_obj_set_width(map.map, LV_PCT(100));
	lv_obj_set_flex_grow(map.map, 1);
	lv_obj_set_style_bg_color(map.map, lv_color_hex(COLOR_LAND), 0);
	lv_obj_set_style_bg_opa(map.map, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(map.map, false);
	lv_obj_set_clickable(map.map, true);
	lv_obj_add_event_cb(map.map, map_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(map.map, map_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(map.map, map_cb, LV_EVENT_SHORT_CLICKED, NULL);
	lv_obj_add_event_cb(map.map, pinch_cb, LV_EVENT_GESTURE, NULL);

	/* Tile cells clip their quad, which is larger when it shows a lower zoom */
	for (int r = 0; r < ROWS; r++) {
		for (int c = 0; c < COLS; c++) {
			lv_obj_t *cell = lv_obj_create(map.map);
			lv_obj_remove_style_all(cell);
			lv_obj_set_size(cell, TILE, TILE);
			lv_obj_set_clickable(cell, false);
			lv_obj_set_scrollable(cell, false);
			lv_obj_set_hidden(cell, true);
			lv_obj_t *quad = lv_3dtexture_create(cell);
			lv_obj_set_clickable(quad, false);
			map.cells[r][c] = cell;
			map.quads[r][c] = quad;
		}
	}

	/* Required by the tile usage policy */
	lv_obj_t *credit = lv_label_create(map.map);
	lv_label_set_text_static(credit, "© OpenStreetMap contributors");
	lv_obj_set_style_text_font(credit, &ui_font_16, 0);
	lv_obj_set_style_text_color(credit, lv_color_hex(0x333333), 0);
	lv_obj_set_style_bg_color(credit, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(credit, LV_OPA_70, 0);
	lv_obj_set_style_pad_hor(credit, 8, 0);
	lv_obj_set_style_pad_ver(credit, 4, 0);
	lv_obj_align(credit, LV_ALIGN_BOTTOM_LEFT, 0, 0);

	map.status = lv_label_create(map.map);
	lv_label_set_text_static(map.status, "No internet connection");
	lv_obj_set_style_text_font(map.status, &ui_font_20, 0);
	lv_obj_set_style_text_color(map.status, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_bg_color(map.status, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_bg_opa(map.status, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(map.status, 24, 0);
	lv_obj_set_style_pad_hor(map.status, 24, 0);
	lv_obj_set_style_pad_ver(map.status, 12, 0);
	lv_obj_align(map.status, LV_ALIGN_TOP_MID, 0, 16);
	lv_obj_set_hidden(map.status, true);

	zoom_button(LV_SYMBOL_PLUS, 1, -152);
	zoom_button(LV_SYMBOL_MINUS, -1, -40);

	lv_obj_update_layout(scr);
	map.w = lv_obj_get_width(map.map);
	map.h = lv_obj_get_height(map.map);
	map.multi = curl_multi_init();
	map.scale = 1;
	/* Pinches follow the fingers from the start (the touchscreen is the only pointer).
	 * Once a recognizer reports its gesture, LVGL resets the others until the fingers
	 * lift: two fingers panning a little were a two finger swipe (or a rotation), and
	 * the pinch never came. Nothing uses those two. */
	for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
		if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER) {
			lv_indev_set_pinch_up_threshold(i, PINCH_START);
			lv_indev_set_pinch_down_threshold(i, 1 / PINCH_START);
			i->recognizers[LV_INDEV_GESTURE_ROTATE].recog_fn = NULL;
			i->recognizers[LV_INDEV_GESTURE_TWO_FINGERS_SWIPE].recog_fn = NULL;
		}
	}
	view_load();
	view_update();
	map.timer = lv_timer_create(tick_cb, 30, NULL);
	ui_push(scr);
}

#else

void maps_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Maps", NULL, &body);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *label = lv_label_create(body);
	lv_label_set_text_static(label, "Maps needs the GPU renderer");
	ui_push(scr);
}

#endif
