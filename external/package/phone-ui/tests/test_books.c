/*
 * Books (books/): ePub files in /var/lib/books, listed with their title and
 * author, read a chapter at a time. Chapters are drawn into textures, not labels:
 * the tests check the header, the footer (the table of contents' entry the reader
 * is at), and pixels.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <zlib.h>

#include "test.h"
#include "ui_test.h"

#define BOOKS "/var/lib/books"
/* Taps in the reader: its view is below the header (152 px) */
#define LOWER_Y 1200
#define UPPER_Y 220

TEST_SETUP
{
	ui_test_start();
}

/* A zip: entries deflated unless `stored` */
struct zip_file {
	const char *name, *data;
	bool stored;
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
	unsigned char *packed[16];
	for (int i = 0; i < count; i++) {
		size_t n = strlen(files[i].data);
		crcs[i] = crc32(0, (const Bytef *)files[i].data, n);
		packed[i] = malloc(n + 64);
		if (files[i].stored) {
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
		offsets[i] = ftell(f);
		put32(f, 0x04034b50);
		put16(f, 20);
		put16(f, 0);
		put16(f, files[i].stored ? 0 : 8);
		put32(f, 0);
		put32(f, crcs[i]);
		put32(f, csizes[i]);
		put32(f, n);
		put16(f, strlen(files[i].name));
		put16(f, 0);
		fputs(files[i].name, f);
		fwrite(packed[i], 1, csizes[i], f);
	}
	unsigned long dir = ftell(f);
	for (int i = 0; i < count; i++) {
		put32(f, 0x02014b50);
		put16(f, 20);
		put16(f, 20);
		put16(f, 0);
		put16(f, files[i].stored ? 0 : 8);
		put32(f, 0);
		put32(f, crcs[i]);
		put32(f, csizes[i]);
		put32(f, strlen(files[i].data));
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
		{ "OEBPS/styles/book.css", ".red { background: #ff0000; height: 3000px; margin: 0 }", false },
		{ "OEBPS/text/one.xhtml",
		  "<?xml version='1.0' encoding='utf-8'?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
		  "<title>One</title><link href=\"../styles/book.css\" rel=\"stylesheet\" type=\"text/css\"/>"
		  "</head><body style=\"margin: 0\">"
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

static void tap_page(int32_t y)
{
	ui_test_tap_at(360, y);
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
}

TEST(books_listed_with_title_and_author)
{
	CHECK(book_write("test.epub"));
	CHECK(ui_test_write(BOOKS "/broken.epub", "not a zip"));
	CHECK(ui_test_write(BOOKS "/notes.txt", "not a book"));
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_shows("Test Book"));
	CHECK(ui_test_shows("Ann Author"));
	/* What cannot be read is listed by its file name */
	CHECK(ui_test_shows("broken.epub"));
	CHECK(!ui_test_shows("notes.txt"));
	CHECK(!ui_test_shows("No books"));
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
		tap_page(LOWER_Y);
	/* 300 + 4500 px: four pages of red, then chapter two */
	CHECK(taps >= 4 && taps <= 6);
	CHECK(is_color(page_pixel(), 0x0000ff));

	tap_page(UPPER_Y);
	CHECK(ui_test_shows("Chapter One"));
	/* At its end */
	CHECK(is_color(page_pixel(), 0xff0000));
}

TEST(link_followed_to_its_chapter)
{
	CHECK(book_write("test.epub"));
	CHECK(open_book());
	tap_page(152 + 60);
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
	tap_page(LOWER_Y);
	tap_page(LOWER_Y);
	CHECK(ui_test_back());
	ui_test_wait(500);
	CHECK(ui_test_tap_text("Test Book"));
	ui_test_wait(200);
	ui_test_settle();
	/* Two pages down: past the link, in the red */
	CHECK(ui_test_shows("Chapter One"));
	tap_page(LOWER_Y);
	tap_page(LOWER_Y);
	tap_page(LOWER_Y);
	CHECK(ui_test_shows("Chapter Two & more"));
}
