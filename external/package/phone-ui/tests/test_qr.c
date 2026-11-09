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
