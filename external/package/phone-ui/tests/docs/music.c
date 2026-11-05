/* docs/apps/music.md's screenshots (included by test_docs.c) */

#define MUSIC_DIR "/var/lib/music"
#define MUSIC_RATE 44100
#define MUSIC_FRAME_BYTES 417 /* MPEG-1 layer III, 128 kbit/s, 44.1 kHz */
#define MUSIC_FRAME_SAMPLES 1152

struct music_song {
	const char *file, *title, *artist, *album, *track;
	unsigned seconds;
	uint32_t from, to; /* the cover's colors */
};

static void music_syncsafe(uint8_t *p, uint32_t v)
{
	p[0] = (v >> 21) & 0x7f;
	p[1] = (v >> 14) & 0x7f;
	p[2] = (v >> 7) & 0x7f;
	p[3] = v & 0x7f;
}

static void music_frame(FILE *f, const char *id, const void *data, uint32_t size)
{
	uint8_t h[10] = { 0 };
	memcpy(h, id, 4);
	music_syncsafe(h + 4, size);
	fwrite(h, 1, 10, f);
	fwrite(data, 1, size, f);
}

static void music_text(FILE *f, const char *id, const char *text)
{
	char buf[256];
	buf[0] = 3; /* UTF-8 */
	size_t n = strlen(text);
	memcpy(buf + 1, text, n);
	music_frame(f, id, buf, (uint32_t)n + 1);
}

/* A made-up cover: a diagonal gradient with rings around a sun (gallery_mix(): docs/gallery.c) */
static void music_cover(FILE *f, uint32_t from, uint32_t to)
{
	enum { S = 400 };
	uint8_t *px = malloc(S * S * 4);
	for (int y = 0; y < S; y++) {
		for (int x = 0; x < S; x++) {
			float t = (x + y) / (2.0f * S);
			float d = hypotf(x - S * 0.5f, y - S * 0.62f) / S;
			uint32_t c = gallery_mix(from, to, t);
			if (d < 0.16f)
				c = gallery_mix(0xffffff, c, d / 0.16f * 0.5f);
			else if (d < 0.45f && ((int)(d * 60) & 1))
				c = gallery_mix(c, 0xffffff, 0.18f);
			uint8_t *p = px + ((size_t)y * S + x) * 4;
			p[0] = c & 0xff;
			p[1] = (c >> 8) & 0xff;
			p[2] = (c >> 16) & 0xff;
			p[3] = 0xff;
		}
	}
	char *jpeg = NULL;
	size_t size = 0;
	FILE *m = open_memstream(&jpeg, &size);
	ui_jpeg_write(m, px, S, S, S * 4, 90, false);
	fclose(m);
	free(px);
	static const char head[] = "\0image/jpeg\0\3"; /* Latin-1, MIME, front cover, "" */
	char *data = malloc(sizeof(head) + size);
	memcpy(data, head, sizeof(head));
	memcpy(data + sizeof(head), jpeg, size);
	music_frame(f, "APIC", data, (uint32_t)(sizeof(head) + size));
	free(data);
	free(jpeg);
}

/* An MP3 of silence with ID3v2.4 tags and a cover */
static void music_write(const struct music_song *s)
{
	char path[256];
	snprintf(path, sizeof(path), MUSIC_DIR "/%s", s->file);
	ui_test_write(path, "");
	FILE *f = fopen(path, "wb");
	char *tags = NULL;
	size_t size = 0;
	FILE *t = open_memstream(&tags, &size);
	music_text(t, "TIT2", s->title);
	music_text(t, "TPE1", s->artist);
	music_text(t, "TALB", s->album);
	music_text(t, "TRCK", s->track);
	music_cover(t, s->from, s->to);
	fclose(t);
	uint8_t h[10] = { 'I', 'D', '3', 4, 0, 0 };
	music_syncsafe(h + 6, (uint32_t)size);
	fwrite(h, 1, 10, f);
	fwrite(tags, 1, size, f);
	free(tags);
	uint8_t frame[MUSIC_FRAME_BYTES] = { 0xff, 0xfb, 0x90, 0xc0 }; /* mono, silence */
	unsigned frames = (s->seconds * MUSIC_RATE + MUSIC_FRAME_SAMPLES - 1) / MUSIC_FRAME_SAMPLES;
	for (unsigned i = 0; i < frames; i++)
		fwrite(frame, 1, sizeof(frame), f);
	fclose(f);
}

#define MUSIC_LANTERNS "The Paper Lanterns/Harbour Lights/"
#define MUSIC_KOLLIN "Mira Kollin/Northern Static/"

