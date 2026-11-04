/*
 * Recorder (recorder.c) on the tests' sound card (fake_alsa.c): recording from the
 * microphone into WAV files, the list, playback, deleting.
 */

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "test.h"
#include "ui_test.h"

#define MEMOS "/var/lib/memos"
#define RATE 48000
#define SETTLE (RATE / 50) /* dropped at the start of a recording */
#define HINT "Tap to record"

TEST_SETUP
{
	ui_test_reset();
	ui_test_microphone(8192); /* -12 dBFS */
}

/* The round red button: an empty label in a 136 px circle */
static lv_obj_t *record_button(void)
{
	lv_obj_t *labels[64];
	unsigned n = ui_test_find_all("", labels, 64);
	for (unsigned i = 0; i < n && i < 64; i++) {
		lv_obj_t *parent = lv_obj_get_parent(labels[i]);
		if (lv_obj_get_width(parent) == 136 && lv_obj_get_height(parent) == 136)
			return parent;
	}
	fprintf(stderr, "  no record button\n");
	return NULL;
}

static bool tap_record(void)
{
	lv_obj_t *b = record_button();
	if (b)
		ui_test_tap(b);
	return b != NULL;
}

static bool record(uint32_t ms)
{
	if (!tap_record())
		return false;
	ui_test_wait(ms);
	return tap_record();
}

/* The files in the memos' folder, in no order */
static unsigned memos(char names[][64], unsigned max)
{
	unsigned n = 0;
	DIR *d = opendir(MEMOS);
	struct dirent *e;
	while (d && (e = readdir(d)))
		if (e->d_name[0] != '.' && n < max)
			snprintf(names[n++], 64, "%.63s", e->d_name);
	if (d)
		closedir(d);
	return n;
}

struct wav {
	uint32_t rate, channels, bits, frames;
	int16_t *samples;
};

static bool wav_read(const char *name, struct wav *w)
{
	char path[128];
	snprintf(path, sizeof(path), MEMOS "/%s", name);
	FILE *f = fopen(path, "rb");
	uint8_t h[44];
	bool ok = f && fread(h, 1, 44, f) == 44 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) &&
	          !memcmp(h + 36, "data", 4);
	if (ok) {
		w->channels = h[22] | h[23] << 8;
		w->rate = h[24] | h[25] << 8 | (uint32_t)h[26] << 16 | (uint32_t)h[27] << 24;
		w->bits = h[34] | h[35] << 8;
		uint32_t bytes = h[40] | h[41] << 8 | (uint32_t)h[42] << 16 | (uint32_t)h[43] << 24;
		w->frames = bytes / (w->channels * 2);
		w->samples = malloc(bytes + 2);
		ok = fread(w->samples, 1, bytes, f) == bytes;
		uint32_t riff = h[4] | h[5] << 8 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 24;
		ok = ok && riff == 36 + bytes && fgetc(f) == EOF;
	}
	if (f)
		fclose(f);
	return ok;
}

/* A memo of 16-bit samples: a ramp, to tell where they went */
static void memo_write(const char *name, unsigned channels, unsigned rate, unsigned frames)
{
	char path[128];
	snprintf(path, sizeof(path), MEMOS "/%s", name);
	ui_test_write(path, "");
	FILE *f = fopen(path, "wb");
	uint32_t bytes = frames * channels * 2;
	uint8_t h[44] = "RIFF";
	uint32_t v = 36 + bytes;
	memcpy(h + 4, &v, 4);
	memcpy(h + 8, "WAVEfmt ", 8);
	v = 16;
	memcpy(h + 16, &v, 4);
	uint16_t s = 1;
	memcpy(h + 20, &s, 2);
	s = (uint16_t)channels;
	memcpy(h + 22, &s, 2);
	memcpy(h + 24, &rate, 4);
	v = rate * channels * 2;
	memcpy(h + 28, &v, 4);
	s = (uint16_t)(channels * 2);
	memcpy(h + 32, &s, 2);
	s = 16;
	memcpy(h + 34, &s, 2);
	memcpy(h + 36, "data", 4);
	memcpy(h + 40, &bytes, 4);
	fwrite(h, 1, 44, f);
	for (unsigned i = 0; i < frames * channels; i++) {
		int16_t x = (int16_t)(i % 20000);
		fwrite(&x, 2, 1, f);
	}
	fclose(f);
}

