/* docs/apps/books.md's screenshots (included by test_docs.c): public domain books,
 * a few paragraphs of them */

#include <sys/stat.h>
#include <zlib.h>

#include "browser/browser.h"
#include "ui.h"

#define BOOKS_DIR "/var/lib/books"
#define BOOKS_COVER_W 400
#define BOOKS_COVER_H 600

/* A cover: `html` drawn by the browser's page, as a JPEG (malloc'd) */
static char *books_cover(const char *html, size_t *size)
{
	struct page *p = page_create(html, strlen(html), "epub:/cover.xhtml", BOOKS_COVER_W, BOOKS_COVER_H);
	uint8_t *px = malloc(BOOKS_COVER_W * BOOKS_COVER_H * 4);
	page_draw(p, px, BOOKS_COVER_W, BOOKS_COVER_H, 0);
	page_destroy(p);
	char *jpeg = NULL;
	FILE *f = open_memstream(&jpeg, size);
	ui_jpeg_write(f, px, BOOKS_COVER_W, BOOKS_COVER_H, BOOKS_COVER_W * 4, 90, true);
	fclose(f);
	free(px);
	return jpeg;
}

static void books_put16(FILE *f, unsigned v)
{
	fputc(v & 0xff, f);
	fputc(v >> 8 & 0xff, f);
}

static void books_put32(FILE *f, unsigned long v)
{
	books_put16(f, v & 0xffff);
	books_put16(f, v >> 16 & 0xffff);
}

/* An ePub of `count` chapters (XHTML bodies) with a table of contents of their
 * <h2>s' texts, `titles`, and a cover drawn from `cover` (HTML) unless it is NULL;
 * files stored, not deflated */
