/*
 * Web Lite: web pages laid out by litehtml (page.cpp): HTML and CSS, no
 * JavaScript, no forms. Pages come over HTTPS when the phone has internet
 * (through the host, `just share-internet`); nothing is kept (no cache, cookies
 * or history beyond the screen).
 *
 * The page is drawn on the CPU in tiles as wide as the screen, each uploaded as a
 * GL texture shown by an lv_3dtexture in a scrolling view, like the map tiles:
 * only the tiles near the view exist, drawn a few per timer tick, nearest first.
 * Style sheets are downloaded before a page is shown (the previous one stays
 * meanwhile); images (PNG, JPEG, WebP, SVG) after, the page laid out again as they arrive.
 *
 * The bar below has back, the address (a URL, or words to search with DuckDuckGo
 * Lite) and reload / stop.
 */

#define _GNU_SOURCE /* vasprintf, stpcpy, memmem */

#include <ctype.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../i18n.h"
#include "../ui.h"

#if LV_USE_3DTEXTURE

#include <curl/curl.h>
#include <pthread.h>
#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>

#include "browser.h"

#define TILE_H 512
#define KEEP_TILES 3         /* textures kept beyond the view on each side */
#define DRAW_BUDGET_MS 12    /* tiles drawn per tick */
#define TICK_MS 16
#define FETCHES 6            /* downloads at a time */
#define HTML_MAX (8 << 20)
#define CSS_MAX (2 << 20)
#define IMAGE_MAX (8 << 20)
#define IMAGE_W 720          /* wider images are scaled down to this */
#define IMAGE_PIXELS (16 << 20)
#define IMAGES_BYTES (96 << 20) /* decoded images per page */
#define CSS_ROUNDS 3         /* @import depth */
#define BUILD_THREAD_BYTES (256 << 10) /* pages this long built on a thread */
#define RELAYOUT_MS 1000     /* at most this often while images arrive, */
#define RELAYOUT_IDLE 4      /* the view left this many times a layout's time in between */
#define HISTORY 32
#define HOME "about:home"
#define SEARCH "https://lite.duckduckgo.com/lite/?q="
#define USER_AGENT "Mozilla/5.0 (Linux; Mobile) phone-ui/1.0 (PinePhone)" /* NON-NLS */

static char *strdup_printf(const char *fmt, ...) LV_FORMAT_ATTRIBUTE(1, 2);

/* The start page, its heading and hint in the phone's language (malloc'd) */
static char *home_html(void)
{
	/* NON-NLS-BEGIN: HTML, the texts are its arguments */
	return strdup_printf("<html><head><title>Web Lite</title><style>"
	       "body { margin: 0; padding: 16px; background: #f2f2f7; color: #1c1c1e; }"
	       "h1 { font-size: 32px; margin: 16px 8px 24px; }"
	       "a { display: block; background: #fff; color: #0a84ff; text-decoration: none;"
	       " padding: 14px 16px; margin-bottom: 8px; font-size: 20px; }"
	       "a span { display: block; color: #8e8e93; font-size: 14px; margin-top: 4px; }"
	       "p { color: #8e8e93; font-size: 14px; margin: 24px 8px; }"
	       "</style></head><body><h1>%s</h1>"
	       "<a href=\"https://en.m.wikipedia.org/\">Wikipedia<span>en.m.wikipedia.org</span></a>"
	       "<a href=\"https://news.ycombinator.com/\">Hacker News<span>news.ycombinator.com</span></a>"
	       "<a href=\"https://lite.cnn.com/\">CNN Lite<span>lite.cnn.com</span></a>"
	       "<a href=\"https://text.npr.org/\">NPR Text<span>text.npr.org</span></a>"
	       "<a href=\"https://lobste.rs/\">Lobsters<span>lobste.rs</span></a>"
	       "<a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo Lite<span>lite.duckduckgo.com</span></a>"
	       "<p>%s</p></body></html>",
	       /* NON-NLS-END */
	       C_("start page", "Start"),
	       _("Type an address or words to search in the bar below. Pages show without JavaScript."));
}

enum fetch_kind { FETCH_PAGE, FETCH_CSS, FETCH_IMAGE };

struct fetch {
	CURL *curl; /* NULL: free */
	enum fetch_kind kind;
	char *url;
	char *data;
	size_t size, max;
};

struct tile {
	lv_obj_t *quad; /* NULL: not drawn */
	unsigned tex;
	bool stale; /* drawn before the last layout */
};

struct entry {
	char *url;
	int32_t scroll;
};