TEST(nothing_recorded)
{
	CHECK(ui_test_open("Recorder"));
	CHECK(ui_test_shows("No recordings"));
	CHECK(ui_test_shows(HINT));
}

TEST(a_recording)
{
	CHECK(ui_test_open("Recorder"));
	lv_obj_t *b = record_button();
	CHECK(b != NULL);
	if (!b)
		return;
	time_t started = time(NULL);
	ui_test_tap(b);
	CHECK(ui_test_shows("0:00"));
	CHECK(!ui_test_shows(HINT));
	ui_test_wait(2100);
	CHECK(ui_test_shows("0:02"));
	ui_test_wait(1000);
	CHECK(tap_record());
	CHECK(ui_test_shows(HINT));

	char names[4][64];
	CHECK_INT(memos(names, 4), 1);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S.wav", localtime(&started));
	CHECK_STR(names[0], stamp);
	struct wav w;
	CHECK(wav_read(names[0], &w));
	CHECK_INT(w.rate, RATE);
	CHECK_INT(w.channels, 1);
	CHECK_INT(w.bits, 16);
	/* About 3 s of the tone, the ADC's first 20 ms dropped */
	CHECK(w.frames > 3 * RATE - SETTLE - RATE / 10 && w.frames < 3 * RATE + RATE / 5);
	int peak = 0;
	for (uint32_t i = 0; i < w.frames; i++)
		peak = abs(w.samples[i]) > peak ? abs(w.samples[i]) : peak;
	CHECK(peak > 8000 && peak <= 8192);
	free(w.samples);

	/* Listed: when, and how long */
	char title[48];
	strftime(title, sizeof(title), "%-d %B %Y, %H:%M", localtime(&started));
	CHECK(ui_test_shows(title));
	CHECK(ui_test_shows("0:03"));
	CHECK(!ui_test_shows("No recordings"));
}

/* Shorter than half a second: a tap by mistake */
TEST(too_short_dropped)
{
	CHECK(ui_test_open("Recorder"));
	CHECK(record(200));
	char names[4][64];
	CHECK_INT(memos(names, 4), 0);
	CHECK(ui_test_shows("No recordings"));
}

TEST(no_microphone)
{
	ui_test_no_sound_card(true);
	CHECK(ui_test_open("Recorder"));
	CHECK(tap_record());
	CHECK(ui_test_shows("No microphone"));
	char names[4][64];
	CHECK_INT(memos(names, 4), 0);
}

/* Header saved once a second: a power cut keeps a playable file */
TEST(header_kept_while_recording)
{
	CHECK(ui_test_open("Recorder"));
	CHECK(tap_record());
	ui_test_wait(2100);
	char names[4][64];
	CHECK_INT(memos(names, 4), 1);
	struct wav w = { 0 };
	CHECK(!wav_read(names[0], &w) || w.frames <= 2 * RATE); /* the header behind the data */
	free(w.samples);
	char path[128];
	snprintf(path, sizeof(path), MEMOS "/%s", names[0]);
	FILE *f = fopen(path, "rb");
	uint8_t h[44];
	CHECK(f && fread(h, 1, 44, f) == 44 && !memcmp(h, "RIFF", 4));
	uint32_t bytes = h[40] | h[41] << 8 | (uint32_t)h[42] << 16 | (uint32_t)h[43] << 24;
	CHECK(bytes >= RATE * 2); /* a second at least */
	if (f)
		fclose(f);
}

/* Recovered: the recording goes on */
TEST(overrun)
{
	CHECK(ui_test_open("Recorder"));
	CHECK(tap_record());
	ui_test_wait(500);
	ui_test_sound_xrun();
	ui_test_wait(1600);
	CHECK(ui_test_shows("0:02") || ui_test_shows("0:01"));
	CHECK(tap_record());
	char names[4][64];
	CHECK_INT(memos(names, 4), 1);
	struct wav w;
	CHECK(wav_read(names[0], &w) && w.frames > RATE);
	free(w.samples);
}