static void books_write(const char *file, const char *title, const char *author, const char *cover,
                        const char *const *titles, const char *const *chapters, int count)
{
	char opf[4096], nav[4096], names[8][32], *xhtml[8];
	int n = snprintf(opf, sizeof(opf),
		"<?xml version='1.0' encoding='utf-8'?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
		"version=\"3.0\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>%s</dc:title>"
		"<dc:creator>%s</dc:creator></metadata><manifest>"
		"<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>%s",
		title, author, cover ? "<item id=\"cover\" href=\"cover.jpg\" media-type=\"image/jpeg\" "
		"properties=\"cover-image\"/>" : "");
	int m = snprintf(nav, sizeof(nav), "<html xmlns=\"http://www.w3.org/1999/xhtml\" "
		"xmlns:epub=\"http://www.idpf.org/2007/ops\"><body><nav epub:type=\"toc\"><ol>");
	for (int i = 0; i < count; i++) {
		snprintf(names[i], sizeof(names[i]), "OEBPS/c%d.xhtml", i + 1);
		n += snprintf(opf + n, sizeof(opf) - n, "<item id=\"c%d\" href=\"c%d.xhtml\" "
		              "media-type=\"application/xhtml+xml\"/>", i + 1, i + 1);
		m += snprintf(nav + m, sizeof(nav) - m, "<li><a href=\"c%d.xhtml\">%s</a></li>", i + 1, titles[i]);
		CHECK(asprintf(&xhtml[i], "<?xml version='1.0' encoding='utf-8'?>\n<html "
		               "xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>%s</title></head><body>"
		               "<h2 style=\"text-align: center\">%s</h2>%s</body></html>",
		               titles[i], titles[i], chapters[i]) > 0);
	}
	n += snprintf(opf + n, sizeof(opf) - n, "</manifest><spine>");
	for (int i = 0; i < count; i++)
		n += snprintf(opf + n, sizeof(opf) - n, "<itemref idref=\"c%d\"/>", i + 1);
	snprintf(opf + n, sizeof(opf) - n, "</spine></package>");
	snprintf(nav + m, sizeof(nav) - m, "</ol></nav></body></html>");

	const char *entry_names[11] = { "mimetype", "META-INF/container.xml", "OEBPS/content.opf",
	                                "OEBPS/nav.xhtml" };
	const char *data[11] = { "application/epub+zip",
		"<?xml version=\"1.0\"?><container version=\"1.0\" "
		"xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
		"<rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
		"</rootfiles></container>", opf, nav };
	size_t sizes[11] = { 0 };
	for (int i = 0; i < count; i++) {
		entry_names[4 + i] = names[i];
		data[4 + i] = xhtml[i];
	}
	int entries = 4 + count;
	char *jpeg = NULL;
	if (cover) {
		jpeg = books_cover(cover, &sizes[entries]);
		entry_names[entries] = "OEBPS/cover.jpg";
		data[entries++] = jpeg;
	}
	for (int i = 0; i < entries; i++)
		if (!sizes[i])
			sizes[i] = strlen(data[i]);

	char path[256];
	snprintf(path, sizeof(path), BOOKS_DIR "/%s", file);
	mkdir(BOOKS_DIR, 0755);
	FILE *f = fopen(path, "wb");
	CHECK(f != NULL);
	if (!f) {
		free(jpeg);
		return;
	}
	unsigned long offsets[11];
	for (int pass = 0; pass < 2; pass++) {
		unsigned long dir = ftell(f);
		for (int i = 0; i < entries; i++) {
			size_t size = sizes[i];
			if (pass == 0)
				offsets[i] = ftell(f);
			books_put32(f, pass ? 0x02014b50 : 0x04034b50);
			if (pass)
				books_put16(f, 20);
			books_put16(f, 20);
			books_put16(f, 0);
			books_put16(f, 0); /* stored */
			books_put32(f, 0);
			books_put32(f, crc32(0, (const Bytef *)data[i], size));
			books_put32(f, size);
			books_put32(f, size);
			books_put16(f, strlen(entry_names[i]));
			books_put16(f, 0);
			if (pass) {
				books_put32(f, 0); /* comment, disk */
				books_put32(f, 0); /* attributes */
				books_put16(f, 0);
				books_put32(f, offsets[i]);
			}
			fputs(entry_names[i], f);
			if (!pass)
				fwrite(data[i], 1, size, f);
		}
		if (pass) {
			unsigned long end = ftell(f);
			books_put32(f, 0x06054b50);
			books_put32(f, 0);
			books_put16(f, entries);
			books_put16(f, entries);
			books_put32(f, end - dir);
			books_put32(f, dir);
			books_put16(f, 0);
		}
	}
	fclose(f);
	for (int i = 0; i < count; i++)
		free(xhtml[i]);
	free(jpeg);
}

