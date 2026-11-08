/*
 * Books (books/): ePub files in /var/lib/books, listed with their title and
 * author, read a page at a time. Pages are drawn into textures, not labels:
 * the tests check the header, the footer (the table of contents' entry the reader
 * is at), and pixels.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <zlib.h>

#include "apps/books/books.h"
#include "apps/browser/browser.h"
#include "ui.h"
#include "test.h"
#include "ui_test.h"

#define BOOKS "/var/lib/books"
/* Taps in the reader: on its left third a page back, elsewhere a page on */
#define BACK_X 100
#define FORWARD_X 600

/* In the light theme: white pages (the dark one: dark_pages_in_the_dark_theme) */
TEST_SETUP
{
	ui_test_reset();
	CHECK(ui_test_write("/var/lib/phone-ui/settings", "light=1\n"));
	ui_test_start();
}

/* A zip: entries deflated unless `stored`, `size` bytes of data (0: a string); a broken
 * one: its data said to be deflated (NOT_DEFLATED, stored as it is) or longer than the
 * file (CUT_SHORT, stored, the last entry) */
enum { NOT_DEFLATED = 1, CUT_SHORT };
struct zip_file {
	const char *name, *data;
	bool stored;
	size_t size;
	int broken;
};

static void put16(FILE *f, unsigned v)
{
	fputc(v & 0xff, f);
	fputc(v >> 8 & 0xff, f);
}

static void put32(FILE *f, unsigned long v)
{
	put16(f, v & 0xffff);
	put16(f, v >> 16 & 0xffff);
}

static bool zip_write(const char *path, const struct zip_file *files, int count)
{
	mkdir(BOOKS, 0755);
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	unsigned long offsets[16], csizes[16], crcs[16];
	unsigned methods[16];
	unsigned char *packed[16];
	size_t sizes[16];
	for (int i = 0; i < count; i++) {
		size_t n = sizes[i] = files[i].size ? files[i].size : strlen(files[i].data);
		crcs[i] = crc32(0, (const Bytef *)files[i].data, n);
		packed[i] = malloc(n + 64);
		bool stored = files[i].stored || files[i].broken;
		if (stored) {
			memcpy(packed[i], files[i].data, n);
			csizes[i] = n;
		} else {
			z_stream z = { .next_in = (Bytef *)files[i].data, .avail_in = n,
			               .next_out = packed[i], .avail_out = n + 64 };
			deflateInit2(&z, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
			deflate(&z, Z_FINISH);
			csizes[i] = z.total_out;
			deflateEnd(&z);
		}
		methods[i] = files[i].stored || files[i].broken == CUT_SHORT ? 0 : 8;
		size_t written = csizes[i];
		if (files[i].broken == CUT_SHORT)
			csizes[i] = sizes[i] = n + 100000;
		offsets[i] = ftell(f);
		put32(f, 0x04034b50);
		put16(f, 20);
		put16(f, 0);
		put16(f, methods[i]);
		put32(f, 0);
		put32(f, crcs[i]);
		put32(f, csizes[i]);
		put32(f, sizes[i]);
		put16(f, strlen(files[i].name));
		put16(f, 0);
		fputs(files[i].name, f);
		fwrite(packed[i], 1, written, f);
	}
	unsigned long dir = ftell(f);
	for (int i = 0; i < count; i++) {
		put32(f, 0x02014b50);
		put16(f, 20);
		put16(f, 20);
		put16(f, 0);
		put16(f, methods[i]);
		put32(f, 0);
		put32(f, crcs[i]);
		put32(f, csizes[i]);
		put32(f, sizes[i]);
		put16(f, strlen(files[i].name));
		put32(f, 0); /* extra, comment */
		put32(f, 0); /* disk, internal attributes */
		put32(f, 0); /* external attributes */
		put32(f, offsets[i]);
		fputs(files[i].name, f);
		free(packed[i]);
	}
	unsigned long dir_size = ftell(f) - dir;
	put32(f, 0x06054b50);
	put32(f, 0);
	put16(f, count);
	put16(f, count);
	put32(f, dir_size);
	put32(f, dir);
	put16(f, 0);
	return fclose(f) == 0;
}

#define CONTAINER "<?xml version=\"1.0\"?><container version=\"1.0\" " \
	"xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>" \
	"<rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>" \
	"</rootfiles></container>"

/* Two chapters, the second's file name with a space; a table of contents (EPUB 3
 * navigation document) with a link to each; chapter one is long, red (from the
 * style sheet in another folder), starting with a link to chapter two; chapter two
 * is blue */
static bool book_write(const char *name)
{
	static const struct zip_file files[] = {
		{ "mimetype", "application/epub+zip", true },
		{ "META-INF/container.xml", CONTAINER, false },
		{ "OEBPS/content.opf",
		  "<?xml version='1.0' encoding='utf-8'?>\n"
		  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
		  "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
		  "<dc:title>Test Book</dc:title><dc:creator id=\"a\">Ann Author</dc:creator></metadata>"
		  "<manifest>"
		  "<item href=\"nav.xhtml\" id=\"nav\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>"
		  "<item href=\"styles/book.css\" id=\"css\" media-type=\"text/css\"/>"
		  "<item href=\"text/one.xhtml\" id=\"c1\" media-type=\"application/xhtml+xml\"/>"
		  "<item href=\"text/two%20b.xhtml\" id=\"c2\" media-type=\"application/xhtml+xml\"/>"
		  "</manifest><spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine></package>",
		  false },
		{ "OEBPS/nav.xhtml",
		  "<?xml version='1.0' encoding='utf-8'?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" "
		  "xmlns:epub=\"http://www.idpf.org/2007/ops\"><body>"
		  "<nav epub:type=\"landmarks\"><ol><li><a href=\"text/two%20b.xhtml\">Landmark</a></li></ol></nav>"
		  "<nav epub:type=\"toc\"><ol>"
		  "<li><a href=\"text/one.xhtml\">Chapter One</a></li>"
		  "<li><a href=\"text/two%20b.xhtml#second\">Chapter <i>Two</i> &amp; more</a></li>"
		  "</ol></nav></body></html>",
		  false },
		{ "OEBPS/styles/book.css", ".red { background: #ff0000; height: 3000px; margin: 0 }\n"
		  "@media screen { .book { margin: 0 10% } }", false },
		{ "OEBPS/text/one.xhtml",
		  "<?xml version='1.0' encoding='utf-8'?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
		  "<title>One</title><link href=\"../styles/book.css\" rel=\"stylesheet\" type=\"text/css\"/>"
		  "</head><body class=\"book\">"
		  "<a href=\"two%20b.xhtml#second\" style=\"display: block; height: 100px\">Next</a>"
		  "<p class=\"red\">Red</p></body></html>",
		  false },
		{ "OEBPS/text/two b.xhtml",
		  "<?xml version='1.0' encoding='utf-8'?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
		  "<title>Two</title></head><body style=\"margin: 0\">"
		  "<div id=\"second\" style=\"background: #0000ff; height: 2000px\">Blue</div></body></html>",
		  false },
	};
	char path[256];
	snprintf(path, sizeof(path), BOOKS "/%s", name);
	return zip_write(path, files, sizeof(files) / sizeof(files[0]));
}

static bool is_color(lv_color_t c, uint32_t rgb)
{
	return c.red == (rgb >> 16 & 0xff) && c.green == (rgb >> 8 & 0xff) && c.blue == (rgb & 0xff);
}

static lv_color_t page_pixel(void)
{
	return ui_test_pixel(360, 800);
}

static void tap_page(int32_t x)
{
	ui_test_tap_at(x, 800);
	ui_test_wait(200);
	ui_test_settle();
}

static bool open_book(void)
{
	bool ok = ui_test_open("Books") && ui_test_tap_text("Test Book");
	ui_test_wait(200);
	ui_test_settle();
	return ok;
}

TEST(empty_library)
{
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_shows("No books"));
	CHECK(ui_test_shows("Copy ePub files to /var/lib/books")); /* its text kept, not on the stack */
}

