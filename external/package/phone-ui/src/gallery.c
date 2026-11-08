/*
 * Gallery: the photos taken by the camera and the screenshots (/var/lib/photos,
 * JPEG or PPM), newest first, as a grid of square thumbnails; a tap opens the
 * photo viewer (viewer.c) on them, with their dates, which can delete them.
 *
 * Thumbnails are decoded scaled down while reading (ui_image_read) into GL
 * textures shown by lv_3dtexture, like the map tiles, one per timer tick,
 * newest first; the UI waits for each.
 *
 * ui_photo_pick() shows the same grid for another app to choose a photo from:
 * a tap selects one (a check on it), the header's button hands its path over.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "i18n.h"
#include "ui.h"

#if LV_USE_3DTEXTURE

#define PHOTOS "/var/lib/photos"
#define COLUMNS 3
#define GAP 4

struct photo {
	char name[64]; /* file name in PHOTOS */
	time_t mtime;
	lv_obj_t *cell;
	unsigned tex;  /* thumbnail, 0: not decoded yet */
	bool bad;      /* could not be decoded */
};

static struct {
	struct photo *photos;
	unsigned count;
	lv_obj_t *body;
	lv_timer_t *timer;
	/* Picking (ui_photo_pick), NULL: the Gallery app */
	void (*picked)(const char *path, void *user_data);
	void *user_data;
	int selected;          /* -1: none */
	lv_obj_t *done, *check;
} gal;

/* Files */

static bool photo_file(const char *name)
{
	const char *ext = strrchr(name, '.');
	return ext && (!strcmp(ext, ".jpg") || !strcmp(ext, ".jpeg") || !strcmp(ext, ".ppm"));
}

static int photo_cmp(const void *a, const void *b)
{
	const struct photo *x = a, *y = b;
	if (x->mtime != y->mtime)
		return (x->mtime < y->mtime) - (x->mtime > y->mtime);
	return -strcmp(x->name, y->name);
}

/* Photos newest first into gal.photos */
static void photos_load(void)
{
	unsigned size = 0;
	DIR *dir = opendir(PHOTOS);
	if (!dir)
		return;
	struct dirent *d;
	while ((d = readdir(dir))) {
		size_t len = strlen(d->d_name);
		if (len >= sizeof(gal.photos->name) || !photo_file(d->d_name))
			continue;
		char path[sizeof(PHOTOS) + sizeof(gal.photos->name)];
		snprintf(path, sizeof(path), PHOTOS "/%s", d->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0)
			continue;
		if (gal.count == size) {
			size = size ? size * 2 : 32;
			gal.photos = realloc(gal.photos, size * sizeof(*gal.photos));
		}
		struct photo *p = &gal.photos[gal.count++];
		memset(p, 0, sizeof(*p));
		memcpy(p->name, d->d_name, len + 1);
		p->mtime = st.st_mtime;
	}
	closedir(dir);
	if (gal.count) /* none: no array */
		qsort(gal.photos, gal.count, sizeof(*gal.photos), photo_cmp);
}

static void empty_show(void)
{
	ui_empty_label(gal.body, _("No photos"));
}

static void photo_path(char *buf, size_t size, const struct photo *p)
{
	snprintf(buf, size, PHOTOS "/%s", p->name);
}

/* Viewer (viewer.c) */

static void view_caption(int index, char *buf, size_t size, void *user_data)
{
	LV_UNUSED(user_data);
	struct tm tm;
	localtime_r(&gal.photos[index].mtime, &tm);
	snprintf(buf, size, "%d %s %d, %02d:%02d", tm.tm_mday, ui_month_name(tm.tm_mon, false), tm.tm_year + 1900,
	         tm.tm_hour, tm.tm_min);
}

