/*
 * Books: ePub files in /var/lib/books (`just books`), shown as a grid of their
 * covers with their title and author, read a page at a time. Covers are decoded
 * after the grid shows, one per timer tick (as the Gallery's thumbnails), cropped
 * to 2:3 and scaled down on the CPU into GL textures.
 *
 * A chapter (a file of the book's spine) is laid out by the browser's litehtml
 * page (browser/page.cpp) with the book's style sheets, read from the zip
 * (epub.c) while it is parsed, then laid out again with its images if it has
 * any, and cut into pages as high as the view without cutting a line of text or
 * an image (page_paginate()). A page is drawn on the CPU into a GL texture shown
 * by an lv_3dtexture; the pages on each side of it are drawn ahead, so that they
 * follow the finger as in the photo viewer. A tap on the left third goes a page
 * back, elsewhere a page on, into the next or previous chapter at the ends (laid
 * out then: the slow part); links within the book are followed. The footer has
 * the table of contents' entry the reader is in and how far into the book.
 * In the dark theme, pages are dark: page.cpp turns every color as light as it was
 * dark (white the paper, black the ink), line art too; other images as they are.
 * The place in each book is kept in /var/lib/books/.positions.
 *
 * Every step logs how long it took ("books: ..."), in /tmp/phone-ui.log.
 */

#define _GNU_SOURCE /* strcasecmp, asprintf */

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../ui.h"
#include "books.h"

#if LV_USE_3DTEXTURE

#include "../browser/browser.h"

#define POSITIONS BOOKS_DIR "/.positions"
#define URL_PREFIX "epub:/"
#define PAGE_GAP 24          /* between pages while sliding */
#define SLIDE_MS 250         /* a whole page's width */
#define FLICK_MS 300         /* a swipe shorter than that turns the page... */
#define FLICK 40             /* ...once it moved that far, else it takes a quarter of the width */
#define TAP 24               /* a press that moved less is a tap */
#define DRAW_MS 30           /* the pages on each side drawn after this, one per tick */
#define IMAGES_BYTES (96 << 20) /* decoded images per chapter */
#define FOOTER_H 56
#define MARGIN 32            /* around the text of a page, on every side */
#define SAVE_MS 5000         /* the position saved at most this often while turning pages */
#define GO_END -1            /* chapter_go(): at the end of the chapter */
#define LOADING_MS 50        /* "Loading..." on the screen before a chapter is laid out */
#define FONT_KEY "books_font_size" /* the text's size in pixels, in the settings */
#define FONT_PX 28           /* by default: an x-height 8% above the UI's 24 px Inter */

/* The text sizes offered (pixels) */
static const int font_steps[] = { 20, 22, 24, 26, 28, 30, 32, 34, 36, 40, 44, 48 };
#define FONT_STEPS (int)(sizeof(font_steps) / sizeof(font_steps[0]))

/* A page on the stage: the previous, current and next one side by side */
struct slot {
	int index;  /* in rd.starts; -1 or rd.pages: the other chapter's, blank until there */
	bool drawn;
	unsigned tex;
	lv_obj_t *sheet, *quad; /* a white sheet, the page's texture on it */
};

/* The reader */
static struct {
	struct epub *book;
	char *file; /* its name in BOOKS_DIR */
	lv_obj_t *scr, *stage, *where, *percent, *loading;
	lv_obj_t *size_bar, *size_label; /* the text size's, Aa in the header shows it */
	int font_px;
	bool relayout; /* the chapter laid out again, the text size changed */
	lv_timer_t *timer;
	/* A chapter to lay out once "Loading..." shows: where in it (chapter_go()), or
	 * the position kept (permyriad >= 0) */
	lv_timer_t *load_timer;
	int load_spine, load_permyriad;
	int32_t load_y;
	char *load_fragment;
	uint32_t load_start;
	int spine;  /* the chapter shown */
	struct page *page;
	int32_t height; /* the chapter's */
	int *starts;    /* where its pages start */
	int pages;
	int index;      /* the page shown */
	int32_t *anchors; /* where the table of contents' entries of this chapter start */
	struct slot slots[3];
	uint8_t *buf;   /* a page's pixels */
	int32_t w, h;   /* a page, the stage */
	int32_t text_h; /* inside the margins above and below */
	int32_t pitch;  /* from one page to the next */
	int32_t offset; /* of the row of pages, while dragged or sliding */
	int32_t start;  /* offset when the finger went down */
	lv_point_t press;
	uint32_t press_time;
	bool dragging, sliding;
	uint32_t saved_at;
} rd;

/* The library: a grid of the books' covers */
struct item {
	char *file, *title, *author; /* title: the file's name if it cannot be read */
	bool ok;
	bool cover_done; /* decoded, or there is none */
	unsigned tex;    /* its cover, 0: none (yet) */
	lv_obj_t *quad, *symbol; /* showing it (the placeholder until then), the symbol on that */
};

static struct {
	struct item *items;
	int count;
	int32_t cover_w, cover_h;
	unsigned placeholder; /* a cover of one color */
	lv_timer_t *timer;
} lib;

static void chapter_go(int i, int32_t y, const char *fragment, bool screen_sliding);

/* Positions: a line per book, "chapter permyriad file" (of the chapter's height,
 * where the page starts) */

