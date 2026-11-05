/* docs/apps/recorder.md's screenshots (included by test_docs.c) */

#define RECORDER_MEMOS "/var/lib/memos"
#define RECORDER_RATE 48000

/* A memo `seconds` long, as the Recorder writes them (48 kHz, 16-bit mono), named
 * after when it started: "20260313-183005" */
static void recorder_memo(const char *stamp, unsigned seconds)
{
	char path[128];
	snprintf(path, sizeof(path), RECORDER_MEMOS "/%s.wav", stamp);
	ui_test_write(path, "");
	FILE *f = fopen(path, "wb");
	uint32_t data = seconds * RECORDER_RATE * 2;
	uint8_t h[44] = "RIFF....WAVEfmt ";
	uint32_t v[] = { 16, 1 | 1 << 16, RECORDER_RATE, RECORDER_RATE * 2, 2 | 16 << 16 };
	memcpy(h + 4, &(uint32_t){ 36 + data }, 4); /* little-endian, as the phone */
	memcpy(h + 16, v, sizeof(v));
	memcpy(h + 36, "data", 4);
	memcpy(h + 40, &data, 4);
	fwrite(h, 1, sizeof(h), f);
	static const uint8_t silence[RECORDER_RATE * 2];
	for (unsigned i = 0; i < seconds; i++)
		fwrite(silence, 1, sizeof(silence), f);
	fclose(f);
}

static void recorder_memos(void)
{
	recorder_memo("20260314-085210", 47);
	recorder_memo("20260313-183005", 133);
	recorder_memo("20260311-121540", 12);
	recorder_memo("20260307-094402", 331);
}

/* The round red button (an empty label in a 136 px circle) */
static bool recorder_tap_record(void)
{
	lv_obj_t *labels[64];
	unsigned n = ui_test_find_all("", labels, 64);
	for (unsigned i = 0; i < n && i < 64; i++) {
		lv_obj_t *parent = lv_obj_get_parent(labels[i]);
		if (lv_obj_get_width(parent) == 136 && lv_obj_get_height(parent) == 136) {
			ui_test_tap(parent);
			return true;
		}
	}
	fprintf(stderr, "  no record button\n");
	return false;
}

/* recorder.md: the recordings, newest first, one playing */
TEST(recorder_list)
{
	recorder_memos();
	CHECK(ui_test_open("Recorder"));
	CHECK(ui_test_shows("Tap to record"));
	CHECK(ui_test_shows("14 March 2026, 08:52"));
	CHECK(ui_test_shows("0:47"));
	CHECK(ui_test_shows("2:13"));
	CHECK(ui_test_shows("7 March 2026, 09:44"));
	CHECK(ui_test_shows("5:31"));
	lv_obj_t *a = ui_test_find("14 March 2026, 08:52"), *b = ui_test_find("13 March 2026, 18:30");
	CHECK(a && b);
	if (a && b) {
		lv_area_t pa, pb;
		lv_obj_get_coords(a, &pa);
		lv_obj_get_coords(b, &pb);
		CHECK(pa.y1 < pb.y1);
	}
	/* A tap plays it, its time running; the play button turns into stop */
	CHECK(ui_test_tap_text("13 March 2026, 18:30"));
	CHECK(ui_test_wait_for("0:01 / 2:13", 3000));
	CHECK(ui_test_shows(LV_SYMBOL_STOP));
	shot("recorder-list");
	/* The trash asks first */
	lv_obj_t *trash[8];
	CHECK_INT(ui_test_find_all(LV_SYMBOL_TRASH, trash, 8), 4);
	ui_test_tap(trash[3]);
	CHECK(ui_test_shows(LV_SYMBOL_TRASH "  Delete"));
	CHECK(ui_test_tap_text("Cancel"));
	CHECK(!ui_test_shows(LV_SYMBOL_TRASH "  Delete"));
	CHECK(ui_test_shows("7 March 2026, 09:44"));
}

/* recorder.md: recording, the time and the level */
TEST(recorder_recording)
{
	recorder_memos();
	ui_test_microphone(8192);
	CHECK(ui_test_open("Recorder"));
	CHECK(recorder_tap_record());
	CHECK(!ui_test_shows("Tap to record"));
	CHECK(ui_test_wait_for("0:03", 5000));
	shot("recorder-recording");
	/* Stopped: saved, at the top of the list as now */
	CHECK(recorder_tap_record());
	CHECK(ui_test_shows("Tap to record"));
	CHECK(ui_test_shows("14 March 2026, 09:41"));
}
