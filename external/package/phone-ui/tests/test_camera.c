/*
 * Camera (camera.c) on the tests' cameras (ui_test.h, fake_camera.c): the preview
 * of each camera, photos at their full size (the rear one's from a raw frame), tap
 * to focus and back to continuous, missing cameras and frames that never come.
 */

#include <dirent.h>
#include <errno.h>
#include <linux/videodev2.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "i18n.h"
#include "ui_test.h"
#include "ui_symbols.h"

#define PHOTOS "/var/lib/photos"
#define FOCUS_ZONE (V4L2_CID_USER_BASE | 0x1001)

TEST_SETUP
{
	ui_test_reset();
}

/* The preview, its center on the screen */
static lv_obj_t *preview(int32_t *x, int32_t *y)
{
	lv_obj_t *p = ui_test_find_widget(&lv_3dtexture_class);
	if (p && x && y) {
		lv_area_t a;
		lv_obj_get_coords(p, &a);
		*x = (a.x1 + a.x2) / 2;
		*y = (a.y1 + a.y2) / 2;
	}
	return p;
}

/* The shutter: left of the rear / front button, which has the only label */
static lv_obj_t *shutter(void)
{
	lv_obj_t *icon = ui_test_find(LV_SYMBOL_REFRESH);
	return icon ? lv_obj_get_child(lv_obj_get_parent(lv_obj_get_parent(icon)), 0) : NULL;
}

static bool take(void)
{
	lv_obj_t *s = shutter();
	if (s)
		ui_test_tap(s);
	return s;
}

/* Within `tolerance` of the 0xRRGGBB color in every channel */
static bool near(lv_color_t c, uint32_t rgb, int tolerance)
{
	int want[3] = { (int)(rgb >> 16 & 0xff), (int)(rgb >> 8 & 0xff), (int)(rgb & 0xff) };
	int got[3] = { c.red, c.green, c.blue };
	for (int i = 0; i < 3; i++)
		if (abs(got[i] - want[i]) > tolerance) {
			printf("color %02x%02x%02x, not %06x\n", c.red, c.green, c.blue, (unsigned)rgb);
			return false;
		}
	return true;
}

/* The photos taken */
static unsigned photos(char *name, size_t size)
{
	DIR *dir = opendir(PHOTOS);
	struct dirent *e;
	unsigned n = 0;
	while (dir && (e = readdir(dir)))
		if (e->d_name[0] != '.' && n++ == 0 && name)
			snprintf(name, size, PHOTOS "/%s", e->d_name);
	if (dir)
		closedir(dir);
	return n;
}

/* The one photo: its size and the color at its center */
static bool photo(int *w, int *h, lv_color_t *center)
{
	char path[300];
	if (photos(path, sizeof(path)) != 1)
		return false;
	CHECK(strncmp(path, PHOTOS "/IMG_", strlen(PHOTOS "/IMG_")) == 0);
	FILE *f = fopen(path, "rb");
	uint8_t *px = f ? ui_jpeg_read(f, 10000, 10000, false, w, h) : NULL;
	if (f)
		fclose(f);
	if (!px)
		return false;
	const uint8_t *p = px + ((size_t)(*h / 2) * *w + *w / 2) * 4;
	*center = lv_color_make(p[2], p[1], p[0]);
	free(px);
	return true;
}

static bool photo_taken(void)
{
	return photos(NULL, 0) > 0;
}

TEST(no_camera)
{
	CHECK(ui_test_open("Camera"));
	CHECK(ui_test_shows("Camera not found"));
	CHECK(!ui_test_camera_streaming());
	CHECK(!ui_test_camera_open());
}

TEST(rear_preview)
{
	ui_test_camera(true, true);
	ui_test_camera_scene(0xc05020);
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	CHECK_STR(ui_test_camera_streaming(), "ov5640 1280x720 UYVY");
	CHECK(!ui_test_shows("Starting camera..."));
	int32_t x, y;
	CHECK(preview(&x, &y));
	CHECK(near(ui_test_pixel(x, y), 0xc05020, 6));
	CHECK(near(ui_test_pixel(x, y - 300), 0xc05020, 6));
	CHECK_INT(ui_test_camera_control(V4L2_CID_FOCUS_AUTO), 1); /* continuous */
}