static bool position_load(const char *file, int *spine, int *permyriad)
{
	FILE *f = fopen(POSITIONS, "r");
	if (!f)
		return false;
	char line[512], name[512];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f))
		found = sscanf(line, "%d %d %511[^\n]", spine, permyriad, name) == 3 && !strcmp(name, file);
	fclose(f);
	return found;
}

static void position_save(void)
{
	if (!rd.book || !rd.page || !rd.pages)
		return;
	int32_t y = rd.starts[rd.index];
	int permyriad = (int)((int64_t)y * 10000 / (rd.height > 0 ? rd.height : 1));
	mkdir(BOOKS_DIR, 0755);
	FILE *in = fopen(POSITIONS, "r");
	FILE *out = fopen(POSITIONS ".new", "w");
	if (!out) {
		perror("books: " POSITIONS);
		if (in)
			fclose(in);
		return;
	}
	char line[512], name[512];
	int a, b;
	while (in && fgets(line, sizeof(line), in))
		if (sscanf(line, "%d %d %511[^\n]", &a, &b, name) == 3 && strcmp(name, rd.file))
			fputs(line, out);
	if (in)
		fclose(in);
	fprintf(out, "%d %d %s\n", rd.spine, permyriad, rd.file);
	bool ok = fflush(out) == 0 && fsync(fileno(out)) == 0;
	ok = fclose(out) == 0 && ok;
	if (!ok || rename(POSITIONS ".new", POSITIONS)) {
		perror("books: " POSITIONS);
		unlink(POSITIONS ".new");
	}
	rd.saved_at = lv_tick_get();
}

/* Chapters */

/* A file of the book from its URL in a page (URL_PREFIX and its path), malloc'd */
static char *resource_read(const char *url, size_t *size)
{
	if (strncmp(url, URL_PREFIX, strlen(URL_PREFIX)))
		return NULL;
	char *path = epub_path("", url + strlen(URL_PREFIX), NULL);
	char *data = path ? epub_read(rd.book, path, size) : NULL;
	free(path);
	return data;
}

/* A style sheet, read as the page is parsed */
static char *css_read(const char *url, void *user)
{
	LV_UNUSED(user);
	return resource_read(url, NULL);
}

/* The page's URL for a path in the zip: what is not a plain character encoded */
static char *chapter_url(const char *path)
{
	char *url = malloc(strlen(URL_PREFIX) + strlen(path) * 3 + 1);
	if (!url)
		return NULL;
	char *o = stpcpy(url, URL_PREFIX);
	for (const unsigned char *s = (const unsigned char *)path; *s; s++) {
		if (*s <= ' ' || *s == '%' || *s == '#' || *s == '?' || *s >= 0x7f)
			o += sprintf(o, "%%%02X", *s);
		else
			*o++ = (char)*s;
	}
	*o = '\0';
	return url;
}

/* An <svg> showing an <image> (covers often are) as an <img>, which litehtml
 * draws: html is replaced (malloc'd) */
static char *svg_images(char *html, size_t *size)
{
	char *svg;
	while ((svg = strstr(html, "<svg"))) {
		char *end = strstr(svg, "</svg>");
		char *image = strstr(svg, "<image");
		char *href = image ? strstr(image, "href=\"") : NULL;
		char *close = href ? strchr(href + 6, '"') : NULL;
		if (!end || !close || close > end)
			break;
		char *out;
		int n = asprintf(&out, "%.*s<div style=\"text-align: center\"><img src=\"%.*s\" "
		                 "style=\"max-width: 100%%\"/></div>%s",
		                 (int)(svg - html), html, (int)(close - href - 6), href + 6, end + 6);
		if (n < 0)
			break;
		free(html);
		html = out;
		*size = n;
	}
	return html;
}

/* The reader's margins on the sides instead of the book's (on <html>, which books
 * leave alone; their body's side margins dropped, as other readers do), in a style
 * before its <body>: html is replaced (malloc'd). Above and below: slot_draw(). */
static char *page_margins(char *html, size_t *size)
{
	char style[160];
	snprintf(style, sizeof(style), "<style>html { padding: 0 %.2fpx } body { margin-left: 0 !important;"
	         " margin-right: 0 !important }</style>", MARGIN / BROWSER_SCALE);
	char *body = strcasestr(html, "<body");
	char *out = body ? malloc(*size + strlen(style) + 1) : NULL;
	if (!out)
		return html;
	size_t before = body - html;
	memcpy(out, html, before);
	memcpy(out + before, style, strlen(style));
	memcpy(out + before + strlen(style), body, *size - before + 1);
	*size += strlen(style);
	free(html);
	return out;
}

static struct page *chapter_layout(int i)
{
	uint32_t start = lv_tick_get();
	size_t size = 0;
	char *html = epub_read(rd.book, rd.book->spine[i], &size);
	if (!html) {
		html = strdup("<html><body><p style=\"color: #8e8e93\">This part of the book cannot be read.</p>"
		              "</body></html>");
		size = html ? strlen(html) : 0;
	}
	char *url = chapter_url(rd.book->spine[i]);
	if (!html || !url) {
		free(html);
		free(url);
		return NULL;
	}
	html = page_margins(svg_images(html, &size), &size);
	uint32_t read_ms = lv_tick_elaps(start);
	size_t html_size = size;
	struct page *p = page_create_local(html, size, url, rd.w, rd.text_h, css_read, NULL, rd.font_px);
	page_set_dark(p, !ui_light);
	free(html);
	free(url);
	unsigned parse_ms, layout_ms;
	page_timings(p, &parse_ms, &layout_ms);

