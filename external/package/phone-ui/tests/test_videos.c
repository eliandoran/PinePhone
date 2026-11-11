/*
 * Videos (videos.c): the files in /var/lib/videos, played through the fake engine
 * (fake_video.c: a video is a text file of its size, length and color, playing on the
 * tests' clock).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input-event-codes.h>

#include "test.h"
#include "ui_test.h"
#include "modem/modem.h"

#define VIDEOS "/var/lib/videos"

TEST_SETUP
{
	ui_test_start();
}

static lv_obj_t *picture(void)
{
	return ui_test_find_widget(&lv_3dtexture_class);
}

static char *read_file(const char *path)
{
	static char buf[4096];
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = '\0';
	fclose(f);
	return buf;
}

TEST(empty)
{
	CHECK(ui_test_open("Videos"));
	CHECK(ui_test_shows("No videos"));
	CHECK(ui_test_shows_part(VIDEOS));
}

/* Its folder is made at startup and shared with a computer over MTP */
TEST(folder_shared)
{
	const char *list = read_file("/tmp/folders");
	CHECK(list && strstr(list, "\"" VIDEOS "\""));
	CHECK(list && strstr(list, "\"Videos\""));
}

/* Video files by name, without their extension; other files left out */
TEST(list)
{
	ui_test_video(VIDEOS "/Holiday.mp4", 480, 844, 68000, 0x3366cc);
	ui_test_video(VIDEOS "/beach.webm", 640, 360, 5000, 0xcc6633);
	ui_test_video(VIDEOS "/Concert.MKV", 640, 360, 5000, 0xcc6633);
	ui_test_write(VIDEOS "/notes.txt", "not a video");
	CHECK(ui_test_open("Videos"));
	CHECK(ui_test_shows("beach"));
	CHECK(ui_test_shows("Concert"));
	CHECK(ui_test_shows("Holiday"));
	CHECK(!ui_test_shows_part("notes"));
	lv_obj_t *a = ui_test_find("beach"), *b = ui_test_find("Concert"), *c = ui_test_find("Holiday");
	CHECK(a && b && c);
	if (a && b && c) {
		lv_area_t pa, pb, pc;
		lv_obj_get_coords(a, &pa);
		lv_obj_get_coords(b, &pb);
		lv_obj_get_coords(c, &pc);
		CHECK(pa.y1 < pb.y1 && pb.y1 < pc.y1);
	}
}

/* Tapping one plays it: its picture, the time running to its length */
TEST(plays)
{
	ui_test_video(VIDEOS "/Holiday.mp4", 480, 844, 68000, 0x3366cc);
	CHECK(ui_test_open("Videos"));
	CHECK(ui_test_tap_text("Holiday"));
	ui_test_wait(500);
	CHECK(ui_test_shows("Holiday")); /* the title */
	CHECK(ui_test_shows("1:08"));
	CHECK(ui_test_shows(LV_SYMBOL_PAUSE));
	lv_obj_t *p = picture();
	CHECK(p != NULL);
	if (p)
		CHECK(ui_test_shows_color(p, 0x3366cc));
	ui_test_wait(2100);
	CHECK(ui_test_shows("0:02"));
	CHECK(ui_test_videos_open() == 1);
	CHECK_STR(ui_test_video_audio(), "default");
}

TEST(pause_resume)
{
	ui_test_video(VIDEOS "/Holiday.mp4", 480, 844, 68000, 0x3366cc);
	CHECK(ui_test_open("Videos"));
	CHECK(ui_test_tap_text("Holiday"));
	ui_test_wait(1100);
	CHECK(ui_test_tap_text(LV_SYMBOL_PAUSE));
	CHECK(ui_test_shows("0:01"));
	ui_test_wait(3000);
	CHECK(ui_test_shows("0:01"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLAY));
	ui_test_wait(1100);
	CHECK(ui_test_shows("0:02"));
}