/* No frame yet: the shutter does nothing */
TEST(starting)
{
	ui_test_camera(true, false);
	ui_test_camera_stall(true);
	CHECK(ui_test_open("Camera"));
	ui_test_wait(500);
	CHECK(ui_test_shows("Starting camera..."));
	CHECK(take());
	ui_test_wait(500);
	CHECK_STR(ui_test_camera_streaming(), "ov5640 1280x720 UYVY");
	CHECK_INT(photos(NULL, 0), 0);
}

/* The rear camera's photos: a raw frame at its full size, portrait, then the preview again */
TEST(rear_photo)
{
	ui_test_camera(true, true);
	ui_test_camera_scene(0x3070b0);
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	CHECK(take());
	CHECK(ui_test_wait_until(photo_taken, 5000));
	ui_test_wait(100);
	CHECK_STR(ui_test_camera_streaming(), "ov5640 1280x720 UYVY");
	CHECK_INT(ui_test_camera_control(V4L2_CID_FOCUS_AUTO), 1); /* continuous again */
	int w = 0, h = 0;
	lv_color_t c;
	CHECK(photo(&w, &h, &c));
	CHECK_INT(w, 1944);
	CHECK_INT(h, 2592);
	CHECK(near(c, 0x3070b0, 6));
	/* And the preview goes on */
	int32_t x, y;
	CHECK(preview(&x, &y));
	ui_test_wait(200);
	CHECK(near(ui_test_pixel(x, y), 0x3070b0, 6));
}

/* While the raw stream is on: the shutter switched to it, the lens kept where it is */
TEST(rear_photo_raw_stream)
{
	ui_test_camera(true, false);
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	ui_test_camera_stall(true);
	CHECK(take());
	CHECK_STR(ui_test_camera_streaming(), "ov5640 2592x1944 BGGR");
	CHECK_INT(ui_test_camera_control(V4L2_CID_FOCUS_AUTO), 0);
	/* A second press while it is taken is not a second photo */
	CHECK(take());
	ui_test_camera_stall(false);
	CHECK(ui_test_wait_until(photo_taken, 5000));
	ui_test_wait(500);
	CHECK_INT(photos(NULL, 0), 1);
}

/* No raw frame within 3 s: no photo, back to the preview */
TEST(rear_photo_timeout)
{
	ui_test_camera(true, false);
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	ui_test_camera_stall(true);
	CHECK(take());
	ui_test_wait(3200);
	CHECK_STR(ui_test_camera_streaming(), "ov5640 1280x720 UYVY");
	CHECK_INT(photos(NULL, 0), 0);
	/* Then photos work again */
	ui_test_camera_stall(false);
	ui_test_wait(200);
	CHECK(take());
	CHECK(ui_test_wait_until(photo_taken, 5000));
}

static long cpu_mhz(void)
{
	long min = 0, max = 0;
	ui_read_long("/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq", &min);
	ui_read_long("/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq", &max);
	return min == max ? min / 1000 : -1;
}

/* The preview keeps up at the lowest frequency (66 FPS, a third of a core); a photo is
 * debayered and encoded on the CPU, at the highest until it is saved or given up (cpu.c) */
TEST(cpu_at_highest_for_photos)
{
	ui_test_camera(true, false);
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	CHECK_INT(cpu_mhz(), 648);
	ui_test_camera_stall(true);
	CHECK(take());
	CHECK_INT(cpu_mhz(), 1152);
	ui_test_wait(3200);
	CHECK_INT(photos(NULL, 0), 0);
	CHECK_INT(cpu_mhz(), 648);
	ui_test_camera_stall(false);
	ui_test_wait(200);
	CHECK(take());
	CHECK(ui_test_wait_until(photo_taken, 5000));
	ui_test_wait(100);
	CHECK_INT(cpu_mhz(), 648);
}