	uint32_t images_start = lv_tick_get();
	const char *image_url;
	size_t image_bytes = 0;
	int images = 0;
	while ((image_url = page_next_wanted(p, PAGE_IMAGE))) {
		int w = 0, h = 0, nw = 0, nh = 0;
		uint8_t *px = NULL;
		char *data = image_bytes < IMAGES_BYTES ? resource_read(image_url, &size) : NULL;
		if (data)
			px = browser_image_decode(data, size, &w, &h, &nw, &nh);
		free(data);
		if (px)
			image_bytes += (size_t)w * h * 4;
		page_set_image(p, image_url, px, w, h, nw, nh);
		images += px != NULL;
	}
	uint32_t decode_ms = lv_tick_elaps(images_start);
	unsigned relayout_ms = 0;
	if (images) {
		page_relayout(p);
		page_timings(p, &parse_ms, &relayout_ms);
	}
	printf("books: %s: %zu bytes read in %u ms, parsed in %u ms, laid out in %u ms, "
	       "%d images in %u + %u ms, %d px, %u ms in all\n",
	       rd.book->spine[i], html_size, read_ms, parse_ms, layout_ms, images, decode_ms,
	       relayout_ms, page_height(p), lv_tick_elaps(start));
	return p;
}

/* Pages */

static int32_t page_end(int index)
{
	return index + 1 < rd.pages ? rd.starts[index + 1] : rd.height;
}

/* The page that has row y */
static int page_at(int32_t y)
{
	int i = 0;
	while (i + 1 < rd.pages && rd.starts[i + 1] <= y)
		i++;
	return i;
}

/* A page on that side of the current one, in this chapter or another */
static bool page_exists(int dir)
{
	int i = rd.index + dir;
	return (i >= 0 && i < rd.pages) || (i < 0 && rd.spine > 0) ||
	       (i >= rd.pages && rd.spine + 1 < rd.book->spine_count);
}

static void slot_set(struct slot *s, int index)
{
	ui_texture_delete(s->tex);
	s->tex = 0;
	s->index = index;
	s->drawn = false;
	lv_obj_set_hidden(s->quad, true);
}

/* The page's paper: white, or dark in the dark theme (page_set_dark()) */
static uint32_t paper(void)
{
	return ui_light ? 0xffffff : PAGE_DARK_PAPER;
}

/* `rows` rows of the page buffer from row y in the paper's color */
static void paper_fill(int32_t y, int32_t rows)
{
	uint32_t c = paper();
	const uint8_t px[4] = { c & 0xff, c >> 8 & 0xff, c >> 16 & 0xff, 0xff };
	for (size_t i = (size_t)y * rd.w; i < (size_t)(y + rows) * rd.w; i++)
		memcpy(rd.buf + i * 4, px, 4);
}

/* The slot's page drawn, if not done yet; another chapter's stays blank */
static void slot_draw(struct slot *s)
{
	if (s->drawn)
		return;
	s->drawn = true;
	if (s->index < 0 || s->index >= rd.pages || !rd.buf)
		return;
	uint32_t start = lv_tick_get();
	int32_t y = rd.starts[s->index], n = page_end(s->index) - y;
	size_t row = (size_t)rd.w * 4;
	/* The text between the margins; below it, the next page's first lines are not
	 * this one's */
	paper_fill(0, MARGIN);
	page_draw(rd.page, rd.buf + MARGIN * row, rd.w, rd.text_h, y);
	paper_fill(MARGIN + n, rd.h - MARGIN - n);
	s->tex = ui_texture_create(rd.buf, rd.w, rd.h);
	lv_3dtexture_set_src(s->quad, s->tex);
	lv_obj_set_hidden(s->quad, false);
	lv_obj_invalidate(s->quad);
	printf("books: page %d of %d drawn in %u ms\n", s->index + 1, rd.pages, lv_tick_elaps(start));
}

static void stage_layout(void)
{
	for (int k = 0; k < 3; k++)
		lv_obj_set_x(rd.slots[k].sheet, (k - 1) * rd.pitch + rd.offset);
}

static void slide_exec_cb(void *var, int32_t v)
{
	LV_UNUSED(var);
	rd.offset = v;
	stage_layout();
}

static void footer_update(void)
{
	int32_t y = rd.pages ? rd.starts[rd.index] : 0;
	/* The last entry starting before the upper third of the page */
	const char *where = "";
	for (int i = 0; i < rd.book->toc_count; i++) {
		const struct epub_toc *t = &rd.book->toc[i];
		if (t->spine < rd.spine || (t->spine == rd.spine && rd.anchors[i] <= y + rd.h / 3))
			where = t->label;
	}
	lv_label_set_text_static(rd.where, where);
	float in_chapter = rd.pages ? (float)rd.index / rd.pages : 0;
	ui_set_text_fmt(rd.percent, "%d%%", (int)((rd.spine + in_chapter) * 100 / rd.book->spine_count));
}