static struct {
	lv_obj_t *title, *view, *doc, *ta, *kb, *reload;
	lv_timer_t *timer;
	CURLM *multi;
	struct fetch fetches[FETCHES];
	struct page *page; /* shown */
	struct page *next; /* waiting for its style sheets, or being built */
	size_t next_size, page_size; /* their HTML */
	int css_round;
	bool css_fetched; /* style sheets arrived since it was parsed */
	struct tile *tiles;
	int tile_count;
	uint8_t *buf; /* a tile's pixels */
	int32_t w, h; /* the view */
	size_t image_bytes;
	bool relayout;
	uint32_t relayout_at;
	struct entry history[HISTORY];
	int history_count;
	int32_t scroll_to; /* once the next page shows, -1: top or its #fragment */
} br;

/* Pages worked on on a thread of their own (below), br.next's and br.page's; NULL if none */
struct job;
static struct job *building, *laying_out;

static void navigate(const char *url, bool push);

/* Text */

static char *html_escape(const char *s, size_t n)
{
	char *out = malloc(n * 6 + 1), *o = out;
	if (!out)
		return NULL;
	for (size_t i = 0; i < n; i++) {
		switch (s[i]) {
		case '<': o = stpcpy(o, "&lt;"); break;
		case '>': o = stpcpy(o, "&gt;"); break;
		case '&': o = stpcpy(o, "&amp;"); break;
		case '"': o = stpcpy(o, "&quot;"); break;
		default: *o++ = s[i];
		}
	}
	*o = '\0';
	return out;
}

static char *strdup_printf(const char *fmt, ...)
{
	va_list ap;
	char *s;
	va_start(ap, fmt);
	if (vasprintf(&s, fmt, ap) < 0)
		s = NULL;
	va_end(ap);
	return s;
}

/* Images */

/* Nearest-neighbor down to IMAGE_W wide; frees px if it makes a new one */
static uint8_t *image_shrink(uint8_t *px, int w, int h, int *ow, int *oh)
{
	*ow = w;
	*oh = h;
	if (w <= IMAGE_W)
		return px;
	int nw = IMAGE_W, nh = (int)((int64_t)h * IMAGE_W / w);
	if (nh < 1)
		nh = 1;
	uint8_t *out = malloc((size_t)nw * nh * 4);
	if (out) {
		for (int y = 0; y < nh; y++) {
			const uint32_t *src = (const uint32_t *)px + (size_t)((int64_t)y * h / nh) * w;
			uint32_t *d = (uint32_t *)out + (size_t)y * nw;
			for (int x = 0; x < nw; x++)
				d[x] = src[(int64_t)x * w / nw];
		}
		*ow = nw;
		*oh = nh;
	}
	free(px);
	return out;
}

static uint8_t *png_decode(const char *data, size_t size, int *w, int *h, int *nw, int *nh)
{
	png_image img = { .version = PNG_IMAGE_VERSION };
	if (!png_image_begin_read_from_memory(&img, data, size))
		return NULL;
	if ((uint64_t)img.width * img.height > IMAGE_PIXELS) {
		png_image_free(&img);
		return NULL;
	}
	/* LVGL's layout: its texture shader swaps red and blue back (c.bgra) */
	img.format = PNG_FORMAT_BGRA;
	uint8_t *px = malloc(PNG_IMAGE_SIZE(img));
	if (!px || !png_image_finish_read(&img, NULL, px, 0, NULL)) {
		free(px);
		png_image_free(&img);
		return NULL;
	}
	*nw = img.width;
	*nh = img.height;
	return image_shrink(px, img.width, img.height, w, h);
}

static uint8_t *webp_decode(const char *data, size_t size, int *w, int *h, int *nw, int *nh)
{
	int iw, ih;
	if (!WebPGetInfo((const uint8_t *)data, size, &iw, &ih) || (uint64_t)iw * ih > IMAGE_PIXELS)
		return NULL;
	/* BGRA in a buffer of ours (free(), as the others) */
	size_t stride = (size_t)iw * 4;
	uint8_t *px = malloc(stride * ih);
	if (!px || !WebPDecodeBGRAInto((const uint8_t *)data, size, px, stride * ih, (int)stride)) {
		free(px);
		return NULL;
	}
	*nw = iw;
	*nh = ih;
	return image_shrink(px, iw, ih, w, h);
}

struct jpeg_error {
	struct jpeg_error_mgr mgr;
	jmp_buf jmp;
};

static void jpeg_error_exit(j_common_ptr cinfo)
{
	longjmp(((struct jpeg_error *)cinfo->err)->jmp, 1);
}