TEST(books_listed_with_title_and_author)
{
	CHECK(book_write("test.epub"));
	CHECK(ui_test_write(BOOKS "/broken.epub", "not a zip"));
	CHECK(ui_test_write(BOOKS "/notes.txt", "not a book"));
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_shows("Test Book"));
	CHECK(ui_test_shows("Ann Author"));
	CHECK(ui_test_font("Test Book") == UI_FONT_BODY);
	CHECK(ui_test_font("Ann Author") == UI_FONT_CAPTION);
	/* What cannot be read is listed by its file name */
	CHECK(ui_test_shows("broken.epub"));
	CHECK(!ui_test_shows("notes.txt"));
	CHECK(!ui_test_shows("No books"));
}

/* A book with this OPF `metadata` and `manifest` (and one chapter), the file
 * `image_name` (in the zip) of `image_size` bytes if it is not NULL */
static bool cover_book_write(const char *name, const char *metadata, const char *manifest,
                             const char *image_name, const void *image, size_t image_size)
{
	char opf[1024];
	snprintf(opf, sizeof(opf),
	         "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\"><metadata "
	         "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">%s</metadata><manifest>%s"
	         "<item href=\"one.xhtml\" id=\"c1\" media-type=\"application/xhtml+xml\"/>"
	         "</manifest><spine><itemref idref=\"c1\"/></spine></package>",
	         metadata, manifest);
	const struct zip_file files[] = {
		{ "mimetype", "application/epub+zip", true, 0 },
		{ "META-INF/container.xml", CONTAINER, false, 0 },
		{ "OEBPS/content.opf", opf, false, 0 },
		{ "OEBPS/one.xhtml", "<html><body>Text</body></html>", false, 0 },
		{ image_name ? image_name : "OEBPS/none", image ? image : "none", true, image_size },
	};
	char path[256];
	snprintf(path, sizeof(path), BOOKS "/%s", name);
	return zip_write(path, files, sizeof(files) / sizeof(files[0]));
}

/* A JPEG of one color (0xRRGGBB), malloc'd */
static void *jpeg_of(uint32_t rgb, size_t *size)
{
	enum { W = 300, H = 450 };
	uint8_t *px = malloc(W * H * 4);
	for (int i = 0; i < W * H; i++) {
		px[i * 4] = rgb & 0xff;
		px[i * 4 + 1] = rgb >> 8 & 0xff;
		px[i * 4 + 2] = rgb >> 16 & 0xff;
		px[i * 4 + 3] = 0xff;
	}
	char *jpeg = NULL;
	FILE *f = open_memstream(&jpeg, size);
	ui_jpeg_write(f, px, W, H, W * 4, 90, true);
	fclose(f);
	free(px);
	return jpeg;
}

static bool near_color(lv_color_t c, uint32_t rgb)
{
	return abs(c.red - (int)(rgb >> 16 & 0xff)) < 24 && abs(c.green - (int)(rgb >> 8 & 0xff)) < 24 &&
	       abs(c.blue - (int)(rgb & 0xff)) < 24;
}

/* The library shows the books' covers: the EPUB 3 cover image, or the EPUB 2 one
 * (<meta name="cover">), JPEG or PNG; a book without one, a placeholder */