/* The file, its cell and thumbnail; the viewer drops it from its own list */
static bool view_remove(int index, void *user_data)
{
	LV_UNUSED(user_data);
	struct photo *p = &gal.photos[index];
	char path[sizeof(PHOTOS) + sizeof(p->name)];
	photo_path(path, sizeof(path), p);
	if (unlink(path) != 0) {
		perror("gallery");
		return false;
	}
	lv_obj_delete(p->cell);
	ui_texture_delete(p->tex);
	memmove(p, p + 1, (gal.count - index - 1) * sizeof(*p));
	if (!--gal.count)
		empty_show();
	return true;
}

static void view_open(int index)
{
	char (*paths)[sizeof(PHOTOS) + sizeof(gal.photos->name)] = malloc(gal.count * sizeof(*paths));
	const char **list = malloc(gal.count * sizeof(*list));
	if (paths && list) {
		for (unsigned i = 0; i < gal.count; i++) {
			photo_path(paths[i], sizeof(paths[i]), &gal.photos[i]);
			list[i] = paths[i];
		}
		static const struct ui_photo_view ops = { .caption = view_caption, .remove = view_remove };
		ui_photo_view(_("Photo"), list, gal.count, index, &ops);
	}
	free(paths);
	free(list);
}

/* Grid */

/* Picking: photo i selected, or none if it was already */
static void pick_select(int i)
{
	gal.selected = gal.selected == i ? -1 : i;
	lv_obj_set_hidden(gal.check, gal.selected < 0);
	lv_obj_set_hidden(gal.done, gal.selected < 0);
	if (gal.selected >= 0) {
		lv_obj_set_parent(gal.check, gal.photos[i].cell);
		lv_obj_move_to_index(gal.check, -1);
	}
}

/* Back to the app that asked, with the photo's path */
static void pick_done_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (gal.selected < 0)
		return;
	char path[sizeof(PHOTOS) + sizeof(gal.photos->name)];
	photo_path(path, sizeof(path), &gal.photos[gal.selected]);
	void (*picked)(const char *, void *) = gal.picked;
	void *user_data = gal.user_data;
	ui_vibrate(UI_VIBRATE_TAP);
	ui_pop();
	picked(path, user_data);
}

static void cell_cb(lv_event_t *e)
{
	lv_obj_t *cell = lv_event_get_target(e);
	for (unsigned i = 0; i < gal.count; i++) {
		if (gal.photos[i].cell == cell) {
			if (gal.picked)
				pick_select(i);
			else
				view_open(i);
			return;
		}
	}
}

/* The next thumbnail, newest first; stops once they are all done */
static void thumb_cb(lv_timer_t *timer)
{
	for (unsigned i = 0; i < gal.count; i++) {
		struct photo *p = &gal.photos[i];
		if (p->tex || p->bad)
			continue;
		int32_t size = lv_obj_get_width(p->cell);
		int w, h;
		char path[sizeof(PHOTOS) + sizeof(p->name)];
		photo_path(path, sizeof(path), p);
		uint8_t *pixels = ui_image_read(path, size, size, true, &w, &h);
		if (!pixels) {
			p->bad = true;
			return;
		}
		/* The middle square, moved to the start of the buffer row by row */
		int s = w < h ? w : h, x0 = (w - s) / 2, y0 = (h - s) / 2;
		for (int y = 0; y < s; y++)
			memmove(pixels + (size_t)y * s * 4, pixels + ((size_t)(y0 + y) * w + x0) * 4,
			        (size_t)s * 4);
		p->tex = ui_texture_create(pixels, s, s);
		free(pixels);

		lv_obj_t *quad = lv_3dtexture_create(p->cell);
		lv_obj_set_size(quad, LV_PCT(100), LV_PCT(100));
		lv_obj_set_clickable(quad, false);
		lv_3dtexture_set_src(quad, p->tex);
		/* Selected before its thumbnail came: the check stays on top */
		if (gal.check && lv_obj_get_parent(gal.check) == p->cell)
			lv_obj_move_to_index(gal.check, -1);
		return;
	}
	lv_timer_pause(timer);
}

