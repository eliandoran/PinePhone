/* Gallery (gallery.c) and the photo viewer (viewer.c), through the UI: phone-ui on
 * the host (ui_test.h). Photos are small PPMs as the camera writes them. */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "test.h"
#include "ui_test.h"

#define PHOTOS "/var/lib/photos"
#define DELETE LV_SYMBOL_TRASH "  Delete photo"

TEST_SETUP
{
	ui_test_start();
}

static time_t base;

/* A w x h photo of one color, taken `age` minutes ago */
static void photo_write(const char *name, int w, int h, int age)
{
	char path[128];
	mkdir(PHOTOS, 0755);
	snprintf(path, sizeof(path), PHOTOS "/%s", name);
	FILE *f = fopen(path, "w");
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int i = 0; i < w * h; i++)
		fwrite("\x40\x80\xc0", 1, 3, f);
	fclose(f);
	if (!base)
		base = time(NULL) - 3600;
	struct timeval tv[2] = { { base - age * 60, 0 }, { base - age * 60, 0 } };
	utimes(path, tv);
}

/* Three photos, newest first: c, b, a */
static void three_photos(void)
{
	photo_write("a.ppm", 32, 24, 30);
	photo_write("b.ppm", 24, 32, 20);
	photo_write("c.ppm", 16, 16, 10);
}

static bool exists(const char *name)
{
	char path[128];
	snprintf(path, sizeof(path), PHOTOS "/%s", name);
	return access(path, F_OK) == 0;
}

/* The thumbnails shown, after they are decoded */
static unsigned thumbnails(lv_obj_t **found, unsigned max)
{
	ui_test_wait(200);
	return ui_test_find_widgets(&lv_3dtexture_class, found, max);
}

/* The caption of a photo taken `age` minutes before base */
static const char *caption(int age)
{
	static char buf[64];
	time_t t = base - age * 60;
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(buf, sizeof(buf), "%-d %B %Y, %H:%M", &tm);
	return buf;
}

/* Opens the Gallery's photo i (newest first) */
static bool view(unsigned i)
{
	lv_obj_t *thumbs[8];
	if (!ui_test_open("Gallery") || thumbnails(thumbs, 8) <= i)
		return false;
	ui_test_tap(thumbs[i]);
	return ui_test_shows("Photo");
}

/* Drags across the photo by dx in ms */
static void drag(int dx, uint32_t ms)
{
	ui_test_swipe(360 - dx / 2, 700, 360 + dx / 2, 700, ms);
}

TEST(no_photos)
{
	CHECK(ui_test_open("Gallery"));
	CHECK(ui_test_shows("No photos"));
}

TEST(no_photos_in_the_folder)
{
	mkdir(PHOTOS, 0755);
	ui_test_write(PHOTOS "/notes.txt", "not a photo");
	CHECK(ui_test_open("Gallery"));
	CHECK(ui_test_shows("No photos"));
}

TEST(grid_newest_first)
{
	three_photos();
	ui_test_write(PHOTOS "/notes.txt", "not a photo");
	ui_test_write(PHOTOS "/empty.jpg", "");
	CHECK(ui_test_open("Gallery"));
	lv_obj_t *thumbs[8];
	CHECK_INT(thumbnails(thumbs, 8), 3);
	CHECK(!ui_test_shows("No photos"));
	/* Three square cells in a row */
	lv_area_t a0, a2;
	lv_obj_get_coords(thumbs[0], &a0);
	lv_obj_get_coords(thumbs[2], &a2);
	CHECK_INT(a0.y1, a2.y1);
	CHECK(a0.x1 < a2.x1);
	CHECK_INT(lv_area_get_width(&a0), lv_area_get_height(&a0));

	ui_test_tap(thumbs[0]);
	CHECK(ui_test_shows(caption(10)));
	CHECK(ui_test_shows("1 of 3"));
}