/* At the end it stops on the last frame; play starts it again */
TEST(ends)
{
	ui_test_video(VIDEOS "/short.mp4", 320, 240, 3000, 0x33cc66);
	CHECK(ui_test_open("Videos"));
	CHECK(ui_test_tap_text("short"));
	ui_test_wait(3500);
	CHECK(ui_test_shows(LV_SYMBOL_PLAY));
	CHECK(ui_test_shows("0:03"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLAY));
	ui_test_wait(1100);
	CHECK(ui_test_shows("0:01"));
	CHECK(ui_test_shows(LV_SYMBOL_PAUSE));
}

/* Dragging the bar goes where the finger let go */
TEST(seek)
{
	ui_test_video(VIDEOS "/Holiday.mp4", 480, 844, 100000, 0x3366cc);
	CHECK(ui_test_open("Videos"));
	CHECK(ui_test_tap_text("Holiday"));
	ui_test_wait(500);
	CHECK(ui_test_tap_text(LV_SYMBOL_PAUSE));
	lv_obj_t *slider = ui_test_find_widget(&lv_slider_class);
	CHECK(slider != NULL);
	if (!slider)
		return;
	lv_area_t a;
	lv_obj_get_coords(slider, &a);
	int32_t y = (a.y1 + a.y2) / 2;
	ui_test_swipe(a.x1 + 4, y, a.x1 + (a.x2 - a.x1) / 2, y, 300);
	ui_test_wait(200);
	CHECK(ui_test_shows_part("0:4") || ui_test_shows_part("0:5"));
}

/* A file the engine cannot play says so, and leaves */
TEST(broken)
{
	ui_test_write(VIDEOS "/broken.mp4", "garbage");
	CHECK(ui_test_open("Videos"));
	CHECK(ui_test_tap_text("broken"));
	ui_test_wait(300);
	CHECK(ui_test_shows("Can't play this video"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("broken"));
	CHECK(ui_test_videos_open() == 0);
}

/* Leaving the player stops it */
TEST(back_closes)
{
	ui_test_video(VIDEOS "/Holiday.mp4", 480, 844, 68000, 0x3366cc);
	CHECK(ui_test_open("Videos"));
	CHECK(ui_test_tap_text("Holiday"));
	ui_test_wait(500);
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Holiday"));
	CHECK(ui_test_videos_open() == 0);
}

/* The screen stays on while it plays, not once paused */
TEST(stays_awake)
{
	ui_test_video(VIDEOS "/Holiday.mp4", 480, 844, 600000, 0x3366cc);
	CHECK(ui_test_open("Videos"));
	CHECK(ui_test_tap_text("Holiday"));
	ui_test_wait(5000);
	CHECK(lv_display_get_inactive_time(NULL) < 1000);
	CHECK(ui_test_tap_text(LV_SYMBOL_PAUSE));
	ui_test_wait(5000);
	CHECK(lv_display_get_inactive_time(NULL) >= 4000);
}

/* The player turns with the phone, full screen sideways */
TEST(turns_full_screen)
{
	ui_test_video(VIDEOS "/Holiday.mp4", 480, 844, 68000, 0x3366cc);
	CHECK(ui_test_open("Videos"));
	CHECK(!ui_screen_fullscreen(lv_screen_active()));
	CHECK(ui_test_tap_text("Holiday"));
	CHECK(ui_screen_fullscreen(lv_screen_active()));
}

/* The picture the right way up: the frames' black band at the bottom */
TEST(right_way_up)
{
	ui_test_video(VIDEOS "/Holiday.mp4", 480, 800, 68000, 0x3366cc);
	CHECK(ui_test_open("Videos"));
	CHECK(ui_test_tap_text("Holiday"));
	ui_test_wait(500);
	lv_obj_t *p = picture();
	CHECK(p != NULL);
	if (!p)
		return;
	lv_area_t a;
	lv_obj_get_coords(p, &a);
	int32_t x = (a.x1 + a.x2) / 2, h = a.y2 - a.y1;
	lv_color_t top = ui_test_pixel(x, a.y1 + h / 16), bottom = ui_test_pixel(x, a.y2 - h / 32);
	CHECK(top.blue > 150 && top.red < 100);
	CHECK(bottom.blue < 40 && bottom.red < 40 && bottom.green < 40);
}

/* The labels of `text` a user sees (not hidden, nor in a hidden parent) */
static unsigned visible(const char *text)
{
	lv_obj_t *found[8];
	unsigned n = ui_test_find_all(text, found, 8), v = 0;
	for (unsigned i = 0; i < n && i < 8; i++) {
		bool shown = true;
		for (lv_obj_t *o = found[i]; o && shown; o = lv_obj_get_parent(o))
			shown = !lv_obj_is_hidden(o);
		v += shown;
	}
	return v;
}

/* Upright one way back, the header's: the controls' is for sideways, without the header */
TEST(one_back_button_upright)
{
	ui_test_video(VIDEOS "/Holiday.mp4", 480, 844, 68000, 0x3366cc);
	CHECK(ui_test_open("Videos"));
	CHECK(ui_test_tap_text("Holiday"));
	ui_test_wait(300);
	CHECK_INT(visible(LV_SYMBOL_LEFT), 1);
}

/* Locked (Power pressed), it pauses, and stays paused once unlocked */
TEST(pauses_when_locked)
{
	ui_test_video(VIDEOS "/Holiday.mp4", 480, 844, 68000, 0x3366cc);
	CHECK(ui_test_open("Videos"));
	CHECK(ui_test_tap_text("Holiday"));
	ui_test_wait(500);
	CHECK(ui_test_video_playing());
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(300);
	ui_test_settle();
	CHECK(ui_locked());
	CHECK(!ui_test_video_playing());
}

static bool registered(void)
{
	return modem_network() == MODEM_NET_HOME;
}

/* A call coming in covers it with its screen: paused */
TEST(pauses_for_a_call)
{
	ui_test_video(VIDEOS "/Holiday.mp4", 480, 844, 68000, 0x3366cc);
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(registered, 10000));
	CHECK(ui_test_open("Videos"));
	CHECK(ui_test_tap_text("Holiday"));
	ui_test_wait(500);
	CHECK(ui_test_video_playing());
	ui_test_modem_incoming("+14155550132");
	CHECK(ui_test_wait_for("Incoming call", 3000));
	CHECK(!ui_test_video_playing());
}

