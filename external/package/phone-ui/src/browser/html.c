/*
 * HTML in an app's screen (ui.h, ui_html_create()): a fragment laid out and drawn
 * by Web Lite's litehtml container (page.cpp) at a fixed width, into textures of
 * TILE_H rows shown by lv_3dtexture objects in a column. Images are fetched one
 * at a time for all of them (ui_fetch), each document then laid out and drawn
 * again: its height follows.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "browser.h"
#include "ui.h"

#define TILE_H 512
#define VIEW_H 1440              /* litehtml's viewport height: vh units */
#define IMAGES 16                /* fetched per document */
#define IMAGES_BYTES (16 << 20)  /* decoded, per document */

static const char head[] = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><style>"
	"html, body { margin: 0; padding: 0; } img { max-width: 100%; }\n";

struct html {
	lv_obj_t *obj;
	struct page *page;
	int32_t w, max_h;
	unsigned *tex; /* per tile */
	int tiles;
	unsigned images;
	size_t image_bytes;
	struct html *next; /* alive */
};

static struct {
	struct html *docs;     /* alive */
	struct ui_fetch fetch; /* an image */
	struct html *for_doc;  /* whose, NULL if none or it was deleted */
	const char *url;       /* the page's */
} im;

static void tiles_free(struct html *d)
{
	for (int i = 0; i < d->tiles; i++)
		ui_texture_delete(d->tex[i]);
	free(d->tex);
	d->tex = NULL;
	d->tiles = 0;
}

/* Drawn again, all of it, at its height now */
static void draw(struct html *d)
{
	int32_t h = LV_CLAMP(1, page_height(d->page), d->max_h);
	lv_obj_set_height(d->obj, h);
#if LV_USE_3DTEXTURE
	lv_obj_clean(d->obj);
	tiles_free(d);
	int count = (h + TILE_H - 1) / TILE_H;
	uint8_t *buf = malloc((size_t)d->w * TILE_H * 4);
	d->tex = calloc(count, sizeof(*d->tex));
	if (!buf || !d->tex) {
		free(buf);
		return;
	}
	d->tiles = count;
	for (int i = 0; i < count; i++) {
		int32_t th = LV_MIN(TILE_H, h - i * TILE_H);
		page_draw(d->page, buf, d->w, th, i * TILE_H);
		d->tex[i] = ui_texture_create(buf, d->w, th);
		lv_obj_t *quad = lv_3dtexture_create(d->obj);
		lv_obj_set_clickable(quad, false);
		lv_obj_set_size(quad, d->w, th);
		lv_obj_set_pos(quad, 0, i * TILE_H);
		lv_3dtexture_set_src(quad, d->tex[i]);
	}
	free(buf);
#endif
}

static void image_arrived(const char *data, long status);

/* The next image any document wants, unless one is on its way */
static void image_next(void)
{
	if (im.fetch.curl)
		return;
	for (struct html *d = im.docs; d; d = d->next) {
		if (d->images >= IMAGES)
			continue;
		const char *url = page_next_wanted(d->page, PAGE_IMAGE);
		if (!url)
			continue;
		d->images++;
		im.for_doc = d;
		im.url = url;
		ui_fetch_start(&im.fetch, url, image_arrived);
		return;
	}
}

static void image_arrived(const char *data, long status)
{
	struct html *d = im.for_doc;
	im.for_doc = NULL;
	if (d) {
		int w = 0, h = 0, nw = 0, nh = 0;
		uint8_t *px = NULL;
		if (data && status == 200 && d->image_bytes < IMAGES_BYTES)
			px = browser_image_decode(data, im.fetch.size, &w, &h, &nw, &nh);
		if (px)
			d->image_bytes += (size_t)w * h * 4;
		page_set_image(d->page, im.url, px, w, h, nw, nh);
		if (px) {
			page_relayout(d->page);
			draw(d);
		}
	}
	image_next();
}

static void delete_cb(lv_event_t *e)
{
	struct html *d = lv_event_get_user_data(e);
	for (struct html **p = &im.docs; *p; p = &(*p)->next) {
		if (*p == d) {
			*p = d->next;
			break;
		}
	}
	if (im.for_doc == d) {
		ui_fetch_stop(&im.fetch);
		im.for_doc = NULL;
	}
	tiles_free(d);
	page_destroy(d->page);
	free(d);
	image_next();
}

lv_obj_t *ui_html_create(lv_obj_t *parent, const char *html, const char *base_url, int32_t w, int32_t max_h,
                         const char *css)
{
	lv_obj_t *obj = ui_box(parent);
	lv_obj_set_size(obj, w, 1);
	struct html *d = calloc(1, sizeof(*d));
	size_t size = sizeof(head) + (css ? strlen(css) : 0) + strlen(html) + 64;
	char *doc = malloc(size);
	if (!d || !doc) {
		free(d);
		free(doc);
		return obj;
	}
	snprintf(doc, size, "%s%s</style></head><body>%s</body></html>", head, css ? css : "", html);
	d->obj = obj;
	d->w = w;
	d->max_h = max_h;
	d->page = page_create(doc, strlen(doc), base_url, w, VIEW_H);
	free(doc);
	d->next = im.docs;
	im.docs = d;
	lv_obj_add_event_cb(obj, delete_cb, LV_EVENT_DELETE, d);
	draw(d);
	image_next();
	return obj;
}