static uint8_t *jpeg_decode(const char *data, size_t size, int *w, int *h, int *nw, int *nh)
{
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error err;
	uint8_t *volatile px = NULL;
	cinfo.err = jpeg_std_error(&err.mgr);
	err.mgr.error_exit = jpeg_error_exit;
	if (setjmp(err.jmp)) {
		jpeg_destroy_decompress(&cinfo);
		free(px);
		return NULL;
	}
	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, (const unsigned char *)data, size);
	jpeg_read_header(&cinfo, TRUE);
	if ((uint64_t)cinfo.image_width * cinfo.image_height > 4 * (uint64_t)IMAGE_PIXELS)
		longjmp(err.jmp, 1);
	cinfo.out_color_space = JCS_EXT_BGRA;
	/* Decoded at the smallest n/8 still IMAGE_W wide */
	cinfo.scale_denom = 8;
	cinfo.scale_num = 8;
	while (cinfo.scale_num > 1 && cinfo.image_width * (cinfo.scale_num - 1) / 8 >= IMAGE_W)
		cinfo.scale_num--;
	jpeg_start_decompress(&cinfo);
	size_t stride = cinfo.output_width * 4;
	px = malloc(stride * cinfo.output_height);
	if (!px)
		longjmp(err.jmp, 1);
	while (cinfo.output_scanline < cinfo.output_height) {
		uint8_t *row = px + cinfo.output_scanline * stride;
		jpeg_read_scanlines(&cinfo, &row, 1);
	}
	*nw = cinfo.image_width;
	*nh = cinfo.image_height;
	int ow = cinfo.output_width, oh = cinfo.output_height;
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	return image_shrink(px, ow, oh, w, h);
}

/* The bytes of a data: URI, base64 or percent-encoded (an SVG's text), malloc'd */
static char *data_uri_decode(const char *uri, size_t *size)
{
	const char *comma = strchr(uri, ',');
	if (!comma)
		return NULL;
	const char *s = comma + 1;
	if (!memmem(uri, comma - uri, ";base64", 7)) {
		char *out = malloc(strlen(s) + 1), *d = out;
		if (!out)
			return NULL;
		for (; *s; s++) {
			if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
				char hex[3] = { s[1], s[2], 0 };
				*d++ = (char)strtol(hex, NULL, 16);
				s += 2;
			} else {
				*d++ = *s;
			}
		}
		*size = d - out;
		return out;
	}
	char *out = malloc(strlen(s) * 3 / 4 + 3);
	if (!out)
		return NULL;
	uint32_t acc = 0;
	int bits = 0;
	size_t n = 0;
	for (; *s; s++) {
		/* Percent-encoded too: %0A (a line break), %2B (+) */
		char c = *s;
		if (c == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
			char hex[3] = { s[1], s[2], 0 };
			c = (char)strtol(hex, NULL, 16);
			s += 2;
		}
		if (c == '=')
			break;
		const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		const char *p = c ? strchr(a, c) : NULL;
		if (!p)
			continue; /* whitespace */
		acc = acc << 6 | (uint32_t)(p - a);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[n++] = (char)(acc >> bits);
		}
	}
	*size = n;
	return out;
}

uint8_t *browser_image_decode(const char *data, size_t size, int *w, int *h, int *natural_w,
                              int *natural_h)
{
	if (size >= 3 && !memcmp(data, "\xff\xd8\xff", 3))
		return jpeg_decode(data, size, w, h, natural_w, natural_h);
	if (size >= 8 && !memcmp(data, "\x89PNG", 4))
		return png_decode(data, size, w, h, natural_w, natural_h);
	if (size >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4))
		return webp_decode(data, size, w, h, natural_w, natural_h);
	if (svg_is(data, size))
		return svg_decode(data, size, IMAGE_W, IMAGE_PIXELS, w, h, natural_w, natural_h);
	return NULL;
}

static void image_arrived(const char *url, const char *data, size_t size)
{
	int w = 0, h = 0, nw = 0, nh = 0;
	uint8_t *px = NULL;
	if (data && br.image_bytes < IMAGES_BYTES)
		px = browser_image_decode(data, size, &w, &h, &nw, &nh);
	if (px)
		br.image_bytes += (size_t)w * h * 4;
	page_set_image(br.page, url, px, w, h, nw, nh);
	if (px)
		br.relayout = true;
}

/* Tiles */

static void tile_free(struct tile *t)
{
	if (t->quad)
		lv_obj_delete(t->quad);
	ui_texture_delete(t->tex);
	memset(t, 0, sizeof(*t));
}

static void tiles_clear(void)
{
	for (int i = 0; i < br.tile_count; i++)
		tile_free(&br.tiles[i]);
	free(br.tiles);
	br.tiles = NULL;
	br.tile_count = 0;
}

/* After a layout: the page's height, all tiles to draw again */
static void tiles_layout(void)
{
	/* Dark in the dark theme, as the view around it */
	if (br.page)
		page_set_dark(br.page, !ui_light);
	int32_t height = br.page ? page_height(br.page) : 0;
	if (height < br.h)
		height = br.h;
	int count = (height + TILE_H - 1) / TILE_H;
	for (int i = count; i < br.tile_count; i++)
		tile_free(&br.tiles[i]);
	struct tile *tiles = realloc(br.tiles, count * sizeof(*tiles));
	if (!tiles)
		return;
	if (count > br.tile_count)
		memset(tiles + br.tile_count, 0, (count - br.tile_count) * sizeof(*tiles));
	br.tiles = tiles;
	br.tile_count = count;
	for (int i = 0; i < count; i++)
		br.tiles[i].stale = true;
	lv_obj_set_height(br.doc, height);
}

