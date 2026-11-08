/*
 * Screenshots (screenshot.c) of the tests' display (ui_test.h, fake_drm.c): what the
 * screen shows saved as a JPEG in the photos, a flash once saved; nothing when there
 * is no display or the photos cannot be written.
 */

#include <dirent.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "test.h"
#include "ui_test.h"

#define PHOTOS "/var/lib/photos"

TEST_SETUP
{
	ui_test_start();
}

/* The screenshots saved, the first one's path in `name` */
static unsigned shots(char *name, size_t size)
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

static int brightness(lv_color_t c)
{
	return c.red + c.green + c.blue;
}

/* The JPEG at `path` is the screen: its size, and every 8th pixel of every 8th row
 * (JPEG's few differences aside: the texts' edges) */
static void same_as_screen(const char *path)
{
	FILE *f = fopen(path, "rb");
	CHECK(f);
	if (!f)
		return;
	int w = 0, h = 0;
	uint8_t *px = ui_jpeg_read(f, 10000, 10000, false, &w, &h);
	fclose(f);
	CHECK(px);
	if (!px)
		return;
	CHECK_INT(w, 720);
	CHECK_INT(h, 1440);
	long sum = 0, n = 0;
	for (int32_t y = 0; w == 720 && h == 1440 && y < h; y += 8)
		for (int32_t x = 0; x < w; x += 8, n++) {
			const uint8_t *p = px + ((size_t)y * w + x) * 4;
			lv_color_t s = ui_test_pixel(x, y);
			sum += abs(p[2] - s.red) + abs(p[1] - s.green) + abs(p[0] - s.blue);
		}
	free(px);
	printf("screenshot: %.2f off on average\n", n ? (double)sum / n : 0.0);
	CHECK(n && sum < 6 * n);
}

/* Power and Volume Down: the screen in the photos */
TEST(keys)
{
	ui_test_button(KEY_POWER, true);
	ui_test_wait(200);
	ui_test_button_press(KEY_VOLUMEDOWN, 50);
	ui_test_wait(1000);
	ui_test_button(KEY_POWER, false);
	ui_test_settle();
	char path[300];
	CHECK_INT(shots(path, sizeof(path)), 1);
	CHECK(strncmp(path, PHOTOS "/SCR_", strlen(PHOTOS "/SCR_")) == 0);
	CHECK(strstr(path, ".jpg") != NULL);
	same_as_screen(path);
}

/* A white flash once saved, gone after */
TEST(flash)
{
	int before = brightness(ui_test_pixel(40, 1000));
	ui_screenshot();
	ui_test_wait(150);
	CHECK_INT(shots(NULL, 0), 1);
	CHECK(brightness(ui_test_pixel(40, 1000)) > before + 200);
	ui_test_settle();
	CHECK_INT(brightness(ui_test_pixel(40, 1000)), before);
}

/* `phone-ui --screenshot <file>` (main.c): where it is asked */
TEST(to_file)
{
	CHECK(ui_screenshot_save("/tmp/shot.jpg"));
	same_as_screen("/tmp/shot.jpg");
	CHECK_INT(shots(NULL, 0), 0);
	remove("/tmp/shot.jpg");
}

TEST(no_display)
{
	remove("/dev/dri/card0");
	int before = brightness(ui_test_pixel(40, 1000));
	ui_screenshot();
	ui_test_wait(150);
	CHECK_INT(shots(NULL, 0), 0);
	CHECK_INT(brightness(ui_test_pixel(40, 1000)), before); /* no flash */
}

/* A scanout it cannot read (nothing shown, no access, tiled, not mappable): nothing
 * saved, no flash */
TEST(scanout_unreadable)
{
	static const enum ui_test_scanout kinds[] = { UI_TEST_SCANOUT_OFF, UI_TEST_SCANOUT_NO_HANDLE,
	                                              UI_TEST_SCANOUT_TILED, UI_TEST_SCANOUT_UNMAPPABLE };
	int before = brightness(ui_test_pixel(40, 1000));
	for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		ui_test_scanout(kinds[i]);
		ui_screenshot();
		ui_test_wait(150);
		CHECK_INT(shots(NULL, 0), 0);
		CHECK_INT(brightness(ui_test_pixel(40, 1000)), before);
		CHECK(!ui_screenshot_save("/tmp/shot.jpg"));
	}
}

/* The photos folder cannot be made: nothing saved, no flash, nothing left behind */
TEST(cannot_write)
{
	ui_test_write(PHOTOS, "a file\n");
	int before = brightness(ui_test_pixel(40, 1000));
	ui_screenshot();
	ui_test_wait(150);
	CHECK_INT(brightness(ui_test_pixel(40, 1000)), before);
	CHECK(!ui_screenshot_save("/tmp/no/such/folder.jpg"));
	struct stat st;
	CHECK(stat("/tmp/no/such/folder.jpg", &st) != 0);
}