/* The front camera: its preview frames, at its full size */
TEST(front_photo)
{
	ui_test_camera(true, true);
	ui_test_camera_scene(0x40a050);
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	CHECK(ui_test_tap_text(LV_SYMBOL_REFRESH));
	ui_test_wait(300);
	CHECK_STR(ui_test_camera_streaming(), "gc2145 1600x1200 UYVY");
	int32_t x, y;
	CHECK(preview(&x, &y));
	CHECK(near(ui_test_pixel(x, y), 0x40a050, 6));
	CHECK(take());
	CHECK(ui_test_wait_until(photo_taken, 5000));
	CHECK_STR(ui_test_camera_streaming(), "gc2145 1600x1200 UYVY");
	int w = 0, h = 0;
	lv_color_t c;
	CHECK(photo(&w, &h, &c));
	CHECK_INT(w, 1200);
	CHECK_INT(h, 1600);
	CHECK(near(c, 0x40a050, 6));
	/* Fixed focus: a tap does not focus */
	ui_test_tap_at(x, y);
	CHECK_INT(ui_test_camera_control(V4L2_CID_AUTO_FOCUS_START), 0);
	/* And back to the rear one */
	CHECK(ui_test_tap_text(LV_SYMBOL_REFRESH));
	ui_test_wait(300);
	CHECK_STR(ui_test_camera_streaming(), "ov5640 1280x720 UYVY");
}

TEST(front_missing)
{
	ui_test_camera(true, false);
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	CHECK(ui_test_tap_text(LV_SYMBOL_REFRESH));
	CHECK(ui_test_shows("Camera not found"));
	CHECK(!ui_test_camera_streaming());
	CHECK(ui_test_tap_text(LV_SYMBOL_REFRESH));
	ui_test_wait(300);
	CHECK_STR(ui_test_camera_streaming(), "ov5640 1280x720 UYVY");
	CHECK(!ui_test_shows("Camera not found"));
}

/* The focus ring's color, 58 px right of where it was tapped (its border) */
static lv_color_t ring(int32_t x, int32_t y)
{
	return ui_test_pixel(x + 58, y);
}

/* A tap focuses on that spot: white ring while focusing, green once focused, then gone */
TEST(focus_tap)
{
	ui_test_camera(true, false);
	ui_test_camera_scene(0x202060);
	ui_test_camera_focus(3, true);
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	int32_t x, y;
	CHECK(preview(&x, &y));
	ui_test_tap_at(x, y);
	CHECK_INT(ui_test_camera_control(V4L2_CID_FOCUS_AUTO), 0);
	/* The middle of the preview is the middle of the sensor's 1280 x 720 (a pixel either way) */
	int32_t zone = ui_test_camera_control(FOCUS_ZONE);
	CHECK(abs((zone >> 16) - 640) <= 1);
	CHECK(abs((zone & 0xffff) - 360) <= 1);
	CHECK_INT(ui_test_camera_control(V4L2_CID_AUTO_FOCUS_START), 1);
	CHECK(near(ring(x, y), 0xffffff, 40));
	ui_test_wait(600);
	CHECK(near(ring(x, y), 0x34c759, 40));
	ui_test_wait(1000);
	CHECK(near(ring(x, y), 0x202060, 6));
	/* A photo keeps the lens there (manual focus after it too) */
	CHECK(take());
	CHECK(ui_test_wait_until(photo_taken, 5000));
	ui_test_wait(100);
	CHECK_INT(ui_test_camera_control(V4L2_CID_FOCUS_AUTO), 0);
}

TEST(focus_fails)
{
	ui_test_camera(true, false);
	ui_test_camera_scene(0x202060);
	ui_test_camera_focus(1, false);
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	int32_t x, y;
	CHECK(preview(&x, &y));
	ui_test_tap_at(x - 100, y + 200);
	ui_test_wait(400);
	CHECK(near(ring(x - 100, y + 200), 0xff453a, 40));
	/* A tap again while the ring shows starts over */
	ui_test_camera_focus(1, true);
	ui_test_tap_at(x, y);
	CHECK_INT(ui_test_camera_control(V4L2_CID_AUTO_FOCUS_START), 2);
	ui_test_wait(400);
	CHECK(near(ring(x, y), 0x34c759, 40));
}

/* Busy for good: given up after 30 polls (100 ms, on 16 ms frames), as not focused */
TEST(focus_busy)
{
	ui_test_camera(true, false);
	ui_test_camera_scene(0x202060);
	ui_test_camera_focus(1000, true);
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	int32_t x, y;
	CHECK(preview(&x, &y));
	ui_test_tap_at(x, y);
	ui_test_wait(2500);
	CHECK(near(ring(x, y), 0xffffff, 40));
	ui_test_wait(1200);
	CHECK(near(ring(x, y), 0xff453a, 40));
}