TEST(covers_shown_in_the_library)
{
	size_t jpeg_size, png_size;
	void *jpeg = jpeg_of(0x00c000, &jpeg_size);
	void *png = ui_test_png(200, 300, 0xff0000, &png_size);
	CHECK(cover_book_write("green.epub", "<dc:title>Green</dc:title><dc:creator>Gail</dc:creator>",
	                       "<item href=\"images/c.jpg\" id=\"img\" media-type=\"image/jpeg\" "
	                       "properties=\"cover-image\"/>",
	                       "OEBPS/images/c.jpg", jpeg, jpeg_size));
	CHECK(cover_book_write("red.epub", "<dc:title>Red</dc:title><meta name=\"cover\" content=\"pic\"/>",
	                       "<item href=\"red.png\" id=\"pic\" media-type=\"image/png\"/>",
	                       "OEBPS/red.png", png, png_size));
	CHECK(cover_book_write("plain.epub", "<dc:title>Plain</dc:title>", "", NULL, NULL, 0));
	free(jpeg);
	free(png);

	CHECK(ui_test_open("Books"));
	ui_test_wait(500);
	ui_test_settle();
	CHECK(ui_test_shows("Green"));
	CHECK(ui_test_shows("Gail"));
	CHECK(ui_test_shows("Red"));
	CHECK(ui_test_shows("Plain"));
	/* Two covers and the placeholder */
	lv_obj_t *covers[4];
	unsigned count = ui_test_find_widgets(&lv_3dtexture_class, covers, 4);
	CHECK_INT(count, 3);
	bool green = false, red = false;
	for (unsigned i = 0; i < count && i < 4; i++) {
		lv_area_t a, tile;
		lv_obj_get_coords(covers[i], &a);
		CHECK(lv_area_get_width(&a) > 150);
		/* Upright, as a book */
		CHECK(lv_area_get_height(&a) > lv_area_get_width(&a));
		lv_color_t c = ui_test_pixel((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
		if (!near_color(c, 0x00c000) && !near_color(c, 0xff0000))
			continue;
		green |= near_color(c, 0x00c000);
		red |= near_color(c, 0xff0000);
		/* Seamless: as wide as its card, at its top, the card's rounded corners cut
		 * out of it */
		lv_obj_get_coords(lv_obj_get_parent(covers[i]), &tile);
		CHECK_INT(a.x1, tile.x1);
		CHECK_INT(a.x2, tile.x2);
		CHECK_INT(a.y1, tile.y1);
		CHECK(near_color(ui_test_pixel(a.x1, a.y1 + 40), lv_color_to_u32(c) & 0xffffff));
		CHECK(near_color(ui_test_pixel(a.x1 + 40, a.y1), lv_color_to_u32(c) & 0xffffff));
		CHECK(!near_color(ui_test_pixel(a.x1, a.y1), lv_color_to_u32(c) & 0xffffff));
		CHECK(!near_color(ui_test_pixel(a.x2, a.y1), lv_color_to_u32(c) & 0xffffff));
	}
	CHECK(green);
	CHECK(red);

	/* A cover opens its book: a white page */
	CHECK(ui_test_tap_text("Green"));
	ui_test_wait(500);
	ui_test_settle();
	CHECK(is_color(page_pixel(), 0xffffff));
}

TEST(chapter_shown_with_its_style_sheet)
{
	CHECK(book_write("test.epub"));
	CHECK(open_book());
	CHECK(ui_test_shows("Test Book"));
	CHECK(ui_test_shows("Chapter One"));
	CHECK(is_color(page_pixel(), 0xff0000));
}

TEST(taps_turn_pages_into_the_next_chapter_and_back)
{
	CHECK(book_write("test.epub"));
	CHECK(open_book());
	int taps = 0;
	while (!ui_test_shows("Chapter Two & more") && taps++ < 10)
		tap_page(FORWARD_X);
	/* 150 + 4500 px: four pages of red, then chapter two */
	CHECK(taps >= 4 && taps <= 6);
	CHECK(is_color(page_pixel(), 0x0000ff));

	tap_page(BACK_X);
	CHECK(ui_test_shows("Chapter One"));
	/* At its end */
	CHECK(is_color(page_pixel(), 0xff0000));
}

/* Style sheets are read from the book as the chapter is parsed, @imports at any
 * depth (the browser downloads them: a few rounds, the page parsed again after each) */
TEST(style_sheets_imported_at_any_depth)
{
	static const struct zip_file files[] = {
		{ "mimetype", "application/epub+zip", true },
		{ "META-INF/container.xml", "<?xml version=\"1.0\"?><container version=\"1.0\" "
		  "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
		  "<rootfile full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/>"
		  "</rootfiles></container>", false },
		{ "content.opf",
		  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\"><metadata "
		  "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Deep</dc:title></metadata><manifest>"
		  "<item href=\"one.xhtml\" id=\"c1\" media-type=\"application/xhtml+xml\"/>"
		  "</manifest><spine><itemref idref=\"c1\"/></spine></package>",
		  false },
		{ "one.xhtml",
		  "<html><head><link href=\"css/a.css\" rel=\"stylesheet\"/></head>"
		  "<body style=\"margin: 0\"><p class=\"red\">Red</p></body></html>",
		  false },
		{ "css/a.css", "@import \"b/b.css\";", false },
		{ "css/b/b.css", "@import \"../c.css\";", false },
		{ "css/c.css", "@import url(d.css);", false },
		{ "css/d.css", "@import \"e.css\";", false },
		{ "css/e.css", ".red { background: #ff0000; height: 3000px; margin: 0 }", false },
	};
	CHECK(zip_write(BOOKS "/deep.epub", files, sizeof(files) / sizeof(files[0])));
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_tap_text("Deep"));
	ui_test_wait(200);
	ui_test_settle();
	CHECK(is_color(page_pixel(), 0xff0000));
}

TEST(link_followed_to_its_chapter)
{
	CHECK(book_write("test.epub"));
	CHECK(open_book());
	/* The link at the top of the page */
	ui_test_tap_at(360, 152 + 60);
	ui_test_wait(200);
	ui_test_settle();
	CHECK(ui_test_shows("Chapter Two & more"));
	CHECK(is_color(page_pixel(), 0x0000ff));
}

TEST(contents_go_to_a_chapter)
{
	CHECK(book_write("test.epub"));
	CHECK(open_book());
	CHECK(ui_test_tap_text(LV_SYMBOL_LIST));
	CHECK(ui_test_shows("Contents"));
	CHECK(ui_test_shows("Chapter One"));
	/* Only the table of contents, not the other navigation */
	CHECK(!ui_test_shows("Landmark"));
	CHECK(ui_test_tap_text("Chapter Two & more"));
	ui_test_wait(200);
	ui_test_settle();
	CHECK(!ui_test_shows("Contents"));
	CHECK(ui_test_shows("Chapter Two & more"));
	CHECK(is_color(page_pixel(), 0x0000ff));
}

TEST(reading_position_kept)
{
	CHECK(book_write("test.epub"));
	CHECK(open_book());
	tap_page(FORWARD_X);
	tap_page(FORWARD_X);
	CHECK(ui_test_back());
	ui_test_wait(500);
	CHECK(ui_test_tap_text("Test Book"));
	ui_test_wait(200);
	ui_test_settle();
	/* Two pages down: past the link, in the red */
	CHECK(ui_test_shows("Chapter One"));
	tap_page(FORWARD_X);
	tap_page(FORWARD_X);
	tap_page(FORWARD_X);
	CHECK(ui_test_shows("Chapter Two & more"));
}

TEST(swipes_turn_pages)
{
	CHECK(book_write("test.epub"));
	CHECK(open_book());
	int swipes = 0;
	while (!ui_test_shows("Chapter Two & more") && swipes++ < 10) {
		ui_test_swipe(600, 800, 100, 800, 200);
		ui_test_wait(400);
		ui_test_settle();
	}
	CHECK(swipes >= 4 && swipes <= 6);
	CHECK(is_color(page_pixel(), 0x0000ff));
	ui_test_swipe(100, 800, 600, 800, 200);
	ui_test_wait(400);
	ui_test_settle();
	CHECK(ui_test_shows("Chapter One"));
	CHECK(is_color(page_pixel(), 0xff0000));
}

/* Whether row y of the page has anything but white */
static bool row_inked(struct page *p, int y)
{
	uint8_t row[720 * 4];
	page_draw(p, row, 720, 1, y);
	for (int i = 0; i < 720 * 4; i++)
		if (row[i] != 0xff)
			return true;
	return false;
}

/* Lines 60 px high (40 CSS px), their text in the middle: pages of 1000 px end
 * between the text of two lines (not text on both sides of the cut), 16 lines a page */
TEST(pages_break_between_lines)
{
	char html[8192] = "<html><body style=\"margin: 0; line-height: 40px\">";
	for (int i = 0; i < 100; i++)
		snprintf(html + strlen(html), sizeof(html) - strlen(html), "<div>Line %d</div>", i);
	strcat(html, "</body></html>");
	struct page *p = page_create(html, strlen(html), "about:lines", 720, 1000);
	CHECK_INT(page_height(p), 6000);
	int *starts = NULL;
	int pages = page_paginate(p, 1000, &starts);
	CHECK_INT(pages, 7); /* 6000 / 960, rounded up */
	for (int i = 1; i < pages; i++) {
		CHECK(!row_inked(p, starts[i] - 1) || !row_inked(p, starts[i]));
		CHECK(starts[i] - starts[i - 1] > 900 && starts[i] - starts[i - 1] <= 1000);
	}
	free(starts);
	page_destroy(p);
}

/* A picture as tall as half a page is not cut either; a taller one than a page is */
static char *no_css(const char *url, void *user)
{
	(void)url;
	(void)user;
	return NULL;
}

/* A book's text is in the book font (serif) at its one size: a heading as high
 * as a paragraph, a line of it as high as the font's lines; the browser's sizes vary */
/* A book's text is in the book font (serif) at the size asked, all of it: a
 * heading as high as a paragraph; the browser's sizes vary */
TEST(book_text_in_one_font_and_size)
{
	const char *para = "<html><body style=\"margin: 0\"><p style=\"margin: 0\">Text</p></body></html>";
	const char *heading = "<html><body style=\"margin: 0\"><h1 style=\"margin: 0; font-size: 2em\">Title</h1>"
	                      "</body></html>";
	int line[2];
	for (int i = 0; i < 2; i++) {
		int px = i ? 40 : 28;
		struct page *p = page_create_local(para, strlen(para), "epub:/x.xhtml", 720, 1000, no_css, NULL, px);
		line[i] = page_height(p);
		page_destroy(p);
		p = page_create_local(heading, strlen(heading), "epub:/x.xhtml", 720, 1000, no_css, NULL, px);
		CHECK_INT(page_height(p), line[i]);
		page_destroy(p);
	}
	/* About as much higher as the size */
	CHECK(line[0] >= 28 && line[0] <= 28 * 3 / 2);
	CHECK(line[1] * 28 >= line[0] * 40 - 28 * 2 && line[1] * 28 <= line[0] * 40 + 28 * 2);

	struct page *p = page_create(heading, strlen(heading), "https://example.com/", 720, 1000);
	struct page *q = page_create(para, strlen(para), "https://example.com/", 720, 1000);
	CHECK(page_height(p) > page_height(q));
	page_destroy(p);
	page_destroy(q);
}

/* Two chapters, the first of a few pages of text */
static bool text_book_write(void)
{
	static const char sentence[] = "Alice was beginning to get very tired of sitting by her sister on the "
		"bank, and of having nothing to do: once or twice she had peeped into the book her sister was "
		"reading, but it had no pictures or conversations in it. ";
	char *one = NULL;
	size_t one_size = 0;
	FILE *f = open_memstream(&one, &one_size);
	fputs("<html><body>", f);
	for (int i = 0; i < 12; i++)
		fprintf(f, "<p>%s</p>", sentence);
	fputs("</body></html>", f);
	fclose(f);
	const struct zip_file files[] = {
		{ "mimetype", "application/epub+zip", true, 0 },
		{ "META-INF/container.xml", CONTAINER, false, 0 },
		{ "OEBPS/content.opf",
		  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\"><metadata "
		  "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Text Book</dc:title></metadata><manifest>"
		  "<item href=\"nav.xhtml\" id=\"nav\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>"
		  "<item href=\"one.xhtml\" id=\"c1\" media-type=\"application/xhtml+xml\"/>"
		  "<item href=\"two.xhtml\" id=\"c2\" media-type=\"application/xhtml+xml\"/>"
		  "</manifest><spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine></package>", false, 0 },
		{ "OEBPS/nav.xhtml",
		  "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><body>"
		  "<nav epub:type=\"toc\"><ol><li><a href=\"one.xhtml\">Part One</a></li>"
		  "<li><a href=\"two.xhtml\">Part Two</a></li></ol></nav></body></html>", false, 0 },
		{ "OEBPS/one.xhtml", one, false, 0 },
		{ "OEBPS/two.xhtml", "<html><body><p>The end.</p></body></html>", false, 0 },
	};
	bool ok = zip_write(BOOKS "/text.epub", files, sizeof(files) / sizeof(files[0]));
	free(one);
	return ok;
}

/* Taps from where the reader is to Part Two */
static int taps_to_part_two(void)
{
	int taps = 0;
	while (!ui_test_shows("Part Two") && taps++ < 40)
		tap_page(FORWARD_X);
	return taps;
}

/* The text size: Aa in the header, smaller and larger; kept for every book, the
 * page laid out again where the reader was */
TEST(text_size_changed)
{
	CHECK(text_book_write());
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_tap_text("Text Book"));
	ui_test_wait(300);
	ui_test_settle();
	int taps = taps_to_part_two();
	CHECK(taps >= 2 && taps < 40);

	/* Back to its start, larger */
	CHECK(ui_test_tap_text(LV_SYMBOL_LIST));
	ui_test_settle();
	CHECK(ui_test_tap_text("Part One"));
	ui_test_wait(300);
	ui_test_settle();
	CHECK(ui_test_tap_text("Aa"));
	CHECK(ui_test_shows("28"));
	for (int i = 0; i < 4; i++) {
		CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
		ui_test_wait(300);
		ui_test_settle();
	}
	CHECK(ui_test_shows("36"));
	CHECK_INT(ui_setting_get_int("books_font_size", 0), 36);
	CHECK(ui_test_shows("Part One"));
	CHECK(taps_to_part_two() > taps);

	/* Kept */
	CHECK(ui_test_back());
	ui_test_wait(500);
	CHECK(ui_test_tap_text("Text Book"));
	ui_test_wait(300);
	ui_test_settle();
	CHECK(ui_test_tap_text("Aa"));
	CHECK(ui_test_shows("36"));
	CHECK(ui_test_tap_text(LV_SYMBOL_MINUS));
	ui_test_wait(300);
	ui_test_settle();
	CHECK(ui_test_shows("34"));
}

