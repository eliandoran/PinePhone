/*
 * Books: ePub files in /var/lib/books (`just books`), listed with their title
 * and author, read a chapter (a file of the book's spine) at a time.
 *
 * A chapter is laid out by the browser's litehtml page (browser/page.cpp) with
 * the book's style sheets, read from the zip (epub.c) while it is parsed, then
 * laid out again with its images if it has any, before it is shown, and drawn in tiles as wide as the screen like the browser's: GL textures
 * in a scrolling view, only those near the view, a few per timer tick. A tap in
 * the upper third goes a screen back, elsewhere a screen on, into the next or
 * previous chapter at the ends; links within the book are followed. The footer
 * has the table of contents' entry the reader is in and how far into the book.
 * The place in each book is kept in /var/lib/books/.positions.
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
#define TILE_H 512
#define KEEP_TILES 3         /* textures kept beyond the view on each side */
#define DRAW_BUDGET_MS 12    /* tiles drawn per tick */
#define TICK_MS 16
#define OVERLAP 48           /* of a screen still shown after turning it */
#define IMAGES_BYTES (96 << 20) /* decoded images per chapter */
#define FOOTER_H 56
#define SAVE_MS 5000         /* the position saved at most this often while scrolling */
#define GO_END -1            /* chapter_go(): at the end of the chapter */

struct tile {
	lv_obj_t *quad; /* NULL: not drawn */
	unsigned tex;
};

/* The reader */
static struct {
	struct epub *book;
	char *file; /* its name in BOOKS_DIR */
	lv_obj_t *view, *doc, *where, *percent;
	lv_timer_t *timer;
	int spine;  /* the chapter shown */
	struct page *page;
	int32_t height; /* the page's, at least the view's */
	int32_t *anchors; /* where the table of contents' entries of this chapter start */
	struct tile *tiles;
	int tile_count;
	uint8_t *buf; /* a tile's pixels */
	int32_t w, h; /* the view */
	int32_t shown_y;  /* the footer is for this scroll position */
	uint32_t saved_at;
} rd;

/* The library */
struct item {
	char *file, *title, *author; /* title: the file's name if it cannot be read */
	bool ok;
};

static struct {
	struct item *items;
	int count;
} lib;

/* Positions: a line per book, "chapter permyriad file" */

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
	if (!rd.book || !rd.page)
		return;
	int32_t y = lv_obj_get_scroll_y(rd.view);
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

/* Margins around the book's own (on <html>, which books leave alone), in a style
 * before its <body>: html is replaced (malloc'd) */
static char *page_margins(char *html, size_t *size)
{
	static const char style[] = "<style>html { padding: 4px 12px }</style>";
	char *body = strcasestr(html, "<body");
	char *out = body ? malloc(*size + sizeof(style)) : NULL;
	if (!out)
		return html;
	size_t before = body - html;
	memcpy(out, html, before);
	memcpy(out + before, style, sizeof(style) - 1);
	memcpy(out + before + sizeof(style) - 1, body, *size - before + 1);
	*size += sizeof(style) - 1;
	free(html);
	return out;
}

static struct page *chapter_layout(int i)
{
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
	uint32_t start = lv_tick_get();
	struct page *p = page_create_local(html, size, url, rd.w, rd.h, css_read, NULL);
	free(html);
	free(url);

	const char *image_url;
	size_t image_bytes = 0;
	bool images = false;
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
		images |= px != NULL;
	}
	if (images)
		page_relayout(p);
	printf("books: %s laid out in %u ms, %d px\n", rd.book->spine[i], lv_tick_elaps(start), page_height(p));
	return p;
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
	for (int i = 0; i < rd.tile_count; i++)
		tile_free(&rd.tiles[i]);
	free(rd.tiles);
	rd.tiles = NULL;
	rd.tile_count = 0;
}

static void tile_draw(int i)
{
	struct tile *t = &rd.tiles[i];
	page_draw(rd.page, rd.buf, rd.w, TILE_H, i * TILE_H);
	unsigned tex = ui_texture_create(rd.buf, rd.w, TILE_H);
	if (!t->quad) {
		t->quad = lv_3dtexture_create(rd.doc);
		lv_obj_set_clickable(t->quad, false);
		lv_obj_set_size(t->quad, rd.w, TILE_H);
		lv_obj_set_pos(t->quad, 0, i * TILE_H);
	}
	lv_3dtexture_set_src(t->quad, tex);
	ui_texture_delete(t->tex);
	t->tex = tex;
	lv_obj_invalidate(t->quad);
}