/* Switching to the front camera while focusing: the ring goes, the front one streams */
TEST(switched_while_focusing)
{
	ui_test_camera(true, true);
	ui_test_camera_scene(0x202060);
	ui_test_camera_focus(1000, true);
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	int32_t x, y;
	CHECK(preview(&x, &y));
	ui_test_tap_at(x, y);
	ui_test_wait(300);
	CHECK(near(ring(x, y), 0xffffff, 40));
	CHECK(ui_test_tap_text(LV_SYMBOL_REFRESH));
	ui_test_wait(300);
	CHECK_STR(ui_test_camera_streaming(), "gc2145 1600x1200 UYVY");
	ui_test_wait(3500); /* past when the focus would have given up */
	CHECK(near(ring(x, y), 0x202060, 6));
}

/* Photos cannot be written (the folder read-only): none left half written, and the camera
 * goes on */
static void photos_read_only(void)
{
	CHECK(mkdir(PHOTOS, 0755) == 0 || errno == EEXIST);
	CHECK(chmod(PHOTOS, 0555) == 0);
}

TEST(rear_photo_not_saved)
{
	ui_test_camera(true, false);
	ui_test_camera_scene(0x3070b0);
	photos_read_only();
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	CHECK(take());
	ui_test_wait(3000);
	CHECK_INT(photos(NULL, 0), 0);
	CHECK_STR(ui_test_camera_streaming(), "ov5640 1280x720 UYVY");
	chmod(PHOTOS, 0755);
	CHECK(take());
	CHECK(ui_test_wait_until(photo_taken, 5000));
}

TEST(front_photo_not_saved)
{
	ui_test_camera(true, true);
	ui_test_camera_scene(0x40a050);
	photos_read_only();
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	CHECK(ui_test_tap_text(LV_SYMBOL_REFRESH));
	ui_test_wait(300);
	CHECK(take());
	ui_test_wait(3000);
	CHECK_INT(photos(NULL, 0), 0);
	CHECK_STR(ui_test_camera_streaming(), "gc2145 1600x1200 UYVY");
	chmod(PHOTOS, 0755);
}

/* A long press: continuous autofocus over the default zones again */
TEST(focus_continuous_again)
{
	ui_test_camera(true, false);
	ui_test_camera_scene(0x202060);
	ui_test_camera_focus(100, true);
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	int32_t x, y;
	lv_obj_t *p = preview(&x, &y);
	CHECK(p);
	ui_test_tap_at(x, y);
	CHECK(near(ring(x, y), 0xffffff, 40));
	ui_test_long_press(lv_obj_get_parent(p));
	CHECK_INT(ui_test_camera_control(FOCUS_ZONE), 0);
	CHECK_INT(ui_test_camera_control(V4L2_CID_FOCUS_AUTO), 1);
	CHECK(near(ring(x, y), 0x202060, 6));
	/* A photo then goes back to continuous */
	CHECK(take());
	CHECK(ui_test_wait_until(photo_taken, 5000));
	ui_test_wait(100);
	CHECK_INT(ui_test_camera_control(V4L2_CID_FOCUS_AUTO), 1);
}

/* Leaving the app powers the camera down; it starts again when opened */
TEST(closed_on_back)
{
	ui_test_camera(true, false);
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	int32_t x, y;
	CHECK(preview(&x, &y));
	ui_test_tap_at(x, y); /* a focus ring's timer running */
	CHECK(ui_test_camera_open());
	CHECK(ui_test_back());
	ui_test_wait(200);
	CHECK(!ui_test_camera_streaming());
	CHECK(!ui_test_camera_open());
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	CHECK_STR(ui_test_camera_streaming(), "ov5640 1280x720 UYVY");
}

/* In Romanian: without a camera */
TEST(romanian)
{
	ui_test_write("/var/lib/phone-ui/settings", "language=ro\n");
	CHECK(ui_test_open("Cameră"));
	CHECK(ui_test_shows("Camera nu a fost găsită"));
}

/* QR codes, read as they come into view (iPhone's way): a button over the preview says
 * what the code holds and does it when tapped */

/* The button's text shown and not another one's (the code read again each frame) */
static bool qr_button(const char *text)
{
	return ui_test_wait_for(text, 3000);
}

/* The button's icon, before its text in the same button */
static bool qr_icon(const char *symbol)
{
	lv_obj_t *icon = ui_test_find(symbol);
	return icon && lv_obj_get_child(lv_obj_get_parent(icon), 0) == icon &&
	       lv_obj_get_child_count(lv_obj_get_parent(icon)) == 2;
}