TEST(pages_break_around_images)
{
	const char *html = "<html><body style=\"margin: 0; line-height: 40px\">"
		"<div style=\"height: 600px\"></div><img src=\"a.png\" style=\"display: block; "
		"width: 100px; height: 300px\"><img src=\"b.png\" style=\"display: block; width: 100px; "
		"height: 1000px\"></body></html>";
	struct page *p = page_create(html, strlen(html), "epub:/x.xhtml", 720, 1000);
	const char *url;
	while ((url = page_next_wanted(p, PAGE_IMAGE))) {
		uint8_t *px = malloc(10 * 10 * 4);
		memset(px, 0xff, 10 * 10 * 4);
		page_set_image(p, url, px, 10, 10, 10, 10);
	}
	page_relayout(p);
	/* 900 px of space, then the 450 px image from 900 to 1350, then 1500 px of image */
	CHECK_INT(page_height(p), 900 + 450 + 1500);
	int *starts = NULL;
	int pages = page_paginate(p, 1000, &starts);
	CHECK(pages >= 3);
	if (pages >= 3) {
		CHECK_INT(starts[1], 900);  /* before the small image */
		CHECK_INT(starts[2], 1900); /* through the tall one */
	}
	free(starts);
	page_destroy(p);
}

/* Pages are at least half full: an image that would leave less on the page before it
 * (one starting in its first half, ending past it) is cut through at the page's end */