/* Left while recording: what was recorded is kept */
TEST(left_while_recording)
{
	CHECK(ui_test_open("Recorder"));
	CHECK(tap_record());
	ui_test_wait(2000);
	CHECK(ui_test_back());
	char names[4][64];
	CHECK_INT(memos(names, 4), 1);
	struct wav w;
	CHECK(wav_read(names[0], &w) && w.frames > RATE * 3 / 2);
	free(w.samples);
	CHECK(ui_test_open("Recorder"));
	CHECK(ui_test_shows("0:01") || ui_test_shows("0:02"));
}

TEST(listed_newest_first)
{
	memo_write("20260101-093000.wav", 1, RATE, 5 * RATE);
	memo_write("20261103-174512.wav", 1, RATE, 75 * RATE);
	memo_write("interview.wav", 1, RATE, RATE);
	ui_test_write(MEMOS "/notes.txt", "not a memo");
	CHECK(ui_test_open("Recorder"));
	lv_obj_t *older = ui_test_find("1 January 2026, 09:30");
	lv_obj_t *newer = ui_test_find("3 November 2026, 17:45");
	CHECK(older && newer && ui_test_find("interview"));
	CHECK(ui_test_shows("1:15"));
	CHECK(ui_test_shows("0:05"));
	CHECK(!ui_test_shows_part("notes"));
	if (older && newer) {
		lv_area_t a, b;
		lv_obj_get_coords(newer, &a);
		lv_obj_get_coords(older, &b);
		CHECK(a.y1 < b.y1);
	}
}

/* Mono, played on both channels, to its end */
TEST(played_back)
{
	memo_write("20260101-093000.wav", 1, RATE, 2 * RATE);
	CHECK(ui_test_open("Recorder"));
	CHECK(ui_test_tap_text("1 January 2026, 09:30"));
	CHECK(ui_test_shows(LV_SYMBOL_STOP));
	CHECK(ui_test_wait_for("0:00 / 0:02", 100));
	ui_test_wait(1100);
	CHECK(ui_test_shows("0:01 / 0:02"));
	ui_test_wait(1500);
	CHECK(!ui_test_shows(LV_SYMBOL_STOP));
	CHECK(ui_test_shows("0:02"));

	size_t frames;
	unsigned channels, rate;
	const int16_t *played = ui_test_played(&frames, &channels, &rate);
	CHECK_INT(channels, 2);
	CHECK_INT(rate, RATE);
	CHECK_INT(frames, 2 * RATE);
	bool same = played != NULL;
	for (size_t i = 0; played && i < frames; i++)
		same &= played[2 * i] == (int16_t)(i % 20000) && played[2 * i + 1] == (int16_t)(i % 20000);
	CHECK(same);
}

TEST(stereo_at_another_rate)
{
	memo_write("20260101-093000.wav", 2, 44100, RATE / 4);
	CHECK(ui_test_open("Recorder"));
	CHECK(ui_test_tap_text("1 January 2026, 09:30"));
	ui_test_wait(1000);
	size_t frames;
	unsigned channels, rate;
	const int16_t *played = ui_test_played(&frames, &channels, &rate);
	CHECK_INT(rate, 44100);
	CHECK_INT(frames, RATE / 4);
	CHECK(played && played[1] == 1 && played[2 * 1000 + 1] == 2001);
	CHECK(!ui_test_shows(LV_SYMBOL_STOP));
}

TEST(stopped_by_a_tap)
{
	memo_write("20260101-093000.wav", 1, RATE, 10 * RATE);
	CHECK(ui_test_open("Recorder"));
	CHECK(ui_test_tap_text("1 January 2026, 09:30"));
	ui_test_wait(500);
	CHECK(ui_test_tap_text("1 January 2026, 09:30"));
	CHECK(!ui_test_shows(LV_SYMBOL_STOP));
	CHECK(ui_test_shows("0:10"));
}