/* Held so, the display turns */
static bool held(float x, float y, lv_display_rotation_t rotation)
{
	const float up[3] = { x, y, 0 }, field[3] = { 0, 0.2f, -0.4f };
	ui_test_sensors_feed(up, field, 1);
	ui_test_wait(1000);
	return lv_display_get_rotation(lv_display_get_default()) == rotation;
}

/* Sideways: the picture alone on the whole display, no header, no status bar */
TEST(sideways_full_screen)
{
	ui_test_wall_clock(1773481260); /* 14 March 2026, 09:41 UTC */
	ui_test_video(VIDEOS "/Film.mp4", 1920, 1080, 600000, 0x3366cc);
	ui_test_sensors(true, false);
	CHECK(ui_test_open("Videos"));
	CHECK(ui_test_tap_text("Film"));
	ui_test_wait(500);
	CHECK(held(1, 0, LV_DISPLAY_ROTATION_90));
	ui_test_wait(4000); /* the controls hidden */
	CHECK(!ui_test_shows("Film")); /* no header */
	lv_obj_t *p = picture();
	CHECK(p != NULL);
	if (!p)
		return;
	lv_area_t a;
	lv_obj_get_coords(p, &a);
	CHECK_INT(a.x1, 80); /* 16:9 on 2:1, in the middle */
	CHECK_INT(a.y1, 0);
	CHECK_INT(lv_area_get_width(&a), 1280);
	CHECK_INT(lv_area_get_height(&a), 720);
	CHECK(!ui_test_shows("09:41")); /* the status bar's clock */
	/* Upright again: the status bar and the header back */
	CHECK(held(0, 1, LV_DISPLAY_ROTATION_0));
	CHECK(ui_test_shows("09:41"));
	CHECK(ui_test_shows("Film"));
}
