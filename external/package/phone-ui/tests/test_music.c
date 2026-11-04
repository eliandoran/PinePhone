/*
 * Music (music.c): the library read from MP3 files the tests write (ID3v2 and v1
 * tags, a cover, silent frames of a known length), and playback through the fake
 * mpg123 (ui_test.h).
 */

#define _GNU_SOURCE /* open_memstream */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test.h"
#include "ui_test.h"

#define MUSIC "/var/lib/music"
#define RATE 44100
#define FRAME_BYTES 417 /* MPEG-1 layer III, 128 kbit/s, 44.1 kHz */
#define FRAME_SAMPLES 1152

struct tags {
	const char *title, *artist, *album, *track;
	bool cover;
};

static void syncsafe(uint8_t *p, uint32_t v)
{
	p[0] = (v >> 21) & 0x7f;
	p[1] = (v >> 14) & 0x7f;
	p[2] = (v >> 7) & 0x7f;
	p[3] = v & 0x7f;
}

/* An ID3v2.4 frame: header, then its data */
static void id3_frame(FILE *f, const char *id, const void *data, uint32_t size)
{
	uint8_t h[10] = { 0 };
	memcpy(h, id, 4);
	syncsafe(h + 4, size);
	fwrite(h, 1, 10, f);
	fwrite(data, 1, size, f);
}

static void id3_text(FILE *f, const char *id, const char *text)
{
	if (!text)
		return;
	char buf[256];
	buf[0] = 3; /* UTF-8 */
	size_t n = strlen(text);
	memcpy(buf + 1, text, n);
	id3_frame(f, id, buf, (uint32_t)n + 1);
}

/* A JPEG cover, red */
static void id3_cover(FILE *f)
{
	uint8_t *px = malloc(200 * 120 * 4);
	for (int i = 0; i < 200 * 120; i++)
		((uint32_t *)px)[i] = 0xffff0000;
	char *jpeg = NULL;
	size_t size = 0;
	FILE *m = open_memstream(&jpeg, &size);
	ui_jpeg_write(m, px, 200, 120, 200 * 4, 90, true);
	fclose(m);
	free(px);
	static const char head[] = "\0image/jpeg\0\3"; /* Latin-1, MIME, front cover */
	char *data = malloc(sizeof(head) + size); /* the description: the "" at its end */
	memcpy(data, head, sizeof(head));
	memcpy(data + sizeof(head), jpeg, size);
	id3_frame(f, "APIC", data, (uint32_t)(sizeof(head) + size));
	free(data);
	free(jpeg);
}

/* An MP3 of silence, `seconds` long: ID3v2 tags (NULL: none), ID3v1 ones if v1 */
static void mp3_write(const char *name, unsigned seconds, const struct tags *v2, const struct tags *v1)
{
	char path[256];
	snprintf(path, sizeof(path), MUSIC "/%s", name);
	ui_test_write(path, "");
	FILE *f = fopen(path, "wb");
	if (v2) {
		char *tags = NULL;
		size_t size = 0;
		FILE *t = open_memstream(&tags, &size);
		id3_text(t, "TIT2", v2->title);
		id3_text(t, "TPE1", v2->artist);
		id3_text(t, "TALB", v2->album);
		id3_text(t, "TRCK", v2->track);
		if (v2->cover)
			id3_cover(t);
		fclose(t);
		uint8_t h[10] = { 'I', 'D', '3', 4, 0, 0 };
		syncsafe(h + 6, (uint32_t)size);
		fwrite(h, 1, 10, f);
		fwrite(tags, 1, size, f);
		free(tags);
	}
	uint8_t frame[FRAME_BYTES] = { 0xff, 0xfb, 0x90, 0xc0 }; /* mono, the rest silence */
	unsigned frames = (seconds * RATE + FRAME_SAMPLES - 1) / FRAME_SAMPLES;
	for (unsigned i = 0; i < frames; i++)
		fwrite(frame, 1, sizeof(frame), f);
	if (v1) {
		char tag[128] = "TAG";
		strncpy(tag + 3, v1->title ? v1->title : "", 30);
		strncpy(tag + 33, v1->artist ? v1->artist : "", 30);
		strncpy(tag + 63, v1->album ? v1->album : "", 30);
		tag[127] = (char)255;
		fwrite(tag, 1, 128, f);
	}
	fclose(f);
}

static void library(void)
{
	mp3_write("b.mp3", 125, &(struct tags){ "Second", "The Band", "Blue Album", "2/10" }, NULL);
	mp3_write("a.mp3", 65, &(struct tags){ "First", "The Band", "Blue Album", "1" }, NULL);
	mp3_write("Old/c.mp3", 42, &(struct tags){ "Ancient", "Someone", "Antiques", NULL }, NULL);
	mp3_write("loose track.mp3", 30, NULL, NULL);
}

/* The position set reached phone-ui: a SAMPLE answered with it, then the next one
 * asked, once phone-ui read what came before */
static void position_known(void)
{
	ui_test_mpg123_forget();
	CHECK(ui_test_mpg123_got("SAMPLE", 2000));
	ui_test_mpg123_forget();
	CHECK(ui_test_mpg123_got("SAMPLE", 2000));
}