/* Shows page `index` of the chapter now, its neighbors are drawn after */
static void page_show(int index)
{
	lv_anim_delete(&rd, slide_exec_cb);
	rd.sliding = false;
	rd.index = index;
	for (int k = 0; k < 3; k++)
		slot_set(&rd.slots[k], index + k - 1);
	slot_draw(&rd.slots[1]);
	rd.offset = 0;
	stage_layout();
	footer_update();
	lv_timer_resume(rd.timer);
}

/* The neighbors, one per tick, never during a swipe (the UI waits for them) */
static void tick_cb(lv_timer_t *timer)
{
	if (rd.dragging || rd.sliding)
		return;
	for (int k = 2; k >= 0; k -= 2) {
		if (!rd.slots[k].drawn) {
			slot_draw(&rd.slots[k]);
			return;
		}
	}
	lv_timer_pause(timer);
}

/* Slid to a neighbor: it becomes the current page, the one beyond it is next to
 * draw; past the chapter's ends, the next or previous chapter is laid out */
static void slide_done_cb(lv_anim_t *a)
{
	LV_UNUSED(a);
	rd.sliding = false;
	int dir = rd.offset < 0 ? 1 : rd.offset > 0 ? -1 : 0;
	if (!dir)
		return;
	int index = rd.index + dir;
	if (index < 0 || index >= rd.pages) {
		chapter_go(rd.spine + dir, dir > 0 ? 0 : GO_END, NULL, false);
		return;
	}
	struct slot *s = rd.slots, old;
	if (dir > 0) {
		old = s[0];
		s[0] = s[1];
		s[1] = s[2];
		s[2] = old;
		slot_set(&s[2], index + 1);
	} else {
		old = s[2];
		s[2] = s[1];
		s[1] = s[0];
		s[0] = old;
		slot_set(&s[0], index - 1);
	}
	rd.index = index;
	rd.offset = 0;
	stage_layout();
	footer_update();
	if (lv_tick_elaps(rd.saved_at) >= SAVE_MS)
		position_save();
	lv_timer_resume(rd.timer);
}

static void slide_to(int32_t target)
{
	int32_t dist = LV_ABS(target - rd.offset);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &rd);
	lv_anim_set_values(&a, rd.offset, target);
	lv_anim_set_duration(&a, LV_MAX(SLIDE_MS * dist / rd.pitch, 100));
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_exec_cb(&a, slide_exec_cb);
	lv_anim_set_completed_cb(&a, slide_done_cb);
	rd.sliding = true;
	lv_anim_start(&a);
}

static void page_turn(int dir)
{
	if (!page_exists(dir))
		return;
	slot_draw(&rd.slots[1 + dir]);
	slide_to(-dir * rd.pitch);
}

/* A link at (x, y) of the stage followed, if there is one */
static bool link_follow(int32_t x, int32_t y)
{
	y -= MARGIN;
	if (!rd.pages || y < 0 || y >= page_end(rd.index) - rd.starts[rd.index])
		return false;
	char *url = page_click(rd.page, x, rd.starts[rd.index] + y);
	if (!url)
		return false;
	printf("books: link %s\n", url);
	if (!strncmp(url, URL_PREFIX, strlen(URL_PREFIX))) {
		char *fragment = NULL;
		char *path = epub_path("", url + strlen(URL_PREFIX), &fragment);
		int i = path ? epub_spine_find(rd.book, path) : -1;
		if (i >= 0) {
			ui_vibrate(15);
			chapter_go(i, 0, fragment, false);
		}
		free(path);
		free(fragment);
	}
	free(url);
	return true;
}

/* The pages follow the finger; let go, they slide to the nearest one, or the
 * next in the direction of a flick. A tap follows a link, or turns the page. */
static void stage_cb(lv_event_t *e)
{
	lv_point_t pt;
	lv_indev_get_point(lv_indev_active(), &pt);

	/* Nothing to turn while a chapter is about to be laid out */
	if (rd.load_timer) {
		rd.dragging = false;
		return;
	}
	switch (lv_event_get_code(e)) {
	case LV_EVENT_PRESSED:
		if (rd.sliding) {
			lv_anim_delete(&rd, slide_exec_cb);
			rd.sliding = false;
		}
		/* Not drawn yet (a quick second swipe): now rather than mid-swipe */
		slot_draw(&rd.slots[0]);
		slot_draw(&rd.slots[2]);
		rd.press = pt;
		rd.press_time = lv_tick_get();
		rd.start = rd.offset;
		rd.dragging = true;
		break;
	case LV_EVENT_PRESSING: {
		int32_t offset = rd.start + pt.x - rd.press.x;
		/* Nothing on that side: it resists */
		if ((offset > 0 && !page_exists(-1)) || (offset < 0 && !page_exists(1)))
			offset /= 3;
		rd.offset = offset;
		stage_layout();
		break;
	}
	case LV_EVENT_RELEASED:
	case LV_EVENT_PRESS_LOST: {
		if (!rd.dragging)
			break;
		rd.dragging = false;
		int32_t dx = pt.x - rd.press.x, dy = pt.y - rd.press.y;
		if (lv_event_get_code(e) == LV_EVENT_RELEASED && rd.start == 0 &&
		    LV_ABS(dx) < TAP && LV_ABS(dy) < TAP) {
			rd.offset = 0;
			stage_layout();
			lv_area_t a;
			lv_obj_get_coords(rd.stage, &a);
			if (!link_follow(pt.x - a.x1, pt.y - a.y1))
				page_turn(pt.x - a.x1 < rd.w / 3 ? -1 : 1);
			break;
		}
		bool flick = lv_tick_elaps(rd.press_time) < FLICK_MS && LV_ABS(dx) > FLICK;
		int32_t target = 0;
		if (page_exists(1) && (rd.offset < -rd.pitch / 4 || (flick && dx < 0)))
			target = -rd.pitch;
		else if (page_exists(-1) && (rd.offset > rd.pitch / 4 || (flick && dx > 0)))
			target = rd.pitch;
		if (target != rd.offset)
			slide_to(target);
		else
			slide_done_cb(NULL);
		break;
	}
	default:
		break;
	}
}