static const struct music_song music_songs[] = {
	{ MUSIC_LANTERNS "01 Low Tide.mp3", "Low Tide", "The Paper Lanterns", "Harbour Lights", "1/4", 222, 0x0b3d6b, 0xf28c38 },
	{ MUSIC_LANTERNS "02 Harbour Lights.mp3", "Harbour Lights", "The Paper Lanterns", "Harbour Lights", "2/4", 245, 0x0b3d6b, 0xf28c38 },
	{ MUSIC_LANTERNS "03 Saltwater Letters.mp3", "Saltwater Letters", "The Paper Lanterns", "Harbour Lights", "3/4", 198, 0x0b3d6b, 0xf28c38 },
	{ MUSIC_LANTERNS "04 Lighthouse Keeper.mp3", "Lighthouse Keeper", "The Paper Lanterns", "Harbour Lights", "4/4", 291, 0x0b3d6b, 0xf28c38 },
	{ MUSIC_KOLLIN "01 Radio Snow.mp3", "Radio Snow", "Mira Kollin", "Northern Static", "1", 231, 0x1d1b4f, 0x5fd3c7 },
	{ MUSIC_KOLLIN "02 Glass Mountains.mp3", "Glass Mountains", "Mira Kollin", "Northern Static", "2", 267, 0x1d1b4f, 0x5fd3c7 },
	{ MUSIC_KOLLIN "03 Polar Night Drive.mp3", "Polar Night Drive", "Mira Kollin", "Northern Static", "3", 302, 0x1d1b4f, 0x5fd3c7 },
};

/* The library, then "Saltwater Letters" playing, 1:24 in */
static bool music_playing(void)
{
	for (size_t i = 0; i < sizeof(music_songs) / sizeof(music_songs[0]); i++)
		music_write(&music_songs[i]);
	if (!ui_test_open("Music") || !ui_test_tap_text("Saltwater Letters"))
		return false;
	CHECK(ui_test_shows("Now Playing"));
	CHECK(ui_test_mpg123_got("LOAD " MUSIC_DIR "/" MUSIC_LANTERNS "03 Saltwater Letters.mp3", 2000));
	ui_test_mpg123_at(84LL * MUSIC_RATE, 199LL * MUSIC_RATE);
	return ui_test_wait_for("1:24", 3000);
}

/* music.md: Now Playing, the cover from the file */
TEST(music_now_playing)
{
	CHECK(music_playing());
	CHECK(ui_test_shows("Harbour Lights"));     /* the album */
	CHECK(ui_test_shows("Saltwater Letters"));
	CHECK(ui_test_shows("The Paper Lanterns"));
	CHECK(ui_test_shows("-1:55"));
	CHECK(ui_test_shows(LV_SYMBOL_PAUSE));
	CHECK(ui_test_shows(LV_SYMBOL_SHUFFLE));
	CHECK(ui_test_shows(LV_SYMBOL_LOOP));
	CHECK(ui_test_find_widget(&lv_slider_class) != NULL);
	CHECK(ui_test_find_widget(&lv_3dtexture_class) != NULL); /* the cover */
	shot("music-now-playing");
}

/* music.md: the library by album, the song playing at the bottom */
TEST(music_library)
{
	CHECK(music_playing());
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Music"));
	CHECK(ui_test_shows("Harbour Lights"));
	CHECK(ui_test_shows("Northern Static"));
	CHECK(ui_test_shows("3:19")); /* as estimated from the file, as mpg123 counts it */
	CHECK(ui_test_shows("Mira Kollin"));
	/* Albums in order, songs by number */
	lv_obj_t *low = ui_test_find("Low Tide"), *keeper = ui_test_find("Lighthouse Keeper");
	lv_obj_t *radio = ui_test_find("Radio Snow");
	CHECK(low && keeper && radio);
	if (low && keeper && radio) {
		lv_area_t a, b, c;
		lv_obj_get_coords(low, &a);
		lv_obj_get_coords(keeper, &b);
		lv_obj_get_coords(radio, &c);
		CHECK(a.y1 < b.y1 && b.y1 < c.y1);
	}
	/* The bar of the song playing, with its pause and next buttons: its title in
	 * its row and in the bar */
	CHECK(ui_test_shows(LV_SYMBOL_PAUSE));
	CHECK(ui_test_shows(LV_SYMBOL_NEXT));
	lv_obj_t *found[4];
	CHECK_INT(ui_test_find_all("Saltwater Letters", found, 4), 2);
	shot("music-library");
}