static void tiles_update(void)
{
	if (!rd.page || !rd.tile_count)
		return;
	int32_t y = lv_obj_get_scroll_y(rd.view);
	int first = y / TILE_H, last = (y + rd.h - 1) / TILE_H;
	for (int i = 0; i < rd.tile_count; i++)
		if (rd.tiles[i].quad && (i < first - KEEP_TILES || i > last + KEEP_TILES))
			tile_free(&rd.tiles[i]);
	/* The view first, then below it (where reading goes), then above */
	uint32_t start = lv_tick_get();
	for (int k = 0; k <= last - first + 2 * KEEP_TILES; k++) {
		int i = k <= last - first ? first + k :
		        k <= last - first + KEEP_TILES ? last + (k - (last - first)) :
		        first - (k - (last - first) - KEEP_TILES);
		if (i < 0 || i >= rd.tile_count || rd.tiles[i].quad)
			continue;
		tile_draw(i);
		if (lv_tick_elaps(start) >= DRAW_BUDGET_MS)
			break;
	}
}

/* Reading */

static void footer_update(void)
{
	int32_t y = lv_obj_get_scroll_y(rd.view);
	rd.shown_y = y;
	/* The last entry starting before the upper third of the view */
	const char *where = "";
	for (int i = 0; i < rd.book->toc_count; i++) {
		const struct epub_toc *t = &rd.book->toc[i];
		if (t->spine < rd.spine || (t->spine == rd.spine && rd.anchors[i] <= y + rd.h / 3))
			where = t->label;
	}
	lv_label_set_text_static(rd.where, where);
	/* The top of the view: 0% at the start */
	float in_chapter = rd.height > 0 ? (float)y / rd.height : 0;
	ui_set_text_fmt(rd.percent, "%d%%", (int)((rd.spine + in_chapter) * 100 / rd.book->spine_count));
}

static void scroll_set(int32_t y)
{
	int32_t max = rd.height - rd.h;
	if (y > max)
		y = max;
	if (y < 0)
		y = 0;
	lv_obj_scroll_to_y(rd.view, y, LV_ANIM_OFF);
	tiles_update();
	footer_update();
}

/* Chapter i (laid out if it is not the one shown) at y (GO_END: its end), or at
 * the element `fragment` if it has one */
static void chapter_go(int i, int32_t y, const char *fragment)
{
	if (i < 0 || i >= rd.book->spine_count)
		return;
	if (i != rd.spine || !rd.page) {
		struct page *p = chapter_layout(i);
		if (!p)
			return;
		tiles_clear();
		if (rd.page)
			page_destroy(rd.page);
		rd.page = p;
		rd.spine = i;
		rd.height = page_height(p) > rd.h ? page_height(p) : rd.h;
		rd.tile_count = (rd.height + TILE_H - 1) / TILE_H;
		rd.tiles = calloc(rd.tile_count, sizeof(*rd.tiles));
		if (!rd.tiles)
			rd.tile_count = 0;
		lv_obj_set_height(rd.doc, rd.height);
		lv_obj_update_layout(rd.view);
		for (int k = 0; k < rd.book->toc_count; k++) {
			const struct epub_toc *t = &rd.book->toc[k];
			int32_t a = t->spine == i && t->fragment ? page_anchor_y(p, t->fragment) : 0;
			rd.anchors[k] = a > 0 ? a : 0;
		}
	}
	if (fragment) {
		int32_t a = page_anchor_y(rd.page, fragment);
		y = a > 0 ? a : 0;
	}
	scroll_set(y == GO_END ? rd.height : y);
	position_save();
}

static void page_turn(bool forward)
{
	int32_t y = lv_obj_get_scroll_y(rd.view), max = rd.height - rd.h;
	if (forward && y >= max - 1)
		chapter_go(rd.spine + 1, 0, NULL);
	else if (!forward && y <= 0)
		chapter_go(rd.spine - 1, GO_END, NULL);
	else
		scroll_set(forward ? y + rd.h - OVERLAP : y - (rd.h - OVERLAP));
}

static void view_clicked_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!rd.page)
		return;
	lv_point_t p;
	lv_area_t a;
	lv_indev_get_point(lv_indev_active(), &p);
	lv_obj_get_coords(rd.view, &a);
	char *url = page_click(rd.page, p.x - a.x1, p.y - a.y1 + lv_obj_get_scroll_y(rd.view));
	if (url) {
		printf("books: link %s\n", url);
		if (!strncmp(url, URL_PREFIX, strlen(URL_PREFIX))) {
			char *fragment = NULL;
			char *path = epub_path("", url + strlen(URL_PREFIX), &fragment);
			int i = path ? epub_spine_find(rd.book, path) : -1;
			if (i >= 0) {
				ui_vibrate(15);
				chapter_go(i, 0, fragment);
			}
			free(path);
			free(fragment);
		}
		free(url);
		return;
	}
	page_turn(p.y - a.y1 >= rd.h / 3);
}

static void scroll_end_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (lv_tick_elaps(rd.saved_at) >= SAVE_MS)
		position_save();
}