static void gallery_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_timer_delete(gal.timer);
	for (unsigned i = 0; i < gal.count; i++)
		ui_texture_delete(gal.photos[i].tex);
	free(gal.photos);
	memset(&gal, 0, sizeof(gal));
}

static void gallery_create(const char *title, void (*picked)(const char *path, void *user_data),
                           void *user_data)
{
	/* Already open (picking from an app opened over the Gallery) */
	if (gal.body)
		return;
	lv_obj_t *header;
	lv_obj_t *scr = ui_app_screen_create(title, &header, &gal.body);
	lv_obj_add_event_cb(scr, gallery_delete_cb, LV_EVENT_DELETE, NULL);
	gal.picked = picked;
	gal.user_data = user_data;
	gal.selected = -1;
	if (picked) {
		gal.done = ui_header_button(header, LV_SYMBOL_OK, pick_done_cb, NULL);
		lv_obj_set_hidden(gal.done, true);
	}
	lv_obj_set_flex_flow(gal.body, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_all(gal.body, GAP, 0);
	lv_obj_set_style_pad_bottom(gal.body, 32, 0);
	lv_obj_set_style_pad_gap(gal.body, GAP, 0);
	lv_obj_set_scroll_dir(gal.body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(gal.body, LV_SCROLLBAR_MODE_OFF);

	photos_load();
	if (!gal.count)
		empty_show();

	lv_obj_update_layout(scr);
	int32_t size = (lv_obj_get_content_width(gal.body) - (COLUMNS - 1) * GAP) / COLUMNS;
	for (unsigned i = 0; i < gal.count; i++) {
		lv_obj_t *cell = lv_obj_create(gal.body);
		lv_obj_remove_style_all(cell);
		lv_obj_set_size(cell, size, size);
		lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(cell, lv_color_hex(UI_COLOR_CARD), 0);
		lv_obj_set_scrollable(cell, false);
		lv_obj_set_clickable(cell, true);
		lv_obj_add_event_cb(cell, cell_cb, LV_EVENT_SHORT_CLICKED, NULL);
		gal.photos[i].cell = cell;
	}

	/* The selected photo's check, moved from cell to cell */
	if (picked && gal.count) {
		gal.check = ui_box(gal.photos[0].cell);
		lv_obj_set_size(gal.check, 48, 48);
		lv_obj_align(gal.check, LV_ALIGN_TOP_RIGHT, -8, 8);
		lv_obj_set_style_radius(gal.check, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(gal.check, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(gal.check, lv_color_hex(UI_COLOR_ACCENT), 0);
		lv_obj_set_style_border_width(gal.check, 3, 0);
		lv_obj_set_style_border_color(gal.check, lv_color_white(), 0);
		lv_obj_t *mark = lv_label_create(gal.check);
		lv_label_set_text_static(mark, LV_SYMBOL_OK);
		lv_obj_set_style_text_color(mark, lv_color_white(), 0);
		lv_obj_center(mark);
		lv_obj_set_hidden(gal.check, true);
	}

	gal.timer = lv_timer_create(thumb_cb, 10, NULL);
	ui_push(scr);
}

void gallery_open(void)
{
	gallery_create(_("Gallery"), NULL, NULL);
}

void ui_photo_pick(const char *title, void (*picked)(const char *path, void *user_data), void *user_data)
{
	gallery_create(title, picked, user_data);
}

#else

void gallery_open(void)
{
	ui_photo_pick(_("Gallery"), NULL, NULL);
}

void ui_photo_pick(const char *title, void (*picked)(const char *path, void *user_data), void *user_data)
{
	LV_UNUSED(picked);
	LV_UNUSED(user_data);
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(title, NULL, &body);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *label = lv_label_create(body);
	lv_label_set_text_static(label, _("The gallery needs the GPU renderer"));
	ui_push(scr);
}

#endif