TEST(pages_at_least_half_full)
{
	const char *html = "<html><body style=\"margin: 0\"><div style=\"height: 200px\"></div>"
		"<img src=\"a.png\" style=\"display: block; width: 100px; height: 500px\"></body></html>";
	struct page *p = page_create(html, strlen(html), "epub:/x.xhtml", 720, 1000);
	const char *url;
	while ((url = page_next_wanted(p, PAGE_IMAGE))) {
		uint8_t *px = malloc(10 * 10 * 4);
		memset(px, 0xff, 10 * 10 * 4);
		page_set_image(p, url, px, 10, 10, 10, 10);
	}
	page_relayout(p);
	/* The image from 300 to 1050 */
	CHECK_INT(page_height(p), 1050);
	int *starts = NULL;
	int pages = page_paginate(p, 1000, &starts);
	CHECK_INT(pages, 2);
	if (pages == 2)
		CHECK_INT(starts[1], 1000);
	free(starts);
	page_destroy(p);
}

/* A chapter is laid out after the screen shows "Loading...", opening the book or
 * turning into another chapter */
TEST(loading_shown_while_a_chapter_is_laid_out)
{
	CHECK(book_write("test.epub"));
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_tap_text("Test Book"));
	CHECK(ui_test_shows("Loading..."));
	ui_test_wait(500);
	ui_test_settle();
	CHECK(!ui_test_shows("Loading..."));
	CHECK(is_color(page_pixel(), 0xff0000));

	/* The link to chapter two */
	ui_test_tap_at(360, 152 + 60);
	CHECK(ui_test_shows("Loading..."));
	ui_test_wait(500);
	ui_test_settle();
	CHECK(!ui_test_shows("Loading..."));
	CHECK(is_color(page_pixel(), 0x0000ff));

	/* Within the chapter: no loading */
	tap_page(FORWARD_X);
	CHECK(!ui_test_shows("Loading..."));
}

/* The reader's margins all around the page, 32 px, the book's own on the sides
 * (10% here) left out: the red in chapter one fills the page inside them */
TEST(margins_even_around_the_page)
{
	CHECK(book_write("test.epub"));
	CHECK(open_book());
	tap_page(FORWARD_X); /* the second page: red from top to bottom */
	int32_t top = 152, bottom = 1440 - 56;
	CHECK(is_color(ui_test_pixel(360, top + 16), 0xffffff));
	CHECK(is_color(ui_test_pixel(360, top + 48), 0xff0000));
	CHECK(is_color(ui_test_pixel(360, bottom - 16), 0xffffff));
	CHECK(is_color(ui_test_pixel(360, bottom - 48), 0xff0000));
	CHECK(is_color(ui_test_pixel(16, 800), 0xffffff));
	CHECK(is_color(ui_test_pixel(48, 800), 0xff0000));
	CHECK(is_color(ui_test_pixel(720 - 16, 800), 0xffffff));
	CHECK(is_color(ui_test_pixel(720 - 48, 800), 0xff0000));
}

/* In the dark theme, pages are dark: the white paper dark gray, colors as light as
 * they were dark (the red lighter, still red), the margins the paper's color */
TEST(dark_pages_in_the_dark_theme)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Display"));
	CHECK(ui_test_tap_text("Dark"));
	ui_test_wait(200);
	CHECK(!ui_light);
	CHECK(book_write("test.epub"));
	CHECK(open_book());
	tap_page(FORWARD_X); /* the second page: red from top to bottom */
	CHECK(is_color(ui_test_pixel(360, 152 + 16), PAGE_DARK_PAPER));
	CHECK(is_color(ui_test_pixel(16, 800), PAGE_DARK_PAPER));
	lv_color_t red = page_pixel();
	CHECK_INT(red.red, 0xff);
	CHECK(red.green > 0x40 && red.green < 0xc0);
	CHECK_INT(red.blue, red.green);
}

/* A BGRA image w x h of one color (0xAARRGGBB) on its left half, another on its right */
static uint8_t *halves(int w, int h, uint32_t left, uint32_t right)
{
	uint8_t *px = malloc((size_t)w * h * 4);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint32_t c = x < w / 2 ? left : right;
			uint8_t *p = px + ((size_t)y * w + x) * 4;
			p[0] = c & 0xff;
			p[1] = c >> 8 & 0xff;
			p[2] = c >> 16 & 0xff;
			p[3] = c >> 24;
		}
	return px;
}

/* Drawn dark, line art (black and white, or black on transparent: engravings, ink
 * drawings) is turned as the text is, black the ink and white the paper; pictures in
 * color are left as they are */
TEST(line_art_turned_on_dark_pages)
{
	const char *html = "<html><body style=\"margin: 0\">"
		"<img src=\"ink.png\" style=\"display: block; width: 100px; height: 100px\">"
		"<img src=\"engraving.png\" style=\"display: block; width: 100px; height: 100px\">"
		"<img src=\"photo.png\" style=\"display: block; width: 100px; height: 100px\">"
		"</body></html>";
	struct page *p = page_create(html, strlen(html), "epub:/x.xhtml", 720, 1000);
	const char *url;
	while ((url = page_next_wanted(p, PAGE_IMAGE))) {
		uint8_t *px = strstr(url, "ink") ? halves(10, 10, 0xff000000, 0x00000000) :
		              strstr(url, "engraving") ? halves(10, 10, 0xff000000, 0xffffffff) :
		              halves(10, 10, 0xffff0000, 0xff00ff00);
		page_set_image(p, url, px, 10, 10, 10, 10);
	}
	page_relayout(p);
	page_set_dark(p, true);
	static uint8_t buf[720 * 450 * 4];
	page_draw(p, buf, 720, 450, 0);
	/* (x, y) as 0xRRGGBB */
#define AT(x, y) ((uint32_t)buf[((y) * 720 + (x)) * 4 + 2] << 16 | buf[((y) * 720 + (x)) * 4 + 1] << 8 | \
	buf[((y) * 720 + (x)) * 4])
	CHECK_INT(AT(30, 75), PAGE_DARK_INK);    /* black on transparent */
	CHECK_INT(AT(120, 75), PAGE_DARK_PAPER); /* through the transparent half */
	CHECK_INT(AT(30, 225), PAGE_DARK_INK);   /* black on white */
	CHECK_INT(AT(120, 225), PAGE_DARK_PAPER);
	CHECK_INT(AT(30, 375), 0xff0000);        /* in color: as it is */
	CHECK_INT(AT(120, 375), 0x00ff00);
#undef AT
	page_destroy(p);
}

/* Whether the page (inside its margins) has a pixel of about `rgb` */
static bool page_has_color(uint32_t rgb)
{
	for (int32_t y = 152 + 32; y < 1440 - 56 - 32; y += 2)
		for (int32_t x = 32; x < 720 - 32; x += 2)
			if (near_color(ui_test_pixel(x, y), rgb))
				return true;
	return false;
}

/* A file's text, "" if none (static) */
static const char *read_text(const char *path)
{
	static char buf[1024];
	FILE *f = fopen(path, "r");
	size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
	if (f)
		fclose(f);
	buf[n] = '\0';
	return buf;
}