static void tile_draw(int i)
{
	struct tile *t = &br.tiles[i];
	page_draw(br.page, br.buf, br.w, TILE_H, i * TILE_H);
	unsigned tex = ui_texture_create(br.buf, br.w, TILE_H);
	if (!t->quad) {
		t->quad = lv_3dtexture_create(br.doc);
		lv_obj_set_clickable(t->quad, false);
		lv_obj_set_size(t->quad, br.w, TILE_H);
		lv_obj_set_pos(t->quad, 0, i * TILE_H);
	}
	/* The new texture shown before the old one goes */
	lv_3dtexture_set_src(t->quad, tex);
	ui_texture_delete(t->tex);
	t->tex = tex;
	t->stale = false;
	lv_obj_invalidate(t->quad);
}

static void tiles_update(void)
{
	if (!br.page || !br.tile_count || laying_out)
		return;
	int32_t y = lv_obj_get_scroll_y(br.view);
	int first = y / TILE_H, last = (y + br.h - 1) / TILE_H;
	for (int i = 0; i < br.tile_count; i++)
		if (br.tiles[i].quad && (i < first - KEEP_TILES || i > last + KEEP_TILES))
			tile_free(&br.tiles[i]);
	/* The view first, then below it (where reading goes), then above */
	uint32_t start = lv_tick_get();
	for (int k = 0; k <= last - first + 2 * KEEP_TILES; k++) {
		int i = k <= last - first ? first + k :
		        k <= last - first + KEEP_TILES ? last + (k - (last - first)) :
		        first - (k - (last - first) - KEEP_TILES);
		if (i < 0 || i >= br.tile_count)
			continue;
		if (br.tiles[i].quad && !br.tiles[i].stale)
			continue;
		tile_draw(i);
		if (lv_tick_elaps(start) >= DRAW_BUDGET_MS)
			break;
	}
}

/* Pages */

static void loading_show(bool loading)
{
	lv_label_set_text_static(lv_obj_get_child(br.reload, 0), loading ? LV_SYMBOL_CLOSE : LV_SYMBOL_REFRESH);
	if (loading)
		lv_label_set_text_static(br.title, _("Loading..."));
}

static void address_show(void)
{
	if (!lv_obj_has_state(br.ta, LV_STATE_FOCUSED))
		lv_textarea_set_text(br.ta, br.page && strcmp(page_url(br.page), HOME) ? page_url(br.page) : "");
}

static bool fetch_start(enum fetch_kind kind, const char *url, size_t max);

static void images_request(void)
{
	/* Not while it is laid out on its thread, which may ask for more */
	if (laying_out)
		return;
	for (int i = 0; i < FETCHES && br.page; i++) {
		if (br.fetches[i].curl)
			continue;
		const char *url;
		while ((url = page_next_wanted(br.page, PAGE_IMAGE)) && !strncmp(url, "data:", 5)) {
			size_t size = 0;
			char *data = data_uri_decode(url, &size);
			image_arrived(url, data, size);
			free(data);
		}
		if (!url)
			return;
		fetch_start(FETCH_IMAGE, url, IMAGE_MAX);
	}
}

/*
 * Long pages are built and laid out on a thread of their own (seconds for Wikipedia's
 * longest articles), the view staying live. A page being worked on is left alone until
 * it is done: br.next until build_poll() goes on with it; br.page, laid out again as
 * images arrive, gets no new tiles, fetches' results, images asked or taps meanwhile.
 * One dropped meanwhile is destroyed by its thread. Text is measured under LVGL's lock
 * (page.cpp). Shorter pages are worked on here, as all of them in the browser's build.
 */

struct job {
	struct page *page;
	bool layout;        /* page_relayout(), else page_rebuild() */
	bool destroy;       /* page_destroy() only */
	bool done, dropped; /* job_lock */
};

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t build_start, layout_start;

#ifndef __EMSCRIPTEN__
static void *job_run(void *arg)
{
	struct job *j = arg;
	if (!j->destroy) {
		if (j->layout)
			page_relayout(j->page);
		else
			page_rebuild(j->page);
		pthread_mutex_lock(&job_lock);
		j->done = true;
		j->destroy = j->dropped;
		pthread_mutex_unlock(&job_lock);
	}
	if (j->destroy) {
		page_destroy(j->page);
		free(j);
	}
	return NULL;
}
#endif

/* The job on its thread, for a page of `size` bytes of HTML; NULL if shorter than
 * BUILD_THREAD_BYTES or the thread did not start (then the caller does it) */