static void tick_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	tiles_update();
	if (rd.page && lv_obj_get_scroll_y(rd.view) != rd.shown_y)
		footer_update();
}

/* Contents */

static void contents_row_cb(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	ui_pop();
	chapter_go(rd.book->toc[i].spine, 0, rd.book->toc[i].fragment);
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

static void reader_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	position_save();
	lv_timer_delete(rd.timer);
	tiles_clear();
	if (rd.page)
		page_destroy(rd.page);
	epub_close(rd.book);
	free(rd.file);
	free(rd.anchors);
	free(rd.buf);
	memset(&rd, 0, sizeof(rd));
}

static void reader_open(const char *file)
{
	char path[512];
	snprintf(path, sizeof(path), BOOKS_DIR "/%s", file);
	struct epub *book = epub_open(path);
	if (!book)
		return;
	memset(&rd, 0, sizeof(rd));
	rd.book = book;
	rd.file = strdup(file);
	rd.anchors = calloc(book->toc_count + 1, sizeof(*rd.anchors));

	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create(ui_intern(*book->title ? book->title : file), &header, &body);
	lv_obj_add_event_cb(scr, reader_delete_cb, LV_EVENT_DELETE, NULL);
	ui_header_button(header, LV_SYMBOL_LIST, contents_open, NULL);

	rd.view = lv_obj_create(body);
	lv_obj_remove_style_all(rd.view);
	lv_obj_set_width(rd.view, LV_PCT(100));
	lv_obj_set_flex_grow(rd.view, 1);
	lv_obj_set_style_bg_opa(rd.view, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(rd.view, lv_color_white(), 0);
	lv_obj_set_scroll_dir(rd.view, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(rd.view, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(rd.view, view_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(rd.view, scroll_end_cb, LV_EVENT_SCROLL_END, NULL);
	rd.doc = ui_box(rd.view);
	lv_obj_set_width(rd.doc, LV_PCT(100));

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

	ui_push(scr);
	lv_obj_update_layout(scr);
	rd.w = lv_obj_get_width(rd.view);
	rd.h = lv_obj_get_height(rd.view);
	rd.buf = malloc((size_t)rd.w * TILE_H * 4);
	rd.timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	if (!rd.buf || !rd.anchors) {
		printf("books: out of memory\n");
		return;
	}

	int spine = 0, permyriad = 0;
	if (!position_load(file, &spine, &permyriad) || spine < 0 || spine >= book->spine_count)
		spine = permyriad = 0;
	chapter_go(spine, 0, NULL);
	scroll_set((int32_t)((int64_t)rd.height * permyriad / 10000));
}

/* Library */

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
		it->file = strdup(d->d_name);
		it->ok = book != NULL;
		it->title = strdup(book && *book->title ? book->title : d->d_name);
		it->author = strdup(book ? book->author : "Cannot be read");
		epub_close(book);
	}
	if (dir)
		closedir(dir);
	if (lib.count)
		qsort(lib.items, lib.count, sizeof(*lib.items), item_compare);
}

static void library_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	for (int i = 0; i < lib.count; i++) {
		free(lib.items[i].file);
		free(lib.items[i].title);
		free(lib.items[i].author);
	}
	free(lib.items);
	memset(&lib, 0, sizeof(lib));
}

void books_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Books", NULL, &body);
	lv_obj_add_event_cb(scr, library_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_style_pad_hor(body, 16, 0);
	lv_obj_set_style_pad_bottom(body, 32, 0);
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

	library_load();
	if (!lib.count) {
		ui_empty_label(body, "No books");
		lv_obj_t *hint = ui_dim_label(body, "Copy ePub files to " BOOKS_DIR, &ui_font_20);
		lv_obj_set_width(hint, LV_PCT(100));
		lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_margin_top(hint, 16, 0);
	}
	lv_obj_t *card = lib.count ? ui_card_create(body) : NULL;
	for (int i = 0; i < lib.count; i++) {
		struct item *it = &lib.items[i];
		lv_obj_t *row = ui_row_create(card, 120, i + 1 < lib.count, it->ok ? book_cb : NULL, it);
		lv_obj_set_style_pad_hor(row, 24, 0);
		lv_obj_t *text = ui_row_text(row);
		lv_obj_t *title = ui_one_line_label(text, it->title, &ui_font_26);
		lv_obj_set_width(title, LV_PCT(100));
		lv_obj_t *author = ui_one_line_label(text, it->author, &ui_font_20);
		lv_obj_set_width(author, LV_PCT(100));
		lv_obj_set_style_text_color(author, lv_color_hex(UI_COLOR_DIM), 0);
	}
	ui_push(scr);
}

#else

void books_open(void)
{
	printf("books: needs OpenGL ES (lv_3dtexture)\n");
}

#endif
