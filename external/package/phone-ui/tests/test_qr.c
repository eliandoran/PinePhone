/* qr_scan.c: what a code says (ui_qr_parse()), and codes read from frames */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test.h"

#include "ui/qr.h"
#include "ui/qrcodegen.h"

TEST(wifi)
{
	struct ui_qr_content c;
	ui_qr_parse("WIFI:T:WPA;S:Home;P:secret123;;", &c);
	CHECK_INT(c.kind, UI_QR_WIFI);
	CHECK_STR(c.ssid, "Home");
	CHECK_STR(c.password, "secret123");
	CHECK_STR(c.type, "WPA");
	CHECK(!c.hidden);
}

/* Fields in any order, escapes undone, lower case */
TEST(wifi_escaped)
{
	struct ui_qr_content c;
	ui_qr_parse("wifi:S:Joe's\\;Net;H:true;p:a\\\\b\\;c\\,d\\:e\\\"f;T:SAE;;", &c);
	CHECK_INT(c.kind, UI_QR_WIFI);
	CHECK_STR(c.ssid, "Joe's;Net");
	CHECK_STR(c.password, "a\\b;c,d:e\"f");
	CHECK_STR(c.type, "SAE");
	CHECK(c.hidden);
}

TEST(wifi_open)
{
	struct ui_qr_content c;
	ui_qr_parse("WIFI:T:nopass;S:Cafe;;", &c);
	CHECK_INT(c.kind, UI_QR_WIFI);
	CHECK_STR(c.type, "nopass");
	CHECK_STR(c.password, "");
}

/* No name: nothing to join */
TEST(wifi_without_name)
{
	struct ui_qr_content c;
	ui_qr_parse("WIFI:T:WPA;P:secret123;;", &c);
	CHECK_INT(c.kind, UI_QR_TEXT);
}

TEST(others)
{
	struct ui_qr_content c;
	ui_qr_parse("https://example.com/a?b=c", &c);
	CHECK_INT(c.kind, UI_QR_URL);
	ui_qr_parse("HTTP://EXAMPLE.COM", &c);
	CHECK_INT(c.kind, UI_QR_URL);
	ui_qr_parse("tel:+40 770 000-001", &c);
	CHECK_INT(c.kind, UI_QR_PHONE);
	CHECK_STR(c.number, "+40770000001");
	ui_qr_parse("tel:", &c);
	CHECK_INT(c.kind, UI_QR_TEXT);
	ui_qr_parse("ftp://example.com", &c);
	CHECK_INT(c.kind, UI_QR_TEXT);
}