/* A book of one OPF (`opf`, at the zip's root) and the files after it */
static bool opf_book_write(const char *name, const char *opf, const struct zip_file *more, int count)
{
	struct zip_file files[16] = {
		{ "mimetype", "application/epub+zip", true, 0, 0 },
		{ "META-INF/container.xml", "<?xml version=\"1.0\"?><container version=\"1.0\"><rootfiles>"
		  "<rootfile full-path=\"content.opf\"/></rootfiles></container>", false, 0, 0 },
		{ "content.opf", opf, false, 0, 0 },
	};
	for (int i = 0; i < count && i < 13; i++)
		files[3 + i] = more[i];
	char path[256];
	snprintf(path, sizeof(path), BOOKS "/%s", name);
	return zip_write(path, files, 3 + count);
}

/* EPUB 2: the table of contents from the NCX (the spine's toc), nested entries in
 * order, those outside the spine or without a label left out; the title's character
 * references decoded */
TEST(contents_from_the_ncx)
{
	static const struct zip_file more[] = {
		{ "toc.ncx",
		  "<?xml version=\"1.0\"?><ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\"><navMap>"
		  "<navPoint id=\"p1\"><navLabel><text>Opening</text></navLabel><content src=\"one.xhtml\"/>"
		  "<navPoint id=\"p1a\"><navLabel><text>Inside &#8220;One&#x201D;</text></navLabel>"
		  "<content src=\"one.xhtml#inner\"/></navPoint></navPoint>"
		  "<navPoint id=\"p2\"><navLabel><text>Closing</text></navLabel><content src=\"two.xhtml\"/></navPoint>"
		  "<navPoint id=\"p3\"><navLabel><text>Elsewhere</text></navLabel><content src=\"notes.xhtml\"/></navPoint>"
		  "<navPoint id=\"p4\"><navLabel><text> </text></navLabel><content src=\"two.xhtml\"/></navPoint>"
		  "<navPoint id=\"p5\"><navLabel><text>Nowhere</text></navLabel></navPoint>"
		  "</navMap></ncx>", false, 0, 0 },
		{ "one.xhtml", "<html><body><p>One</p><p id=\"inner\">Inner</p></body></html>", false, 0, 0 },
		{ "two.xhtml", "<html><body style=\"margin: 0\"><div style=\"background: #0000ff; height: 2000px\">"
		  "</div></body></html>", false, 0, 0 },
	};
	CHECK(opf_book_write("old.epub",
	                     "<package version=\"2.0\"><metadata><dc:title>C&#97;f&#233; &#x4E2D; &#128214; &amp; Co"
	                     "</dc:title><dc:creator>Bo</dc:creator></metadata><manifest>"
	                     /* Spaces around =, single quotes; a value that names other attributes */
	                     "<item id = \"ncx\" href = 'toc.ncx' media-type=\"application/x-dtbncx+xml\"/>"
	                     "<item id=\"c1\" href=\"one.xhtml\" media-type=\"application/xhtml+xml\"/>"
	                     "<item title=\"a=x:href=y, the href here\" id=\"c2\" href=\"two.xhtml\" "
	                     "media-type=\"application/xhtml+xml\"/>"
	                     /* No href; an unquoted id (no id) */
	                     "<item id=\"style\" media-type=\"text/css\"/>"
	                     "<item id=c3 href=\"three.xhtml\" media-type=\"application/xhtml+xml\"/>"
	                     "</manifest><spine toc=\"ncx\"><itemref idref=\"c1\"/><itemref idref=\"c2\"/>"
	                     "<itemref idref=\"style\"/></spine></package>",
	                     more, sizeof(more) / sizeof(more[0])));
	CHECK(ui_test_open("Books"));
	const char *title = "Caf\xc3\xa9 \xe4\xb8\xad \xf0\x9f\x93\x96 & Co";
	CHECK(ui_test_shows(title));
	CHECK(ui_test_tap_text(title));
	ui_test_wait(200);
	ui_test_settle();
	CHECK(ui_test_shows("Inside \xe2\x80\x9cOne\xe2\x80\x9d"));
	CHECK(ui_test_shows("0%"));
	CHECK(ui_test_tap_text(LV_SYMBOL_LIST));
	CHECK(ui_test_shows("Opening"));
	CHECK(ui_test_shows("Inside \xe2\x80\x9cOne\xe2\x80\x9d"));
	CHECK(!ui_test_shows("Elsewhere"));
	CHECK(!ui_test_shows("Nowhere"));
	CHECK(ui_test_tap_text("Closing"));
	ui_test_wait(300);
	ui_test_settle();
	CHECK(ui_test_shows("Closing"));
	CHECK(ui_test_shows("50%"));
	CHECK(is_color(page_pixel(), 0x0000ff));
}

/* EPUB 3 without a nav marked as the contents: the first one, cut short (no end tag,
 * the last link not even closed); links without text left out */
TEST(contents_from_an_unmarked_nav)
{
	static const struct zip_file more[] = {
		{ "nav.xhtml", "<html><body><nav><ol><li><a href=\"one.xhtml\"/></li>"
		  "<li><a href=\"two.xhtml\">Second</a></li><li><a href=\"one.xhtml\"", false, 0, 0 },
		{ "one.xhtml", "<html><body><p>One</p></body></html>", false, 0, 0 },
		{ "two.xhtml", "<html><body><p>Two</p></body></html>", false, 0, 0 },
	};
	CHECK(opf_book_write("nav.epub",
	                     "<package version=\"3.0\"><metadata><dc:title>Unmarked</dc:title></metadata><manifest>"
	                     "<item id=\"nav\" href=\"nav.xhtml\" properties=\"nav\"/>"
	                     "<item id=\"c1\" href=\"one.xhtml\"/><item id=\"c2\" href=\"two.xhtml\"/>"
	                     "</manifest><spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine></package>",
	                     more, sizeof(more) / sizeof(more[0])));
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_tap_text("Unmarked"));
	ui_test_wait(200);
	ui_test_settle();
	CHECK(ui_test_tap_text(LV_SYMBOL_LIST));
	CHECK(ui_test_shows("Second"));
	lv_obj_t *rows[4];
	CHECK_INT(ui_test_find_all("Second", rows, 4), 1);
}

/* The navigation document read as a chapter too (as many books have it), its contents
 * linking to an element of itself ("#..."): an entry for that chapter */