TEST_SETUP
{
	ui_test_reset();
}

TEST(no_music)
{
	CHECK(ui_test_open("Music"));
	CHECK(ui_test_shows("No music"));
	CHECK(ui_test_shows("Copy MP3 files to " MUSIC));
}

/* By album (untagged last), then track number */
TEST(library_by_album)
{
	library();
	/* Latin-1 ID3v1 only */
	mp3_write("v1.mp3", 10, NULL, &(struct tags){ "Caf\xe9", "V. One", "Antiques", NULL });
	ui_test_write(MUSIC "/notes.txt", "not music");
	ui_test_write(MUSIC "/broken.mp3", "not an MP3 at all");
	CHECK(ui_test_open("Music"));
	static const char *const order[] = {
		"Antiques", "Ancient", "Caf\xc3\xa9", "Blue Album", "First", "Second", "Unknown album", "loose track",
	};
	int32_t y = -1;
	for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
		lv_obj_t *label = ui_test_find(order[i]);
		CHECK(label != NULL);
		if (!label)
			continue;
		lv_area_t a;
		lv_obj_get_coords(label, &a);
		if (a.y1 <= y)
			fprintf(stderr, "  \"%s\" out of order\n", order[i]);
		CHECK(a.y1 > y);
		y = a.y1;
	}
	CHECK(ui_test_shows("1:05"));
	CHECK(ui_test_shows("2:05"));
	CHECK(ui_test_shows("0:42"));
	CHECK(ui_test_shows("Unknown artist"));
	CHECK(ui_test_shows("V. One"));
	CHECK(!ui_test_shows_part("broken"));
}

TEST(played)
{
	library();
	CHECK(ui_test_open("Music"));
	CHECK(ui_test_tap_text("First"));
	CHECK(ui_test_shows("Now Playing"));
	CHECK(ui_test_shows("Blue Album"));
	CHECK(ui_test_shows("The Band"));
	CHECK(ui_test_shows(LV_SYMBOL_PAUSE));
	CHECK(ui_test_mpg123_got("SILENCE", 1000));
	CHECK(ui_test_mpg123_got("LOAD " MUSIC "/a.mp3", 1000));
	CHECK(ui_test_shows("0:00"));
	CHECK(ui_test_shows("-1:05"));

	ui_test_mpg123_at(30LL * RATE, 65LL * RATE);
	CHECK(ui_test_wait_for("0:30", 3000));
	CHECK(ui_test_shows("-0:35"));

	CHECK(ui_test_tap_text(LV_SYMBOL_PAUSE));
	CHECK(ui_test_mpg123_got("PAUSE", 1000));
	CHECK(ui_test_shows(LV_SYMBOL_PLAY));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLAY));
	CHECK(ui_test_shows(LV_SYMBOL_PAUSE));
}

TEST(next_and_previous)
{
	library();
	CHECK(ui_test_open("Music"));
	CHECK(ui_test_tap_text("First"));
	CHECK(ui_test_tap_text(LV_SYMBOL_NEXT));
	CHECK(ui_test_mpg123_got("LOAD " MUSIC "/b.mp3", 1000));
	CHECK(ui_test_shows("Second"));
	/* Into a song: back to its start */
	ui_test_mpg123_at(10LL * RATE, 125LL * RATE);
	CHECK(ui_test_wait_for("0:10", 3000));
	ui_test_mpg123_forget();
	CHECK(ui_test_tap_text(LV_SYMBOL_PREV));
	CHECK(ui_test_mpg123_got("LOAD " MUSIC "/b.mp3", 1000));
	/* At its start: the one before */
	ui_test_mpg123_at(0, 125LL * RATE);
	position_known();
	CHECK(ui_test_tap_text(LV_SYMBOL_PREV));
	CHECK(ui_test_mpg123_got("LOAD " MUSIC "/a.mp3", 1000));
	CHECK(ui_test_shows("First"));
}

/* The next one at the end of a song; the end of the list stops, unless repeating */
TEST(end_of_songs)
{
	library();
	CHECK(ui_test_open("Music"));
	CHECK(ui_test_tap_text("loose track")); /* the last */
	CHECK(ui_test_mpg123_got("LOAD " MUSIC "/loose track.mp3", 1000));
	ui_test_mpg123_end();
	CHECK(ui_test_mpg123_got("LOADPAUSED " MUSIC "/Old/c.mp3", 3000));
	CHECK(ui_test_shows("Ancient"));
	CHECK(ui_test_shows(LV_SYMBOL_PLAY));

	CHECK(ui_test_tap_text(LV_SYMBOL_LOOP));
	for (int i = 0; i < 3; i++)
		CHECK(ui_test_tap_text(LV_SYMBOL_NEXT));
	CHECK(ui_test_shows("loose track"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLAY));
	ui_test_mpg123_forget();
	ui_test_mpg123_end();
	CHECK(ui_test_mpg123_got("LOAD " MUSIC "/Old/c.mp3", 3000));
	CHECK(ui_test_shows(LV_SYMBOL_PAUSE));
}