/* A UYVY frame, gray, with `text`'s code (modules of `px`) at (x0, y0), or none */
static uint8_t *frame(int w, int h, const char *text, int px, int x0, int y0)
{
	uint8_t *f = malloc((size_t)w * h * 2), temp[qrcodegen_BUFFER_LEN_MAX], code[qrcodegen_BUFFER_LEN_MAX];
	for (size_t i = 0; i < (size_t)w * h * 2; i += 2) {
		f[i] = 128;
		f[i + 1] = 110;
	}
	if (text && qrcodegen_encodeText(text, temp, code, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
	                                 qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
		int n = qrcodegen_getSize(code) + 8;
		for (int y = 0; y < n * px; y++)
			for (int x = 0; x < n * px; x++)
				f[((size_t)(y0 + y) * w + x0 + x) * 2 + 1] =
					qrcodegen_getModule(code, x / px - 4, y / px - 4) ? 20 : 235;
	}
	return f;
}

/* The outcome of a frame, waited for (up to 5 s) */
static bool scanned(struct ui_qr_scan *s, char *text, size_t size, float corners[4][2])
{
	bool found = false;
	for (int i = 0; i < 500 && !ui_qr_scan_result(s, &found, text, size, corners); i++)
		nanosleep(&(struct timespec){ .tv_nsec = 10000000 }, NULL);
	return found;
}

TEST(read_from_a_frame)
{
	struct ui_qr_scan *s = ui_qr_scan_create();
	char text[256];
	float corners[4][2];
	uint8_t *f = frame(1280, 720, "https://example.com/page", 8, 400, 100);
	CHECK(ui_qr_scan_uyvy(s, f, 1280, 720, 1280 * 2));
	CHECK(!ui_qr_scan_uyvy(s, f, 1280, 720, 1280 * 2) || true); /* busy or done: either */
	CHECK(scanned(s, text, sizeof(text), corners));
	CHECK_STR(text, "https://example.com/page");
	/* The top left corner, of the code itself (inside the quiet zone), in frame pixels */
	CHECK(corners[0][0] > 400 + 4 * 8 - 6 && corners[0][0] < 400 + 4 * 8 + 6);
	CHECK(corners[0][1] > 100 + 4 * 8 - 6 && corners[0][1] < 100 + 4 * 8 + 6);
	free(f);

	f = frame(1280, 720, NULL, 0, 0, 0);
	while (!ui_qr_scan_uyvy(s, f, 1280, 720, 1280 * 2))
		nanosleep(&(struct timespec){ .tv_nsec = 1000000 }, NULL);
	CHECK(!scanned(s, text, sizeof(text), corners));
	free(f);
	ui_qr_scan_delete(s);
}

/* Another size of frame (the front camera's) */
TEST(frame_size_changes)
{
	struct ui_qr_scan *s = ui_qr_scan_create();
	char text[256];
	float corners[4][2];
	uint8_t *f = frame(1600, 1200, "hello", 12, 500, 300);
	CHECK(ui_qr_scan_uyvy(s, f, 1600, 1200, 1600 * 2));
	CHECK(scanned(s, text, sizeof(text), corners));
	CHECK_STR(text, "hello");
	free(f);
	ui_qr_scan_delete(s);
}

/* Contacts: vCards (3.0, as phones' cameras and contact apps read them) */

TEST(vcard_made)
{
	char text[512];
	ui_qr_vcard(text, sizeof(text), "Jane Doe", "+44 7700 900123");
	CHECK_STR(text, "BEGIN:VCARD\r\nVERSION:3.0\r\nN:Doe;Jane;;;\r\nFN:Jane Doe\r\n"
	                "TEL;TYPE=CELL:+44 7700 900123\r\nEND:VCARD\r\n");
	/* The last word is the family name; vCard's special characters escaped */
	ui_qr_vcard(text, sizeof(text), "Plumber; 24h, fast\\", "0744555210");
	CHECK_STR(text, "BEGIN:VCARD\r\nVERSION:3.0\r\nN:fast\\\\;Plumber\\; 24h\\,;;;\r\n"
	                "FN:Plumber\\; 24h\\, fast\\\\\r\nTEL;TYPE=CELL:0744555210\r\nEND:VCARD\r\n");
	/* One word: a given name */
	ui_qr_vcard(text, sizeof(text), "Cher", "+15550100");
	CHECK(strstr(text, "\r\nN:;Cher;;;\r\n"));
}

/* What ui_qr_vcard() makes reads back as it was */
TEST(vcard_read_back)
{
	char text[512];
	struct ui_qr_content c;
	ui_qr_vcard(text, sizeof(text), "Plumber; 24h, fast\\", "0744555210");
	ui_qr_parse(text, &c);
	CHECK_INT(c.kind, UI_QR_CONTACT);
	CHECK_STR(c.name, "Plumber; 24h, fast\\");
	CHECK_STR(c.number, "0744555210");
}

/* As other phones make them: 2.1 or 4.0, LF only, groups, parameters, a home number
 * before the mobile one, a long line folded */
TEST(vcard_others)
{
	struct ui_qr_content c;
	ui_qr_parse("BEGIN:VCARD\nVERSION:4.0\nN:Popescu;Ana;;;\nFN:Ana Popes\n cu\n"
	            "item1.TEL;TYPE=HOME:+40 21 555 0100\nTEL;TYPE=cell,voice;VALUE=uri:tel:+40-744-555-210\n"
	            "EMAIL:ana@example.com\nEND:VCARD", &c);
	CHECK_INT(c.kind, UI_QR_CONTACT);
	CHECK_STR(c.name, "Ana Popescu");
	CHECK_STR(c.number, "+40-744-555-210");

	/* No FN: N's given and family names; no mobile: the first number */
	ui_qr_parse("BEGIN:VCARD\r\nVERSION:2.1\r\nN:Doe;John\r\nTEL;WORK:555 0100\r\nEND:VCARD\r\n", &c);
	CHECK_INT(c.kind, UI_QR_CONTACT);
	CHECK_STR(c.name, "John Doe");
	CHECK_STR(c.number, "555 0100");

	/* Neither a name nor a number: text */
	ui_qr_parse("BEGIN:VCARD\nVERSION:3.0\nEMAIL:x@example.com\nEND:VCARD", &c);
	CHECK_INT(c.kind, UI_QR_TEXT);
}

/* MECARD (Japanese phones', some generators'): N is "family,given" */
TEST(mecard)
{
	struct ui_qr_content c;
	ui_qr_parse("MECARD:N:Doe,Jane;TEL:+447700900123;EMAIL:jane@example.com;;", &c);
	CHECK_INT(c.kind, UI_QR_CONTACT);
	CHECK_STR(c.name, "Jane Doe");
	CHECK_STR(c.number, "+447700900123");
	ui_qr_parse("MECARD:N:Bob\\;s Garage;TEL:555;;", &c);
	CHECK_STR(c.name, "Bob;s Garage");
}