static struct job *job_start(struct page *p, size_t size, bool layout, bool destroy)
{
#ifndef __EMSCRIPTEN__
	struct job *j = size >= BUILD_THREAD_BYTES ? calloc(1, sizeof(*j)) : NULL;
	if (!j)
		return NULL;
	*j = (struct job){ .page = p, .layout = layout, .destroy = destroy };
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t thread;
	bool started = pthread_create(&thread, &attr, job_run, j) == 0;
	pthread_attr_destroy(&attr);
	if (started)
		return j;
	free(j);
#else
	LV_UNUSED(p);
	LV_UNUSED(size);
	LV_UNUSED(layout);
	LV_UNUSED(destroy);
#endif
	return NULL;
}

static bool job_done(const struct job *j)
{
	pthread_mutex_lock(&job_lock);
	bool done = j->done;
	pthread_mutex_unlock(&job_lock);
	return done;
}

/* *j given up: true if its thread destroys the page once done, false if it was done
 * (the caller's to destroy) */
static bool job_drop(struct job **j)
{
	pthread_mutex_lock(&job_lock);
	bool done = (*j)->done;
	(*j)->dropped = !done;
	pthread_mutex_unlock(&job_lock);
	if (done)
		free(*j);
	*j = NULL;
	return !done;
}

/* The CPU at its highest while a page is worked on */
static void boost_update(void)
{
	ui_cpu_boost(building || laying_out);
}

/* A page destroyed: a long one on a thread (its tree takes a while to free), br.page
 * by the thread laying it out if there is one */
static void page_drop(struct page *p, size_t size)
{
	if (!p)
		return;
	if (p == br.page && laying_out) {
		bool theirs = job_drop(&laying_out);
		boost_update();
		if (theirs)
			return;
	}
	if (!job_start(p, size, false, true))
		page_destroy(p);
}

static void page_show(struct page *p)
{
	tiles_clear();
	page_drop(br.page, br.page_size);
	br.page = p;
	br.page_size = br.next_size;
	br.next = NULL;
	br.image_bytes = 0;
	br.relayout = false;
	const char *title = page_title(p);
	if (*title)
		lv_label_set_text(br.title, title);
	else
		lv_label_set_text_static(br.title, _("Web Lite"));
	address_show();
	loading_show(false);
	tiles_layout();
	lv_obj_update_layout(br.view);

	const char *hash = strchr(page_url(p), '#');
	int32_t y = br.scroll_to;
	if (y < 0 && hash)
		y = page_anchor_y(p, hash + 1);
	lv_obj_scroll_to_y(br.view, y > 0 ? y : 0, LV_ANIM_OFF);
	br.scroll_to = -1;
	tiles_update();
	images_request();
}

/* Builds br.next at the CPU's highest: true if on its thread, done later */
static bool build_next(void)
{
	build_start = lv_tick_get();
	ui_cpu_boost(true);
	building = job_start(br.next, br.next_size, false, false);
	if (building)
		return true;
	page_rebuild(br.next);
	boost_update();
	printf("browser: built in %u ms, %d px\n", lv_tick_elaps(build_start), page_height(br.next));
	return false;
}

static void css_request(void);

/* Each tick: br.next's build done on its thread goes on */
static void build_poll(void)
{
	if (!building || !job_done(building))
		return;
	free(building);
	building = NULL;
	boost_update();
	printf("browser: built in %u ms on a thread, %d px\n", lv_tick_elaps(build_start), page_height(br.next));
	css_request();
}

/* br.next destroyed, or left to its thread to destroy when done */
static void next_drop(void)
{
	if (!br.next)
		return;
	bool theirs = building && job_drop(&building);
	boost_update();
	if (!theirs)
		page_destroy(br.next);
	br.next = NULL;
}

/* Waits for the next page's style sheets, then shows it */
static void css_request(void)
{
	for (;;) {
		int active = 0;
		for (int i = 0; i < FETCHES; i++) {
			if (br.fetches[i].curl) {
				active += br.fetches[i].kind == FETCH_CSS;
				continue;
			}
			const char *url = br.css_round < CSS_ROUNDS ? page_next_wanted(br.next, PAGE_CSS) : NULL;
			if (url && fetch_start(FETCH_CSS, url, CSS_MAX))
				active++;
		}
		if (active) {
			br.css_fetched = true;
			return;
		}
		if (page_built(br.next) && !br.css_fetched)
			break;
		/* Built with those that arrived (none: there were none, or they failed to start),
		 * which can ask for more (@import, those the head did not list): here again once
		 * built on its thread */
		br.css_fetched = false;
		br.css_round++;
		if (build_next())
			return;
	}
	page_show(br.next);
}