TEST(qr_web_address)
{
	ui_test_camera(true, false);
	ui_test_camera_qr("https://example.com/page");
	ui_test_serve("https://example.com/page", 200, "text/html", "<title>Example page</title><p>Hello</p>");
	CHECK(ui_test_open("Camera"));
	CHECK(qr_button("Open example.com"));
	CHECK(qr_icon(UI_SYMBOL_GLOBE));
	ui_test_screenshot("build/camera_qr.png");
	CHECK(ui_test_tap_text("Open example.com"));
	CHECK(ui_test_wait_for("Example page", 3000));
	CHECK(ui_test_requested("https://example.com/page"));
	CHECK(!ui_test_camera_streaming()); /* the camera closed meanwhile */
}

TEST(qr_wifi_network)
{
	ui_test_wifi_ap("Home", -50, "[WPA2-PSK-CCMP][ESS]", "secret123");
	ui_test_camera(true, false);
	ui_test_camera_qr("WIFI:T:WPA;S:Home;P:secret123;;");
	CHECK(ui_test_open("Camera"));
	CHECK(qr_button("Join Home"));
	CHECK(qr_icon(LV_SYMBOL_WIFI));
	CHECK(ui_test_tap_text("Join Home"));
	CHECK(ui_test_wait_for("Connected to Home", 3000)); /* Settings, Wi-Fi */
}

TEST(qr_phone_number)
{
	ui_test_camera(true, false);
	ui_test_camera_qr("tel:+40770000001");
	CHECK(ui_test_open("Camera"));
	CHECK(qr_button("Call +40770000001"));
	CHECK(qr_icon(UI_SYMBOL_PHONE));
	CHECK(ui_test_tap_text("Call +40770000001"));
	CHECK(ui_test_wait_for("+40770000001", 2000)); /* in the dialer, to call or change */
	CHECK(!ui_test_shows("Call +40770000001"));
}

/* Anything else: shown whole when tapped */
TEST(qr_text)
{
	ui_test_camera(true, false);
	ui_test_camera_qr("Meet at the north entrance, 6 pm");
	CHECK(ui_test_open("Camera"));
	CHECK(qr_button("Meet at the north entrance, 6 pm"));
	CHECK(qr_icon(UI_SYMBOL_ALIGN_LEFT));
	CHECK(ui_test_tap_text("Meet at the north entrance, 6 pm"));
	CHECK(ui_test_wait_for("QR code", 2000));
	CHECK(ui_test_shows("Meet at the north entrance, 6 pm"));
}

/* The code taken away: its button goes a moment later */
TEST(qr_gone)
{
	ui_test_camera(true, false);
	ui_test_camera_qr("https://example.com/page");
	CHECK(ui_test_open("Camera"));
	CHECK(qr_button("Open example.com"));
	ui_test_camera_qr(NULL);
	ui_test_wait(500);
	CHECK(ui_test_shows("Open example.com")); /* not at the first frame without it */
	for (int i = 0; i < 40 && ui_test_shows("Open example.com"); i++)
		ui_test_wait_for("-", 100);
	CHECK(!ui_test_shows("Open example.com"));
}

/* The front camera reads them too */
TEST(qr_front_camera)
{
	ui_test_camera(true, true);
	CHECK(ui_test_open("Camera"));
	ui_test_wait(200);
	CHECK(ui_test_tap_text(LV_SYMBOL_REFRESH));
	ui_test_camera_qr("https://example.com/page");
	CHECK(qr_button("Open example.com"));
	CHECK_STR(ui_test_camera_streaming(), "gc2145 1600x1200 UYVY");
}

/* Photos are still taken with a code in view */
TEST(qr_photo)
{
	ui_test_camera(true, false);
	ui_test_camera_qr("https://example.com/page");
	CHECK(ui_test_open("Camera"));
	CHECK(qr_button("Open example.com"));
	CHECK(take());
	ui_test_wait(1500);
	DIR *d = opendir(PHOTOS);
	int photos = 0;
	for (struct dirent *e; d && (e = readdir(d));)
		photos += e->d_name[0] != '.';
	if (d)
		closedir(d);
	CHECK_INT(photos, 1);
}