static void books_library(void)
{
	static const char *const alice_titles[] = {
		"Down the Rabbit-Hole", "The Pool of Tears", "A Caucus-Race and a Long Tale",
	};
	static const char *const alice[] = {
		"<p>Alice was beginning to get very tired of sitting by her sister on the bank, and of "
		"having nothing to do: once or twice she had peeped into the book her sister was reading, "
		"but it had no pictures or conversations in it, &#8220;and what is the use of a book,&#8221; "
		"thought Alice &#8220;without pictures or conversations?&#8221;</p>"
		"<p>So she was considering in her own mind (as well as she could, for the hot day made her "
		"feel very sleepy and stupid), whether the pleasure of making a daisy-chain would be worth "
		"the trouble of getting up and picking the daisies, when suddenly a White Rabbit with pink "
		"eyes ran close by her.</p>"
		"<p>There was nothing so <i>very</i> remarkable in that; nor did Alice think it so <i>very</i> "
		"much out of the way to hear the Rabbit say to itself, &#8220;Oh dear! Oh dear! I shall be "
		"late!&#8221; (when she thought it over afterwards, it occurred to her that she ought to have "
		"wondered at this, but at the time it all seemed quite natural); but when the Rabbit actually "
		"<i>took a watch out of its waistcoat-pocket</i>, and looked at it, and then hurried on, Alice "
		"started to her feet, for it flashed across her mind that she had never before seen a rabbit "
		"with either a waistcoat-pocket, or a watch to take out of it, and burning with curiosity, she "
		"ran across the field after it, and fortunately was just in time to see it pop down a large "
		"rabbit-hole under the hedge.</p>"
		"<p>In another moment down went Alice after it, never once considering how in the world she "
		"was to get out again.</p>",
		"<p>&#8220;Curiouser and curiouser!&#8221; cried Alice.</p>",
		"<p>They were indeed a queer-looking party that assembled on the bank.</p>",
	};
	static const char *const machine_titles[] = { "I" };
	static const char *const machine[] = { "<p>The Time Traveller was expounding a recondite matter to us.</p>" };
	static const char *const pride_titles[] = { "Chapter 1" };
	static const char *const pride[] = { "<p>It is a truth universally acknowledged...</p>" };
	static const char *const moby_titles[] = { "Loomings" };
	static const char *const moby[] = { "<p>Call me Ishmael.</p>" };
	books_write("alice.epub", "Alice's Adventures in Wonderland", "Lewis Carroll",
	            "<html style=\"background: #2e5e3e\"><body style=\"margin: 0; color: #f4e8c8; text-align: center\">"
	            "<div style=\"padding: 60px 16px 0; font-size: 32px\">Alice's Adventures in Wonderland</div>"
	            "<div style=\"padding-top: 90px; font-size: 20px\">Lewis Carroll</div></body></html>",
	            alice_titles, alice, 3);
	books_write("moby-dick.epub", "Moby-Dick; or, The Whale", "Herman Melville",
	            "<html style=\"background: #1d3557\"><body style=\"margin: 0; color: #ffffff; text-align: center\">"
	            "<div style=\"padding: 70px 16px 0; font-size: 40px\">Moby-Dick</div>"
	            "<div style=\"font-size: 20px\">or, The Whale</div>"
	            "<div style=\"padding-top: 140px; font-size: 20px\">Herman Melville</div></body></html>",
	            moby_titles, moby, 1);
	books_write("time-machine.epub", "The Time Machine", "H. G. Wells", NULL, machine_titles, machine, 1);
	books_write("pride.epub", "Pride and Prejudice", "Jane Austen",
	            "<html style=\"background: #8c2f39\"><body style=\"margin: 0; color: #fbe9d0; text-align: center\">"
	            "<div style=\"padding: 70px 16px 0; font-size: 32px\">Pride and Prejudice</div>"
	            "<div style=\"padding-top: 110px; font-size: 20px\">Jane Austen</div></body></html>",
	            pride_titles, pride, 1);
}

/* books.md: the library */
TEST(books_library)
{
	books_library();
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_shows("Alice's Adventures in Wonderland"));
	CHECK(ui_test_shows("Lewis Carroll"));
	CHECK(ui_test_shows("Pride and Prejudice"));
	/* The covers, The Time Machine has none: the placeholder */
	ui_test_wait(500);
	ui_test_settle();
	lv_obj_t *covers[4];
	CHECK_INT(ui_test_find_widgets(&lv_3dtexture_class, covers, 4), 4);
	shot("books-library");
}

/* books.md: a book open, its footer */
TEST(books_reader)
{
	books_library();
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_tap_text("Alice's Adventures in Wonderland"));
	ui_test_wait(300);
	ui_test_settle();
	CHECK(ui_test_shows("Down the Rabbit-Hole"));
	CHECK(ui_test_shows("0%"));
	shot("books-reader");
}

/* books.md: the contents, the chapter read in blue */
TEST(books_contents)
{
	books_library();
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_tap_text("Alice's Adventures in Wonderland"));
	ui_test_wait(300);
	CHECK(ui_test_tap_text(LV_SYMBOL_LIST));
	ui_test_settle();
	CHECK(ui_test_shows("Contents"));
	CHECK(ui_test_shows("The Pool of Tears"));
	shot("books-contents");
}

/* books.md: the text size's bar */
TEST(books_text_size)
{
	books_library();
	CHECK(ui_test_open("Books"));
	CHECK(ui_test_tap_text("Alice's Adventures in Wonderland"));
	ui_test_wait(300);
	CHECK(ui_test_tap_text("Aa"));
	ui_test_settle();
	CHECK(ui_test_shows("28"));
	shot("books-text-size");
}