static void page_arrived(const char *html, size_t size, const char *url)
{
	uint32_t start = lv_tick_get();
	next_drop();
	/* Built once its style sheets are in (css_request()) */
	br.next = page_load(html, size, url, br.w, br.h);
	br.next_size = size;
	br.css_round = 0;
	br.css_fetched = false;
	printf("browser: %s: %zu bytes, read in %u ms, %d px\n", url, size, lv_tick_elaps(start),
	       page_height(br.next));
	css_request();
}

static void error_page(const char *url, const char *what)
{
	char *u = html_escape(url, strlen(url));
	/* NON-NLS-BEGIN: HTML */
	char *html = strdup_printf("<html><head><title>%s</title></head>"
		"<body style=\"margin: 24px 16px\"><h2>%s</h2>"
		"<p>%s</p><p style=\"color: #8e8e93\">%s</p></body></html>",
		_("Not loaded"), _("The page could not be loaded"), what, u ? u : "");
	/* NON-NLS-END */
	if (html)
		page_arrived(html, strlen(html), url);
	free(html);
	free(u);
}

/* Downloads: libcurl's multi interface, polled by the screen's timer */

static size_t write_cb(char *data, size_t size, size_t n, void *user)
{
	struct fetch *f = user;
	if (f->size + size * n > f->max)
		return 0;
	char *p = realloc(f->data, f->size + size * n + 1);
	if (!p)
		return 0;
	memcpy(p + f->size, data, size * n);
	f->data = p;
	f->size += size * n;
	p[f->size] = '\0';
	return size * n;
}

static void fetch_stop(struct fetch *f)
{
	if (f->curl) {
		curl_multi_remove_handle(br.multi, f->curl);
		curl_easy_cleanup(f->curl);
	}
	free(f->url);
	free(f->data);
	memset(f, 0, sizeof(*f));
}

static void fetches_stop(void)
{
	for (int i = 0; i < FETCHES; i++)
		fetch_stop(&br.fetches[i]);
}