TEST(photo_not_decoded)
{
	three_photos();
	ui_test_write(PHOTOS "/bad.jpg", "not a JPEG");
	CHECK(ui_test_open("Gallery"));
	CHECK_INT(thumbnails(NULL, 0), 3);
}

TEST(swipes)
{
	three_photos();
	CHECK(view(0));
	CHECK(ui_test_shows("1 of 3"));
	drag(-500, 300);
	CHECK(ui_test_shows("2 of 3"));
	CHECK(ui_test_shows(caption(20)));
	drag(-500, 300);
	CHECK(ui_test_shows("3 of 3"));
	/* Nothing after the last one: it springs back */
	drag(-500, 300);
	CHECK(ui_test_shows("3 of 3"));
	drag(500, 300);
	CHECK(ui_test_shows("2 of 3"));
}

TEST(flick_and_small_drag)
{
	three_photos();
	CHECK(view(0));
	/* A short quick one turns the photo, a short slow one does not */
	drag(-80, 100);
	CHECK(ui_test_shows("2 of 3"));
	drag(80, 100);
	CHECK(ui_test_shows("1 of 3"));
	drag(-120, 800);
	CHECK(ui_test_shows("1 of 3"));
	/* Nothing before the first one */
	drag(500, 300);
	CHECK(ui_test_shows("1 of 3"));
}

TEST(swipe_caught_while_sliding)
{
	three_photos();
	CHECK(view(0));
	/* Let go, it slides to the next one; caught on the way and dragged on, it ends
	 * there */
	lv_test_mouse_move_to(600, 700);
	lv_test_mouse_press();
	ui_test_wait(16);
	lv_test_mouse_move_to(400, 700);
	ui_test_wait(16);
	lv_test_mouse_release();
	ui_test_wait(64);
	ui_test_swipe(600, 700, 300, 700, 300);
	CHECK(ui_test_shows("2 of 3"));
	drag(-500, 300);
	CHECK(ui_test_shows("3 of 3"));
}

TEST(delete_photos)
{
	three_photos();
	CHECK(view(1));
	CHECK(ui_test_shows("2 of 3"));
	/* The trash button asks, twice hides it again, Cancel too */
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_shows(DELETE));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(!ui_test_shows(DELETE));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text("Cancel"));
	CHECK(!ui_test_shows(DELETE));
	CHECK(exists("b.ppm"));

	/* The next one comes */
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text(DELETE));
	CHECK(!exists("b.ppm"));
	CHECK(ui_test_shows("2 of 2"));
	CHECK(ui_test_shows(caption(30)));
	/* The last one: the one before it */
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text(DELETE));
	CHECK(!exists("a.ppm"));
	CHECK(ui_test_shows(caption(10)));
	CHECK(!ui_test_shows("1 of 1")); /* alone: no position */

	CHECK(ui_test_back());
	CHECK_INT(thumbnails(NULL, 0), 1);
}

TEST(delete_the_last_photo)
{
	photo_write("a.ppm", 8, 8, 0);
	CHECK(view(0));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text(DELETE));
	CHECK(!exists("a.ppm"));
	/* Back in the Gallery */
	CHECK(ui_test_shows("Gallery"));
	CHECK(ui_test_shows("No photos"));
}

TEST(delete_fails)
{
	three_photos();
	CHECK(view(0));
	unlink(PHOTOS "/c.ppm");
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text(DELETE));
	CHECK(ui_test_shows("1 of 3"));
}

static char picked[128];

static void picked_cb(const char *path, void *user_data)
{
	snprintf(picked, sizeof(picked), "%s", path);
	(*(int *)user_data)++;
}