TEST(contents_linking_within_the_contents)
{
	static const struct zip_file more[] = {
		{ "OEBPS/nav.xhtml", "<html><body><nav epub:type=\"toc\" id=\"toc\"><ol>"
		  "<li><a href=\"#toc\">Table of contents</a></li><li><a href=\"one.xhtml\">One</a></li>"
		  "</ol></nav></body></html>", false, 0, 0 },
		{ "OEBPS/one.xhtml", "<html><body><p>One</p></body></html>", false, 0, 0 },
	};
	CHECK(opf_book_write("self.epub",
	                     "<package version=\"3.0\"><metadata><dc:title>Self</dc:title></metadata><manifest>"
	                     "<item id=\"nav\" href=\"OEBPS/nav.xhtml\" properties=\"nav\"/>"
	                     "<item id=\"c1\" href=\"OEBPS/one.xhtml\"/></manifest>"
	                     "<spine><itemref idref=\"nav\"/><itemref idref=\"c1\"/></spine></package>",
	                     more, sizeof(more) / sizeof(more[0])));
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_tap_text("Self"));
	ui_test_wait(200);
	ui_test_settle();
	CHECK(ui_test_shows("Table of contents"));
	CHECK(ui_test_tap_text(LV_SYMBOL_LIST));
	CHECK(ui_test_tap_text("One"));
	ui_test_wait(300);
	ui_test_settle();
	CHECK(ui_test_shows("50%"));
}

TEST(no_contents)
{
	static const struct zip_file more[] = {
		{ "one.xhtml", "<html><body><p>One</p></body></html>", false, 0, 0 },
	};
	CHECK(opf_book_write("bare.epub",
	                     "<package><metadata><dc:title>Bare</dc:title></metadata><manifest>"
	                     "<item id=\"c1\" href=\"one.xhtml\"/></manifest><spine><itemref idref=\"c1\"/></spine>"
	                     "</package>", more, 1));
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_tap_text("Bare"));
	ui_test_wait(200);
	ui_test_settle();
	CHECK(ui_test_tap_text(LV_SYMBOL_LIST));
	CHECK(ui_test_shows("No contents"));
}

/* Without a title: its file's name; without its package: cannot be read */
TEST(books_without_a_title_or_package)
{
	static const struct zip_file more[] = {
		{ "one.xhtml", "<html><body><p>One</p></body></html>", false, 0, 0 },
	};
	CHECK(opf_book_write("untitled.epub", "<package><manifest><item id=\"c1\" href=\"one.xhtml\"/></manifest>"
	                     "<spine><itemref idref=\"c1\"/></spine></package>", more, 1));
	static const struct zip_file lost[] = {
		{ "mimetype", "application/epub+zip", true, 0, 0 },
		{ "META-INF/container.xml", "<container><rootfiles><rootfile full-path=\"gone.opf\"/></rootfiles>"
		  "</container>", false, 0, 0 },
	};
	CHECK(zip_write(BOOKS "/lost.epub", lost, 2));
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_shows("untitled.epub"));
	CHECK(ui_test_shows("lost.epub"));
	CHECK(ui_test_shows("Cannot be read"));
	/* The untitled one opens, its file's name the title */
	CHECK(ui_test_tap_text("untitled.epub"));
	ui_test_wait(200);
	ui_test_settle();
	CHECK(is_color(page_pixel(), 0xffffff));
	CHECK(!ui_test_shows("Books"));
}

/* A chapter that cannot be unpacked (not deflated as said, or cut short) says so, the
 * others read */
TEST(chapter_that_cannot_be_read)
{
	static const struct zip_file more[] = {
		{ "one.xhtml", "<html><body><p>One</p></body></html>", false, 0, NOT_DEFLATED },
		{ "two.xhtml", "<html><body style=\"margin: 0\"><div style=\"background: #0000ff; height: 2000px\">"
		  "</div></body></html>", false, 0, 0 },
		{ "three.xhtml", "<html><body><p>Three</p></body></html>", false, 0, CUT_SHORT },
	};
	CHECK(opf_book_write("broken.epub",
	                     "<package><metadata><dc:title>Damaged</dc:title></metadata><manifest>"
	                     "<item id=\"c1\" href=\"one.xhtml\"/><item id=\"c2\" href=\"two.xhtml\"/>"
	                     "<item id=\"c3\" href=\"three.xhtml\"/></manifest><spine><itemref idref=\"c1\"/>"
	                     "<itemref idref=\"c2\"/><itemref idref=\"c3\"/></spine></package>",
	                     more, sizeof(more) / sizeof(more[0])));
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_tap_text("Damaged"));
	ui_test_wait(200);
	ui_test_settle();
	/* "This part of the book cannot be read." in gray */
	CHECK(page_has_color(0x8e8e93));
	tap_page(FORWARD_X);
	ui_test_wait(200);
	ui_test_settle();
	CHECK(is_color(page_pixel(), 0x0000ff));
	for (int i = 0; i < 3 && !ui_test_shows("66%"); i++) {
		tap_page(FORWARD_X);
		ui_test_wait(200);
		ui_test_settle();
	}
	CHECK(ui_test_shows("66%"));
	CHECK(page_has_color(0x8e8e93));
}

/* A cover drawn as an <svg> with an <image> (as many are) shows the image; images of
 * a chapter come from the book */
TEST(svg_cover_shown_as_its_image)
{
	size_t png_size;
	void *png = ui_test_png(60, 90, 0x00a000, &png_size);
	const struct zip_file more[] = {
		{ "cover.xhtml", "<html><body style=\"margin: 0\"><svg xmlns=\"http://www.w3.org/2000/svg\" "
		  "xmlns:xlink=\"http://www.w3.org/1999/xlink\" viewBox=\"0 0 60 90\">"
		  "<image width=\"60\" height=\"90\" xlink:href=\"images/cover.png\"/></svg>"
		  /* No image in this one: left as it is */
		  "<svg><rect width=\"10\" height=\"10\"/></svg></body></html>", false, 0, 0 },
		{ "images/cover.png", png, true, png_size, 0 },
	};
	CHECK(opf_book_write("cover.epub",
	                     "<package><metadata><dc:title>Covered</dc:title></metadata><manifest>"
	                     "<item id=\"c1\" href=\"cover.xhtml\"/></manifest><spine><itemref idref=\"c1\"/></spine>"
	                     "</package>", more, 2));
	free(png);
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_tap_text("Covered"));
	ui_test_wait(200);
	ui_test_settle();
	CHECK(page_has_color(0x00a000));
}

/* A link to an element of the same chapter: its page, without laying it out again */
TEST(link_within_the_chapter)
{
	static const struct zip_file more[] = {
		{ "one.xhtml", "<html><body style=\"margin: 0\"><a href=\"#below\" style=\"display: block; "
		  "height: 100px\">Down</a><div style=\"height: 3000px\"></div><div id=\"below\" "
		  "style=\"background: #00ff00; height: 2000px\"></div></body></html>", false, 0, 0 },
	};
	CHECK(opf_book_write("jump.epub",
	                     "<package><metadata><dc:title>Jump</dc:title></metadata><manifest>"
	                     "<item id=\"c1\" href=\"one.xhtml\"/></manifest><spine><itemref idref=\"c1\"/></spine>"
	                     "</package>", more, 1));
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_tap_text("Jump"));
	ui_test_wait(200);
	ui_test_settle();
	CHECK(!page_has_color(0x00ff00));
	ui_test_tap_at(360, 152 + 60);
	CHECK(!ui_test_shows("Loading..."));
	ui_test_settle();
	CHECK(page_has_color(0x00ff00));
}