static bool fetch_start(enum fetch_kind kind, const char *url, size_t max)
{
	struct fetch *f = NULL;
	for (int i = 0; i < FETCHES && !f; i++)
		if (!br.fetches[i].curl)
			f = &br.fetches[i];
	if (!f || !(f->curl = curl_easy_init()))
		return false;
	f->kind = kind;
	f->url = strdup(url);
	f->max = max;
	curl_easy_setopt(f->curl, CURLOPT_URL, url);
	curl_easy_setopt(f->curl, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(f->curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(f->curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(f->curl, CURLOPT_MAXREDIRS, 10L);
	curl_easy_setopt(f->curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(f->curl, CURLOPT_WRITEDATA, f);
	curl_easy_setopt(f->curl, CURLOPT_PRIVATE, f);
	curl_easy_setopt(f->curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(f->curl, CURLOPT_TIMEOUT, kind == FETCH_PAGE ? 30L : 20L);
	curl_multi_add_handle(br.multi, f->curl);
	return true;
}

static void page_fetched(struct fetch *f, bool ok, long code)
{
	if (!ok) {
		char what[64];
		snprintf(what, sizeof(what), code ? "HTTP %ld" : _("No connection"), code);
		error_page(f->url, what);
		return;
	}
	char *url = NULL, *type = NULL;
	curl_easy_getinfo(f->curl, CURLINFO_EFFECTIVE_URL, &url);
	curl_easy_getinfo(f->curl, CURLINFO_CONTENT_TYPE, &type);
	url = strdup(url ? url : f->url);
	/* The address after redirects, so that back and reload go there */
	if (br.history_count) {
		free(br.history[br.history_count - 1].url);
		br.history[br.history_count - 1].url = strdup(url);
	}
	if (type && !strncmp(type, "image/", 6)) {
		char *u = html_escape(url, strlen(url));
		/* NON-NLS-BEGIN: HTML */
		char *html = strdup_printf("<html><body style=\"margin: 0\"><img src=\"%s\" "
		                           "style=\"max-width: 100%%\"></body></html>", u ? u : "");
		/* NON-NLS-END */
		if (html)
			page_arrived(html, strlen(html), url);
		free(html);
		free(u);
	} else if (type && !strncmp(type, "text/plain", 10)) {
		char *text = html_escape(f->data, f->size);
		/* NON-NLS-BEGIN: HTML */
		char *html = text ? strdup_printf("<html><body><pre style=\"white-space: pre-wrap\">%s</pre>"
		                                  "</body></html>", text) : NULL;
		/* NON-NLS-END */
		if (html)
			page_arrived(html, strlen(html), url);
		free(html);
		free(text);
	} else if (type && strncmp(type, "text/html", 9) && strncmp(type, "application/xhtml", 17)) {
		char what[128];
		snprintf(what, sizeof(what), _("This kind of file cannot be shown (%s)."), type);
		error_page(url, what);
	} else {
		page_arrived(f->data, f->size, url);
	}
	free(url);
}

static void fetch_done(struct fetch *f, CURLcode result)
{
	long code = 0;
	curl_easy_getinfo(f->curl, CURLINFO_RESPONSE_CODE, &code);
	bool ok = result == CURLE_OK && code < 400 && f->data;
	if (result != CURLE_OK)
		printf("browser: %s: %s\n", f->url, curl_easy_strerror(result));
	else if (code >= 400)
		printf("browser: %s: HTTP %ld\n", f->url, code);

	switch (f->kind) {
	case FETCH_PAGE:
		page_fetched(f, ok && code, result == CURLE_OK ? code : 0);
		fetch_stop(f);
		break;
	case FETCH_CSS:
		page_set_css(br.next, f->url, ok ? f->data : NULL);
		fetch_stop(f);
		css_request();
		break;
	case FETCH_IMAGE:
		image_arrived(f->url, ok ? f->data : NULL, f->size);
		fetch_stop(f);
		images_request();
		break;
	}
}

static bool images_loading(void)
{
	for (int i = 0; i < FETCHES; i++)
		if (br.fetches[i].curl && br.fetches[i].kind == FETCH_IMAGE)
			return true;
	return false;
}

static void tick_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	build_poll();
	if (laying_out && job_done(laying_out)) {
		free(laying_out);
		laying_out = NULL;
		boost_update();
		br.relayout_at = lv_tick_get();
		printf("browser: images laid out in %u ms on a thread\n", lv_tick_elaps(layout_start));
		tiles_layout();
	}
	int running, left;
	curl_multi_perform(br.multi, &running);
	CURLMsg *m;
	/* What arrived waits while the page is laid out on its thread */
	while (!laying_out && (m = curl_multi_info_read(br.multi, &left))) {
		if (m->msg != CURLMSG_DONE)
			continue;
		struct fetch *f = NULL;
		curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, (char **)&f);
		if (f)
			fetch_done(f, m->data.result);
	}
	if (laying_out)
		return;

	/* A long page takes a second to lay out: the view scrolls in between */
	unsigned parse_ms = 0, layout_ms = 0;
	if (br.page)
		page_timings(br.page, &parse_ms, &layout_ms);
	if (br.relayout && (!images_loading() ||
	                    lv_tick_elaps(br.relayout_at) >= LV_MAX(RELAYOUT_MS, RELAYOUT_IDLE * layout_ms))) {
		layout_start = lv_tick_get();
		br.relayout = false;
		ui_cpu_boost(true);
		laying_out = job_start(br.page, br.page_size, true, false);
		if (laying_out)
			return;
		page_relayout(br.page);
		boost_update();
		br.relayout_at = lv_tick_get();
		printf("browser: images laid out in %u ms\n", lv_tick_elaps(layout_start));
		tiles_layout();
	}
	tiles_update();
}

/* Navigation */

static void history_push(const char *url)
{
	if (br.history_count == HISTORY) {
		free(br.history[0].url);
		memmove(br.history, br.history + 1, (HISTORY - 1) * sizeof(br.history[0]));
		br.history_count--;
	}
	if (br.history_count)
		br.history[br.history_count - 1].scroll = lv_obj_get_scroll_y(br.view);
	br.history[br.history_count++] = (struct entry){ strdup(url), 0 };
}

static void navigate(const char *url, bool push)
{
	fetches_stop();
	next_drop();
	/* A link to a place on the same page: scrolls there */
	const char *hash = strchr(url, '#');
	if (push && hash && br.page && !laying_out &&
	    !strncmp(url, page_url(br.page), hash - url) &&
	    (page_url(br.page)[hash - url] == '\0' || page_url(br.page)[hash - url] == '#')) {
		int32_t y = page_anchor_y(br.page, hash + 1);
		if (y >= 0)
			lv_obj_scroll_to_y(br.view, y, LV_ANIM_ON);
		images_request();
		return;
	}
	if (push) {
		history_push(url);
		br.scroll_to = -1;
	}
	if (!strcmp(url, HOME)) {
		char *html = home_html();
		if (html)
			page_arrived(html, strlen(html), HOME);
		free(html);
		return;
	}
	if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
		error_page(url, _("Only web pages (http, https) can be opened."));
		return;
	}
	loading_show(true);
	fetch_start(FETCH_PAGE, url, HTML_MAX);
}

/* What was typed: an address, or words to search */
static char *address_parse(const char *text)
{
	while (*text == ' ')
		text++;
	if (!*text)
		return NULL;
	if (strstr(text, "://") || !strncmp(text, "about:", 6))
		return strdup(text);
	if (!strchr(text, ' ') && strchr(text, '.'))
		return strdup_printf("https://%s", text);
	char *q = curl_easy_escape(NULL, text, 0);
	char *url = q ? strdup_printf(SEARCH "%s", q) : NULL;
	curl_free(q);
	return url;
}

static void back_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (br.history_count < 2) {
		ui_pop();
		return;
	}
	free(br.history[--br.history_count].url);
	br.scroll_to = br.history[br.history_count - 1].scroll;
	navigate(br.history[br.history_count - 1].url, false);
}

static void reload_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	bool loading = false;
	for (int i = 0; i < FETCHES; i++)
		loading |= br.fetches[i].curl && br.fetches[i].kind != FETCH_IMAGE;
	if (loading || br.next) {
		/* Stop: the shown page stays */
		fetches_stop();
		next_drop();
		loading_show(false);
		if (br.page && *page_title(br.page))
			lv_label_set_text(br.title, page_title(br.page));
		else
			lv_label_set_text_static(br.title, _("Web Lite"));
		return;
	}
	if (br.history_count) {
		br.scroll_to = lv_obj_get_scroll_y(br.view);
		navigate(br.history[br.history_count - 1].url, false);
	}
}