/* Chapter i laid out and cut into pages, if it is not the one there already */
static bool chapter_load(int i)
{
	if (i < 0 || i >= rd.book->spine_count)
		return false;
	if (i == rd.spine && rd.page && !rd.relayout)
		return true;
	struct page *p = chapter_layout(i);
	if (!p)
		return false;
	uint32_t start = lv_tick_get();
	int *starts = NULL;
	int pages = page_paginate(p, rd.text_h, &starts);
	if (!pages) {
		page_destroy(p);
		return false;
	}
	printf("books: %d pages in %u ms\n", pages, lv_tick_elaps(start));
	if (rd.page)
		page_destroy(rd.page);
	free(rd.starts);
	rd.page = p;
	rd.relayout = false;
	rd.spine = i;
	rd.height = page_height(p);
	rd.starts = starts;
	rd.pages = pages;
	for (int k = 0; k < rd.book->toc_count; k++) {
		const struct epub_toc *t = &rd.book->toc[k];
		int32_t a = t->spine == i && t->fragment ? page_anchor_y(p, t->fragment) : 0;
		rd.anchors[k] = a > 0 ? a : 0;
	}
	return true;
}

/* The chapter there at the page with row y (GO_END: its last page), or at the
 * element `fragment` if it has one */
static void chapter_show_at(int32_t y, const char *fragment)
{
	if (fragment) {
		int32_t a = page_anchor_y(rd.page, fragment);
		y = a > 0 ? a : 0;
	}
	page_show(y == GO_END ? rd.pages - 1 : page_at(y));
}

static void loading_end(void)
{
	if (rd.load_timer)
		lv_timer_delete(rd.load_timer);
	rd.load_timer = NULL;
	free(rd.load_fragment);
	rd.load_fragment = NULL;
	lv_obj_set_hidden(rd.loading, true);
}

/* "Loading..." on the screen: the chapter now (the UI waits for it) */
static void load_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	rd.load_timer = NULL; /* deleted after this, its only run */
	if (chapter_load(rd.load_spine)) {
		if (rd.load_permyriad >= 0) {
			page_show(page_at((int32_t)((int64_t)rd.height * rd.load_permyriad / 10000)));
		} else {
			chapter_show_at(rd.load_y, rd.load_fragment);
			position_save();
		}
	} else if (rd.page) {
		page_show(rd.index); /* back to the page that was there */
	}
	printf("books: chapter %d shown in %u ms\n", rd.load_spine + 1, lv_tick_elaps(rd.load_start));
	loading_end();
}

/* Chapter i laid out after "Loading..." shows (and after the screen slid in, if
 * screen_sliding: the UI waits for the layout), at y or
 * `fragment` (chapter_show_at()), or at permyriad of its height if >= 0 */
static void loading_start(int i, int32_t y, const char *fragment, int permyriad, bool screen_sliding)
{
	loading_end();
	rd.load_spine = i;
	rd.load_y = y;
	rd.load_fragment = fragment ? strdup(fragment) : NULL;
	rd.load_permyriad = permyriad;
	rd.load_start = lv_tick_get();
	/* On a blank page */
	for (int k = 0; k < 3; k++)
		lv_obj_set_hidden(rd.slots[k].quad, true);
	lv_obj_set_hidden(rd.loading, false);
	rd.load_timer = lv_timer_create(load_cb, (screen_sliding ? ui_anim_ms : 0) + LOADING_MS, NULL);
	lv_timer_set_repeat_count(rd.load_timer, 1);
}

/* Chapter i at the page with row y (GO_END: its last page), or at the element
 * `fragment` if it has one: laid out first if it is another (loading_start()) */
static void chapter_go(int i, int32_t y, const char *fragment, bool screen_sliding)
{
	if (i < 0 || i >= rd.book->spine_count)
		return;
	if (i != rd.spine || !rd.page) {
		loading_start(i, y, fragment, -1, screen_sliding);
		return;
	}
	chapter_show_at(y, fragment);
	position_save();
}

/* Contents */

static void contents_row_cb(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	ui_pop();
	chapter_go(rd.book->toc[i].spine, 0, rd.book->toc[i].fragment, true);
}