/* Recovered from an underrun, played to the end */
TEST(underrun)
{
	memo_write("20260101-093000.wav", 1, RATE, 2 * RATE);
	CHECK(ui_test_open("Recorder"));
	CHECK(ui_test_tap_text("1 January 2026, 09:30"));
	ui_test_wait(500);
	ui_test_sound_xrun();
	ui_test_wait(3000);
	CHECK(!ui_test_shows(LV_SYMBOL_STOP));
}

TEST(not_a_memo)
{
	ui_test_write(MEMOS "/20260101-093000.wav", "RIFF but not really a WAV file at all, no");
	CHECK(ui_test_open("Recorder"));
	CHECK(ui_test_tap_text("1 January 2026, 09:30"));
	CHECK(!ui_test_shows(LV_SYMBOL_STOP));
}

TEST(recording_stops_playback)
{
	memo_write("20260101-093000.wav", 1, RATE, 10 * RATE);
	CHECK(ui_test_open("Recorder"));
	CHECK(ui_test_tap_text("1 January 2026, 09:30"));
	CHECK(ui_test_shows(LV_SYMBOL_STOP));
	CHECK(tap_record());
	CHECK(!ui_test_shows(LV_SYMBOL_STOP));
	CHECK(ui_test_shows("0:00"));
	/* No playing while recording */
	CHECK(ui_test_tap_text("1 January 2026, 09:30"));
	CHECK(!ui_test_shows(LV_SYMBOL_STOP));
	CHECK(tap_record());
}

TEST(left_while_playing)
{
	memo_write("20260101-093000.wav", 1, RATE, 10 * RATE);
	CHECK(ui_test_open("Recorder"));
	CHECK(ui_test_tap_text("1 January 2026, 09:30"));
	ui_test_wait(300);
	CHECK(ui_test_back());
	ui_test_wait(500);
	CHECK(ui_at_home());
}

TEST(deleted)
{
	memo_write("20260101-093000.wav", 1, RATE, 2 * RATE);
	memo_write("20260102-093000.wav", 1, RATE, 2 * RATE);
	CHECK(ui_test_open("Recorder"));
	lv_obj_t *trash[4];
	CHECK_INT(ui_test_find_all(LV_SYMBOL_TRASH, trash, 4), 2);
	ui_test_tap(trash[1]); /* the older one */
	CHECK(ui_test_tap_text("Cancel"));
	CHECK(ui_test_shows("1 January 2026, 09:30"));

	CHECK_INT(ui_test_find_all(LV_SYMBOL_TRASH, trash, 4), 2);
	ui_test_tap(trash[1]);
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH "  Delete"));
	CHECK(!ui_test_shows("1 January 2026, 09:30"));
	CHECK(ui_test_shows("2 January 2026, 09:30"));

	/* The last one, while it plays */
	CHECK(ui_test_tap_text("2 January 2026, 09:30"));
	CHECK_INT(ui_test_find_all(LV_SYMBOL_TRASH, trash, 4), 1);
	ui_test_tap(trash[0]);
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH "  Delete"));
	CHECK(ui_test_shows("No recordings"));
	char names[4][64];
	CHECK_INT(memos(names, 4), 0);
}

/* Two in the same second */
TEST(names_do_not_collide)
{
	time_t now = time(NULL);
	char stamp[48];
	for (int i = 0; i < 3; i++) {
		time_t t = now + i;
		strftime(stamp, sizeof(stamp), MEMOS "/%Y%m%d-%H%M%S.wav", localtime(&t));
		ui_test_write(stamp, "");
	}
	CHECK(ui_test_open("Recorder"));
	CHECK(record(1000));
	char names[8][64];
	unsigned n = memos(names, 8);
	CHECK_INT(n, 4);
	bool suffixed = false;
	for (unsigned i = 0; i < n; i++)
		suffixed |= strstr(names[i], "-2.wav") != NULL;
	CHECK(suffixed);
}