static void ta_cb(lv_event_t *e)
{
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		ui_keyboard_show(br.kb, true);
		break;
	case LV_EVENT_READY: {
		char *url = address_parse(lv_textarea_get_text(br.ta));
		ui_keyboard_show(br.kb, false);
		lv_obj_remove_state(br.ta, LV_STATE_FOCUSED);
		if (url)
			navigate(url, true);
		free(url);
		break;
	}
	case LV_EVENT_CANCEL:
		ui_keyboard_show(br.kb, false);
		lv_obj_remove_state(br.ta, LV_STATE_FOCUSED);
		address_show();
		break;
	default:
		break;
	}
}

static void view_clicked_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!br.page || laying_out)
		return;
	lv_point_t p;
	lv_area_t a;
	lv_indev_get_point(lv_indev_active(), &p);
	lv_obj_get_coords(br.view, &a);
	char *url = page_click(br.page, p.x - a.x1, p.y - a.y1 + lv_obj_get_scroll_y(br.view));
	if (url) {
		printf("browser: link %s\n", url);
		ui_vibrate(UI_VIBRATE_TAP);
		if (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8))
			navigate(url, true);
		free(url);
	}
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_timer_delete(br.timer);
	fetches_stop();
	curl_multi_cleanup(br.multi);
	tiles_clear();
	page_drop(br.page, br.page_size);
	next_drop();
	free(br.buf);
	for (int i = 0; i < br.history_count; i++)
		free(br.history[i].url);
	memset(&br, 0, sizeof(br));
}

void browser_open(void)
{
	static bool curl_ready;
	if (!curl_ready) {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		curl_ready = true;
	}

	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create(_("Web Lite"), &header, &body);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
	br.title = lv_obj_get_child(header, 1);
	br.scroll_to = -1;

	br.view = lv_obj_create(body);
	lv_obj_remove_style_all(br.view);
	lv_obj_set_width(br.view, LV_PCT(100));
	lv_obj_set_flex_grow(br.view, 1);
	lv_obj_set_style_bg_opa(br.view, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(br.view, ui_light ? lv_color_white() : lv_color_hex(PAGE_DARK_PAPER), 0);
	lv_obj_set_scroll_dir(br.view, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(br.view, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(br.view, view_clicked_cb, LV_EVENT_CLICKED, NULL);

	br.doc = ui_box(br.view);
	lv_obj_set_width(br.doc, LV_PCT(100));

	lv_obj_t *bar = ui_bar_create(body);
	lv_obj_t *back = ui_button(bar, 80, 80, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, LV_SYMBOL_LEFT,
	                           back_cb, NULL);
	lv_obj_set_style_text_font(lv_obj_get_child(back, 0), &ui_font_32, 0);
	br.ta = ui_text_field(bar, 80, 2048, ta_cb, NULL);
	lv_textarea_set_placeholder_text(br.ta, _("Address or search"));
	lv_obj_set_flex_grow(br.ta, 1);
	br.reload = ui_button(bar, 80, 80, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, LV_SYMBOL_REFRESH,
	                      reload_cb, NULL);
	lv_obj_set_style_text_font(lv_obj_get_child(br.reload, 0), &ui_font_32, 0);
	br.kb = ui_keyboard_create(body, br.ta);

	ui_push(scr);
	lv_obj_update_layout(scr);
	br.w = lv_obj_get_width(br.view);
	br.h = lv_obj_get_height(br.view);
	br.buf = malloc((size_t)br.w * TILE_H * 4);
	br.multi = curl_multi_init();
	br.timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	if (!br.buf || !br.multi) {
		printf("browser: out of memory\n");
		return;
	}
	navigate(HOME, true);
}

#else

void browser_open(void)
{
	printf("browser: needs OpenGL ES (lv_3dtexture)\n");
}

#endif