static void contents_open(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Contents", NULL, &body);
	lv_obj_set_style_pad_hor(body, 16, 0);
	lv_obj_set_style_pad_bottom(body, 32, 0);
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

	if (!rd.book->toc_count)
		ui_empty_label(body, "No contents");
	lv_obj_t *card = rd.book->toc_count ? ui_card_create(body) : NULL;
	lv_obj_t *current = NULL;
	const char *where = lv_label_get_text(rd.where);
	for (int i = 0; i < rd.book->toc_count; i++) {
		lv_obj_t *row = ui_row_create(card, 112, i + 1 < rd.book->toc_count, contents_row_cb,
		                              (void *)(intptr_t)i);
		lv_obj_set_style_pad_hor(row, 24, 0);
		lv_obj_t *label = ui_one_line_label(row, rd.book->toc[i].label, &ui_font_26);
		lv_obj_set_flex_grow(label, 1);
		if (rd.book->toc[i].label == where) {
			lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_ACCENT), 0);
			current = row;
		}
	}
	ui_push(scr);
	if (current) {
		lv_obj_update_layout(scr);
		lv_obj_scroll_to_view(current, LV_ANIM_OFF);
	}
}

/* Text size */

/* The step nearest to px */
static int font_step(int px)
{
	int best = 0;
	for (int i = 1; i < FONT_STEPS; i++)
		if (abs(font_steps[i] - px) < abs(font_steps[best] - px))
			best = i;
	return best;
}

/* A step smaller or larger (dir -1, 1): kept, the chapter laid out again at the
 * same place */
static void size_cb(lv_event_t *e)
{
	int dir = (int)(intptr_t)lv_event_get_user_data(e);
	int step = font_step(rd.font_px) + dir;
	if (step < 0 || step >= FONT_STEPS)
		return;
	ui_vibrate(15);
	rd.font_px = font_steps[step];
	ui_setting_set_int(FONT_KEY, rd.font_px);
	ui_set_text_fmt(rd.size_label, "%d", rd.font_px);
	/* One about to be laid out takes the size as it is */
	if (rd.load_timer || !rd.page || !rd.pages)
		return;
	rd.relayout = true;
	int permyriad = (int)((int64_t)rd.starts[rd.index] * 10000 / (rd.height > 0 ? rd.height : 1));
	loading_start(rd.spine, 0, NULL, permyriad, false);
}

static void size_open(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_set_hidden(rd.size_bar, !lv_obj_is_hidden(rd.size_bar));
}