TEST(paths_resolved)
{
	char *fragment = NULL;
	char *path = epub_path("OEBPS/text/one.xhtml", "#note", &fragment);
	CHECK_STR(path, "OEBPS/text/one.xhtml");
	CHECK_STR(fragment, "note");
	free(path);
	free(fragment);
	path = epub_path("OEBPS/content.opf", "./text/../images/sub/", NULL);
	CHECK_STR(path, "OEBPS/images/sub");
	free(path);
	path = epub_path("OEBPS/content.opf", "/top%20level.xhtml", NULL);
	CHECK_STR(path, "top level.xhtml");
	free(path);
}

/* Within a chapter, back a page by a swipe; a drag up or down turns nothing, nor one
 * towards no page (it resists); a press while the page slides takes it */
TEST(swipes_within_a_chapter)
{
	CHECK(book_write("test.epub"));
	CHECK(open_book());
	/* Towards no page: back where it was */
	ui_test_swipe(100, 800, 400, 800, 300);
	ui_test_wait(400);
	ui_test_settle();
	CHECK(ui_test_shows("0%"));
	CHECK(is_color(ui_test_pixel(16, 800), 0xffffff));
	CHECK(is_color(ui_test_pixel(48, 800), 0xff0000));
	tap_page(FORWARD_X);
	tap_page(FORWARD_X);
	CHECK(ui_test_shows("25%"));
	ui_test_swipe(100, 800, 600, 800, 200);
	ui_test_wait(400);
	ui_test_settle();
	CHECK(ui_test_shows("12%"));
	/* Up and down */
	ui_test_swipe(360, 600, 360, 1000, 300);
	ui_test_wait(400);
	ui_test_settle();
	CHECK(ui_test_shows("12%"));
	CHECK(is_color(page_pixel(), 0xff0000));

	/* A tap, then a press while it slides, let go where it is */
	lv_test_mouse_move_to(FORWARD_X, 800);
	lv_test_mouse_press();
	ui_test_wait(32);
	lv_test_mouse_release();
	ui_test_wait(80);
	lv_test_mouse_press();
	ui_test_wait(32);
	lv_test_mouse_release();
	ui_test_wait(600);
	ui_test_settle();
	CHECK(ui_test_shows("25%"));
	CHECK(is_color(ui_test_pixel(16, 800), 0xffffff));
	CHECK(is_color(ui_test_pixel(48, 800), 0xff0000));
	CHECK(is_color(page_pixel(), 0xff0000));
}

/* A tap a frame long, without waiting for what it started */
static void quick_tap(int32_t x, int32_t y)
{
	lv_test_mouse_move_to(x, y);
	lv_test_mouse_press();
	ui_test_wait(16);
	lv_test_mouse_release();
	ui_test_wait(16);
}

/* Nothing turns while a chapter is about to be laid out: a tap on the page right after
 * the link to chapter two (laid out 50 ms after it) */
TEST(no_turning_while_loading)
{
	CHECK(book_write("test.epub"));
	CHECK(open_book());
	quick_tap(360, 152 + 60);
	CHECK(ui_test_shows("Loading..."));
	quick_tap(FORWARD_X, 800);
	ui_test_wait(500);
	ui_test_settle();
	CHECK(!ui_test_shows("Loading..."));
	CHECK(ui_test_shows("50%"));
	CHECK(is_color(page_pixel(), 0x0000ff));
}

/* A chapter chosen in the contents right after opening the book: that one */
TEST(contents_chosen_at_once)
{
	CHECK(book_write("test.epub"));
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_tap_text("Test Book"));
	CHECK(ui_test_tap_text(LV_SYMBOL_LIST));
	CHECK(ui_test_tap_text("Chapter Two & more"));
	ui_test_wait(500);
	ui_test_settle();
	CHECK(is_color(page_pixel(), 0x0000ff));
	CHECK(ui_test_shows("Chapter Two & more"));
}


/* Turning pages for a while keeps the place now and then, not only when the book is
 * closed; the other books' places stay */
TEST(position_kept_while_reading)
{
	CHECK(ui_test_write(BOOKS "/.positions", "1 5000 other.epub\n"));
	CHECK(book_write("test.epub"));
	CHECK(open_book());
	ui_test_wait(5000);
	tap_page(FORWARD_X);
	CHECK_STR(read_text(BOOKS "/.positions"), "1 5000 other.epub\n0 2511 test.epub\n");
}

/* Where it cannot be kept (the folder taken: the new file a folder, or the file one),
 * reading goes on */
TEST(position_that_cannot_be_kept)
{
	CHECK(book_write("test.epub"));
	CHECK(ui_test_write(BOOKS "/.positions", "1 5000 other.epub\n"));
	CHECK(mkdir(BOOKS "/.positions.new", 0755) == 0);
	for (int i = 0; i < 2; i++) {
		if (i) {
			/* The other book's place still there */
			CHECK_STR(read_text(BOOKS "/.positions"), "1 5000 other.epub\n");
			CHECK(remove(BOOKS "/.positions") == 0);
			CHECK(rmdir(BOOKS "/.positions.new") == 0);
			CHECK(ui_test_write(BOOKS "/.positions/x", "a folder"));
		}
		CHECK(open_book());
		tap_page(FORWARD_X);
		CHECK(ui_test_shows("12%"));
		CHECK(ui_test_back()); /* kept as it closes */
		ui_test_wait(500);
		CHECK(ui_test_shows("Ann Author"));
	}
	/* Its new file not left behind */
	CHECK_STR(read_text(BOOKS "/.positions.new"), "");
}

/* A book that went since the list was made: it does not open */
TEST(book_gone_since_listed)
{
	CHECK(book_write("test.epub"));
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_write(BOOKS "/test.epub", "not a zip any more"));
	CHECK(ui_test_tap_text("Test Book"));
	ui_test_wait(200);
	CHECK(ui_test_shows("Ann Author"));
	CHECK(!ui_test_shows("Loading..."));
}

/* A cover smaller than its tile is scaled up to it; the library left and opened again */
TEST(small_cover_scaled_up)
{
	size_t png_size;
	void *png = ui_test_png(20, 30, 0xff0000, &png_size);
	CHECK(cover_book_write("small.epub", "<dc:title>Small</dc:title><meta name=\"cover\" content=\"pic\"/>",
	                       "<item href=\"red.png\" id=\"pic\" media-type=\"image/png\"/>",
	                       "OEBPS/red.png", png, png_size));
	free(png);
	CHECK(ui_test_open("Books"));
	ui_test_wait(500);
	ui_test_settle();
	lv_obj_t *cover = ui_test_find_widget(&lv_3dtexture_class);
	CHECK(cover);
	if (cover) {
		lv_area_t a;
		lv_obj_get_coords(cover, &a);
		CHECK(near_color(ui_test_pixel((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2), 0xff0000));
		CHECK(near_color(ui_test_pixel(a.x1 + 30, a.y2 - 30), 0xff0000));
	}
	CHECK(ui_test_back());
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_shows("Small"));
}