TEST(pick)
{
	int calls = 0;
	three_photos();
	ui_photo_pick("Choose a photo", picked_cb, &calls);
	ui_test_settle();
	CHECK(ui_test_shows("Choose a photo"));
	lv_obj_t *thumbs[3];
	CHECK_INT(thumbnails(thumbs, 3), 3);
	/* Nothing chosen: no check, no button */
	CHECK_INT(ui_test_find_all(LV_SYMBOL_OK, NULL, 0), 0);
	ui_test_tap(thumbs[1]);
	CHECK_INT(ui_test_find_all(LV_SYMBOL_OK, NULL, 0), 2);
	ui_test_tap(thumbs[1]); /* again: none */
	CHECK_INT(ui_test_find_all(LV_SYMBOL_OK, NULL, 0), 0);
	ui_test_tap(thumbs[0]);
	ui_test_tap(thumbs[2]);
	CHECK_INT(ui_test_find_all(LV_SYMBOL_OK, NULL, 0), 2);
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK_INT(calls, 1);
	CHECK_STR(picked, PHOTOS "/a.ppm");
	CHECK(!ui_test_shows("Choose a photo"));
}

TEST(pick_over_the_gallery)
{
	int calls = 0;
	three_photos();
	CHECK(ui_test_open("Gallery"));
	ui_photo_pick("Choose a photo", picked_cb, &calls);
	ui_test_settle();
	CHECK(!ui_test_shows("Choose a photo"));
	CHECK(ui_test_shows("Gallery"));
}

TEST(view_from_an_app)
{
	photo_write("a.ppm", 64, 48, 0);
	photo_write("b.ppm", 48, 64, 0);
	const char *paths[] = { PHOTOS "/a.ppm", PHOTOS "/b.ppm" };
	/* Nothing to show: no screen */
	ui_photo_view("Photos", paths, 0, 0, NULL);
	ui_photo_view("Photos", paths, 2, 2, NULL);
	ui_test_settle();
	CHECK(!ui_test_shows("Photos"));

	/* Without a caption or delete */
	ui_photo_view("Photos", paths, 1, 0, NULL);
	ui_test_settle();
	CHECK(ui_test_shows("Photos"));
	CHECK(!ui_test_shows(LV_SYMBOL_TRASH));
	CHECK(!ui_test_shows("1 of 1"));
	CHECK(ui_test_find_widget(&lv_3dtexture_class));
	/* Already open: not again */
	ui_photo_view("Other", paths, 2, 0, NULL);
	ui_test_settle();
	CHECK(!ui_test_shows("Other"));
	CHECK(ui_test_back());
	CHECK(!ui_test_shows("Photos"));
}

/* An app edited a photo (ui_photo_view_replace): shown again, or its neighbor */
TEST(photo_replaced)
{
	photo_write("a.ppm", 64, 48, 0);
	photo_write("b.ppm", 48, 64, 0);
	photo_write("c.ppm", 32, 32, 0);
	const char *paths[] = { PHOTOS "/a.ppm", PHOTOS "/b.ppm" };
	ui_photo_view("Photos", paths, 2, 0, NULL);
	ui_test_settle();
	ui_test_wait(100); /* the neighbor decoded */
	CHECK(ui_test_shows("1 of 2"));
	ui_photo_view_replace(PHOTOS "/a.ppm", PHOTOS "/c.ppm");
	ui_photo_view_replace(PHOTOS "/b.ppm", PHOTOS "/a.ppm");
	ui_photo_view_replace(PHOTOS "/none.ppm", PHOTOS "/a.ppm");
	ui_test_wait(100);
	/* This one and the next, decoded again (none before the first) */
	CHECK_INT(ui_test_find_widgets(&lv_3dtexture_class, NULL, 0), 2);
	drag(-500, 300);
	CHECK(ui_test_shows("2 of 2"));
}

TEST(left_while_decoding)
{
	three_photos();
	CHECK(ui_test_open("Gallery"));
	CHECK(ui_test_back());
	ui_test_wait(200);
	CHECK(view(2));
	CHECK(ui_test_back());
	CHECK(ui_test_back());
	ui_test_wait(200);
}