/* Over the bottom of the page: smaller, the size, larger */
static void size_bar_create(lv_obj_t *body)
{
	rd.size_bar = ui_bar_create(body);
	lv_obj_set_floating(rd.size_bar, true);
	lv_obj_align(rd.size_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_flex_align(rd.size_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(rd.size_bar, 24, 0);
	ui_button(rd.size_bar, 120, 80, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, LV_SYMBOL_MINUS, size_cb,
	          (void *)(intptr_t)-1);
	rd.size_label = lv_label_create(rd.size_bar);
	lv_obj_set_style_text_font(rd.size_label, &ui_font_32, 0);
	ui_set_text_fmt(rd.size_label, "%d", rd.font_px);
	ui_button(rd.size_bar, 120, 80, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, LV_SYMBOL_PLUS, size_cb,
	          (void *)(intptr_t)1);
	lv_obj_set_hidden(rd.size_bar, true);
}

static void reader_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	position_save();
	lv_anim_delete(&rd, NULL);
	if (rd.load_timer)
		lv_timer_delete(rd.load_timer);
	free(rd.load_fragment);
	lv_timer_delete(rd.timer);
	for (int k = 0; k < 3; k++)
		ui_texture_delete(rd.slots[k].tex);
	if (rd.page)
		page_destroy(rd.page);
	epub_close(rd.book);
	free(rd.file);
	free(rd.starts);
	free(rd.anchors);
	free(rd.buf);
	memset(&rd, 0, sizeof(rd));
}

static void reader_open(const char *file)
{
	char path[512];
	snprintf(path, sizeof(path), BOOKS_DIR "/%s", file);
	uint32_t start = lv_tick_get();
	struct epub *book = epub_open(path);
	if (!book)
		return;
	printf("books: %s opened in %u ms: %d chapters, %d entries in its contents\n", file,
	       lv_tick_elaps(start), book->spine_count, book->toc_count);
	memset(&rd, 0, sizeof(rd));
	rd.book = book;
	rd.file = strdup(file);
	rd.anchors = calloc(book->toc_count + 1, sizeof(*rd.anchors));
	rd.font_px = font_steps[font_step(ui_setting_get_int(FONT_KEY, FONT_PX))];

	lv_obj_t *header, *body;
	rd.scr = ui_app_screen_create(ui_intern(*book->title ? book->title : file), &header, &body);
	lv_obj_add_event_cb(rd.scr, reader_delete_cb, LV_EVENT_DELETE, NULL);
	ui_header_button(header, LV_SYMBOL_LIST, contents_open, NULL);
	ui_header_button(header, "Aa", size_open, NULL);
	lv_obj_set_scrollable(body, false);

	/* The page, its neighbors on each side; swipes and taps anywhere on it */
	rd.stage = lv_obj_create(body);
	lv_obj_remove_style_all(rd.stage);
	lv_obj_set_width(rd.stage, LV_PCT(100));
	lv_obj_set_flex_grow(rd.stage, 1);
	lv_obj_set_scrollable(rd.stage, false);
	lv_obj_set_clickable(rd.stage, true);
	lv_obj_add_event_cb(rd.stage, stage_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(rd.stage, stage_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(rd.stage, stage_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(rd.stage, stage_cb, LV_EVENT_PRESS_LOST, NULL);

	lv_obj_t *footer = ui_box(body);
	lv_obj_set_size(footer, LV_PCT(100), FOOTER_H);
	lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(footer, 24, 0);
	lv_obj_set_style_pad_column(footer, 16, 0);
	rd.where = ui_one_line_label(footer, "", &ui_font_20);
	lv_obj_set_flex_grow(rd.where, 1);
	lv_obj_set_style_text_color(rd.where, lv_color_hex(UI_COLOR_DIM), 0);
	rd.percent = ui_dim_label(footer, "", &ui_font_20);
	size_bar_create(body);

	ui_push(rd.scr);
	lv_obj_update_layout(rd.scr);
	rd.w = lv_obj_get_width(rd.stage);
	rd.h = lv_obj_get_height(rd.stage);
	rd.text_h = rd.h - 2 * MARGIN;
	rd.pitch = rd.w + PAGE_GAP;
	for (int k = 0; k < 3; k++) {
		struct slot *s = &rd.slots[k];
		s->sheet = lv_obj_create(rd.stage);
		lv_obj_remove_style_all(s->sheet);
		lv_obj_set_size(s->sheet, rd.w, rd.h);
		lv_obj_set_style_bg_opa(s->sheet, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(s->sheet, lv_color_hex(paper()), 0);
		lv_obj_set_clickable(s->sheet, false);
		lv_obj_set_scrollable(s->sheet, false);
		s->quad = lv_3dtexture_create(s->sheet);
		lv_obj_set_size(s->quad, rd.w, rd.h);
		lv_obj_set_clickable(s->quad, false);
		lv_obj_set_hidden(s->quad, true);
	}
	rd.loading = lv_label_create(rd.stage);
	lv_label_set_text_static(rd.loading, "Loading...");
	lv_obj_set_style_text_font(rd.loading, &ui_font_26, 0);
	lv_obj_set_style_text_color(rd.loading, lv_color_hex(0x8e8e93), 0);
	lv_obj_center(rd.loading);
	lv_obj_set_hidden(rd.loading, true);
	rd.buf = malloc((size_t)rd.w * rd.h * 4);
	rd.timer = lv_timer_create(tick_cb, DRAW_MS, NULL);
	if (!rd.buf || !rd.anchors) {
		printf("books: out of memory\n");
		return;
	}

	int spine = 0, permyriad = 0;
	if (!position_load(file, &spine, &permyriad) || spine < 0 || spine >= book->spine_count)
		spine = permyriad = 0;
	/* At the page where it was left */
	loading_start(spine, 0, NULL, permyriad, true);
}

/* Library */

#define COLUMNS 3

static void book_cb(lv_event_t *e)
{
	reader_open(((struct item *)lv_event_get_user_data(e))->file);
}

static int item_compare(const void *a, const void *b)
{
	const struct item *x = a, *y = b;
	return strcasecmp(x->title, y->title);
}

static void library_load(void)
{
	DIR *dir = opendir(BOOKS_DIR);
	struct dirent *d;
	while (dir && (d = readdir(dir))) {
		size_t n = strlen(d->d_name);
		if (d->d_name[0] == '.' || n < 5 || strcasecmp(d->d_name + n - 5, ".epub"))
			continue;
		struct item *items = realloc(lib.items, (lib.count + 1) * sizeof(*items));
		if (!items)
			break;
		lib.items = items;
		char path[512];
		snprintf(path, sizeof(path), BOOKS_DIR "/%s", d->d_name);
		struct epub *book = epub_open(path);
		struct item *it = &lib.items[lib.count++];
		memset(it, 0, sizeof(*it));
		it->file = strdup(d->d_name);
		it->ok = book != NULL;
		it->title = strdup(book && *book->title ? book->title : d->d_name);
		it->author = strdup(book ? book->author : "Cannot be read");
		it->cover_done = !book || !book->cover;
		epub_close(book);
	}
	if (dir)
		closedir(dir);
	if (lib.count)
		qsort(lib.items, lib.count, sizeof(*lib.items), item_compare);
}

/* A w x h image cropped to the middle of it as wide as bw x bh (filled), then
 * averaged down (or repeated up) to bw x bh: malloc'd */
static uint8_t *cover_fit(const uint8_t *px, int w, int h, int bw, int bh)
{
	uint8_t *out = malloc((size_t)bw * bh * 4);
	if (!out)
		return NULL;
	float s = ui_fit_scale(w, h, bw, bh, true);
	float x0 = (w - bw / s) / 2, y0 = (h - bh / s) / 2;
	uint8_t *o = out;
	for (int y = 0; y < bh; y++) {
		int sy0 = y0 + y / s, sy1 = y0 + (y + 1) / s;
		if (sy1 > h)
			sy1 = h;
		if (sy1 <= sy0)
			sy1 = sy0 + 1;
		for (int x = 0; x < bw; x++, o += 4) {
			int sx0 = x0 + x / s, sx1 = x0 + (x + 1) / s;
			if (sx1 > w)
				sx1 = w;
			if (sx1 <= sx0)
				sx1 = sx0 + 1;
			uint32_t sum[4] = { 0 }, n = (sy1 - sy0) * (sx1 - sx0);
			for (int sy = sy0; sy < sy1; sy++) {
				const uint8_t *p = px + ((size_t)sy * w + sx0) * 4;
				for (int sx = sx0; sx < sx1; sx++, p += 4)
					for (int c = 0; c < 4; c++)
						sum[c] += p[c];
			}
			for (int c = 0; c < 4; c++)
				o[c] = sum[c] / n;
		}
	}
	return out;
}

/* The book's cover as lib.cover_w x lib.cover_h BGRA (malloc'd), NULL if it has none
 * that can be read: JPEGs decoded scaled down already */
static uint8_t *cover_read(const char *file)
{
	char path[512];
	snprintf(path, sizeof(path), BOOKS_DIR "/%s", file);
	struct epub *book = epub_open(path);
	size_t size = 0;
	char *data = book && book->cover ? epub_read(book, book->cover, &size) : NULL;
	epub_close(book);
	if (!data)
		return NULL;
	uint8_t *px = NULL;
	int w = 0, h = 0, nw, nh;
	if (size >= 3 && !memcmp(data, "\xff\xd8\xff", 3)) {
		FILE *f = fmemopen(data, size, "rb");
		px = f ? ui_jpeg_read(f, lib.cover_w, lib.cover_h, true, &w, &h) : NULL;
		if (f)
			fclose(f);
	} else {
		px = browser_image_decode(data, size, &w, &h, &nw, &nh);
	}
	free(data);
	uint8_t *out = px && w > 0 && h > 0 ? cover_fit(px, w, h, lib.cover_w, lib.cover_h) : NULL;
	free(px);
	return out;
}

/* The next book's cover, in the order shown; stops once they are all done */
static void cover_cb(lv_timer_t *timer)
{
	for (int i = 0; i < lib.count; i++) {
		struct item *it = &lib.items[i];
		if (it->cover_done)
			continue;
		it->cover_done = true;
		uint32_t start = lv_tick_get();
		uint8_t *px = cover_read(it->file);
		it->tex = px ? ui_tile_texture(px, lib.cover_w, lib.cover_h, lib.cover_w, 0) : 0;
		free(px);
		printf("books: cover of %s %s in %u ms\n", it->file, it->tex ? "shown" : "unreadable",
		       lv_tick_elaps(start));
		if (it->tex) {
			/* set_src() does not redraw it */
			lv_3dtexture_set_src(it->quad, it->tex);
			lv_obj_invalidate(it->quad);
			lv_obj_set_hidden(it->symbol, true);
		}
		return;
	}
	lv_timer_pause(timer);
}

static void library_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (lib.timer)
		lv_timer_delete(lib.timer);
	for (int i = 0; i < lib.count; i++) {
		free(lib.items[i].file);
		free(lib.items[i].title);
		free(lib.items[i].author);
		ui_texture_delete(lib.items[i].tex);
	}
	ui_texture_delete(lib.placeholder);
	free(lib.items);
	memset(&lib, 0, sizeof(lib));
}

/* A book's tile: its cover (the placeholder with the app's symbol until it is
 * there, or without one), its title on two lines and its author */
static void tile_create(lv_obj_t *grid, struct item *it, int32_t tile_w)
{
	lv_obj_t *text;
	ui_tile_create(grid, tile_w, lib.cover_h, it->ok ? book_cb : NULL, it, &it->quad, &text);
	lv_3dtexture_set_src(it->quad, lib.placeholder);
	it->symbol = ui_dim_label(it->quad, LV_SYMBOL_FILE, &ui_font_48);
	lv_obj_center(it->symbol);

	lv_obj_t *title = lv_label_create(text);
	lv_label_set_text_static(title, it->title);
	lv_obj_set_style_text_font(title, &ui_font_20, 0);
	/* Wrapped, cut after the second line (the label draws within itself) */
	lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_size(title, LV_PCT(100), 2 * lv_font_get_line_height(&ui_font_20));
	lv_obj_t *author = ui_one_line_label(text, it->author, &ui_font_16);
	lv_obj_set_width(author, LV_PCT(100));
	lv_obj_set_style_text_color(author, lv_color_hex(UI_COLOR_DIM), 0);
}

void books_open(void)
{
	uint32_t start = lv_tick_get();
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Books", NULL, &body);
	lv_obj_add_event_cb(scr, library_delete_cb, LV_EVENT_DELETE, NULL);

	library_load();
	if (!lib.count) {
		lv_obj_set_style_pad_hor(body, 16, 0);
		ui_empty_label(body, "No books");
		lv_obj_t *hint = ui_dim_label(body, "Copy ePub files to " BOOKS_DIR, &ui_font_20);
		lv_obj_set_width(hint, LV_PCT(100));
		lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_margin_top(hint, 16, 0);
	}

	lv_obj_t *grid = ui_tile_grid_create(body);
	/* Covers 2:3, as most are */
	int32_t tile_w = ui_tile_width(COLUMNS);
	lib.cover_w = tile_w;
	lib.cover_h = lib.cover_w * 3 / 2;
	if (lib.count)
		lib.placeholder = ui_tile_texture(NULL, lib.cover_w, lib.cover_h, lib.cover_w, UI_COLOR_BUTTON);
	for (int i = 0; i < lib.count; i++)
		tile_create(grid, &lib.items[i], tile_w);
	lib.timer = lv_timer_create(cover_cb, 10, NULL);
	ui_push(scr);
	printf("books: %d books listed in %u ms\n", lib.count, lv_tick_elaps(start));
}

#else

void books_open(void)
{
	printf("books: needs OpenGL ES (lv_3dtexture)\n");
}

#endif