/* The song shown in Now Playing */
static const char *playing_title(void)
{
	static const char *const titles[] = { "First", "Second", "Ancient", "loose track" };
	for (size_t i = 0; i < 4; i++)
		if (ui_test_shows(titles[i]))
			return titles[i];
	return "";
}

TEST(shuffle)
{
	library();
	CHECK(ui_test_open("Music"));
	CHECK(ui_test_tap_text("First"));
	CHECK(ui_test_tap_text(LV_SYMBOL_SHUFFLE));
	for (int i = 0; i < 8; i++) {
		const char *before = playing_title();
		CHECK(ui_test_tap_text(LV_SYMBOL_NEXT));
		CHECK(strcmp(playing_title(), before) != 0); /* another one */
	}
}

TEST(seek)
{
	library();
	CHECK(ui_test_open("Music"));
	CHECK(ui_test_tap_text("Second"));
	lv_obj_t *slider = ui_test_find_widget(&lv_slider_class);
	CHECK(slider != NULL);
	if (!slider)
		return;
	lv_area_t a;
	lv_obj_get_coords(slider, &a);
	int32_t y = (a.y1 + a.y2) / 2;
	ui_test_swipe(a.x1, y, a.x1 + (a.x2 - a.x1) * 60 / 125, y, 300);
	/* 60 s of 125, where the finger let go */
	CHECK(ui_test_mpg123_got("JUMP 60s", 1000));
}

/* The music goes on after leaving: the bar under the list */
TEST(goes_on_after_leaving)
{
	library();
	CHECK(ui_test_open("Music"));
	CHECK(ui_test_tap_text("First"));
	CHECK(ui_test_back());
	CHECK(ui_test_back());
	CHECK(ui_at_home());
	CHECK(ui_test_open("Music"));
	lv_obj_t *found[4];
	CHECK_INT(ui_test_find_all("First", found, 4), 2); /* its row, and the bar */
	CHECK(!ui_test_mpg123_got("STOP", 300));
	/* The bar opens Now Playing, plays and skips */
	ui_test_tap(found[1]);
	CHECK(ui_test_shows("Now Playing"));
	CHECK(ui_test_back());
	ui_test_mpg123_forget();
	lv_obj_t *next[4];
	unsigned n = ui_test_find_all(LV_SYMBOL_NEXT, next, 4);
	CHECK(n >= 1);
	if (n)
		ui_test_tap(next[0]);
	CHECK(ui_test_mpg123_got("LOAD " MUSIC "/b.mp3", 1000));
}

/* mpg123 killed: play starts it again where it was */
TEST(mpg123_gone)
{
	library();
	CHECK(ui_test_open("Music"));
	CHECK(ui_test_tap_text("First"));
	ui_test_mpg123_at(20LL * RATE, 65LL * RATE);
	CHECK(ui_test_wait_for("0:20", 3000));
	ui_test_mpg123_kill();
	CHECK(ui_test_wait_for(LV_SYMBOL_PLAY, 3000));
	remove("/tmp/ui-test-bin/mpg123.kill");
	ui_test_mpg123_forget();
	CHECK(ui_test_tap_text(LV_SYMBOL_PLAY));
	CHECK(ui_test_mpg123_got("LOAD " MUSIC "/a.mp3", 1000));
	CHECK(ui_test_mpg123_got("JUMP 20s", 1000));
}

TEST(cover)
{
	mp3_write("a.mp3", 20, &(struct tags){ "Covered", "Artist", "With Cover", "1", true }, NULL);
	mp3_write("b.mp3", 20, &(struct tags){ "Bare", "Artist", "Without", "1" }, NULL);
	CHECK(ui_test_open("Music"));
	CHECK(ui_test_tap_text("Covered"));
	lv_obj_t *art = ui_test_find_widget(&lv_3dtexture_class);
	CHECK(art != NULL);
	if (art) {
		lv_area_t a;
		lv_obj_get_coords(art, &a);
		lv_color_t c = ui_test_pixel((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
		CHECK(c.red > 200 && c.green < 60 && c.blue < 60);
	}
	CHECK(ui_test_back());
	CHECK(ui_test_tap_text("Bare"));
	CHECK(ui_test_find_widget(&lv_3dtexture_class) == NULL); /* the record instead */
}

/* The song playing removed: it stops when the library is read again */
TEST(song_removed)
{
	library();
	CHECK(ui_test_open("Music"));
	CHECK(ui_test_tap_text("First"));
	CHECK(ui_test_back());
	CHECK(ui_test_back());
	unlink(MUSIC "/a.mp3");
	CHECK(ui_test_open("Music"));
	CHECK(ui_test_mpg123_got("STOP", 1000));
	CHECK(!ui_test_shows("First"));
}

/* A song that cannot be opened stops */
TEST(song_gone_while_listed)
{
	library();
	CHECK(ui_test_open("Music"));
	unlink(MUSIC "/a.mp3");
	CHECK(ui_test_tap_text("First"));
	CHECK(ui_test_wait_for(LV_SYMBOL_PLAY, 3000));
}
