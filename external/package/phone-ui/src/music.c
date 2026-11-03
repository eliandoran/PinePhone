/*
 * Music: a made-up library of songs and a Now Playing screen. Nothing is played:
 * a timer moves the position along, and playback goes on after leaving the app.
 */

#include <stdlib.h>
#include <string.h>

#include "ui.h"

#define ROW_H 112
#define TICK_MS 250
#define EQ_BARS 3

struct track {
	const char *title, *artist, *album;
	uint32_t seconds;
	uint32_t color; /* of the album's cover */
};

static const struct track tracks[] = {
	{ "Harbour Lights",     "The Low Tides",    "Salt and Static", 214, 0x0a84ff },
	{ "Paper Satellites",   "The Low Tides",    "Salt and Static", 187, 0x0a84ff },
	{ "Undertow",           "The Low Tides",    "Salt and Static", 251, 0x0a84ff },
	{ "Midnight Tram",      "Vera Lindqvist",   "Northbound",      203, 0xbf5af2 },
	{ "Glass Orchard",      "Vera Lindqvist",   "Northbound",      239, 0xbf5af2 },
	{ "Copper Sky",         "Vera Lindqvist",   "Northbound",      176, 0xbf5af2 },
	{ "Slow Engines",       "Kites Over Kyiv",  "Long Exposure",   312, 0xff9f0a },
	{ "Static Bloom",       "Kites Over Kyiv",  "Long Exposure",   228, 0xff9f0a },
	{ "Fifth Floor Window", "Marco Aurelio",    "Late Shift",      195, 0x30d158 },
	{ "Neon Pharmacy",      "Marco Aurelio",    "Late Shift",      167, 0x30d158 },
	{ "Last Ferry Home",    "Marco Aurelio",    "Late Shift",      264, 0x30d158 },
	{ "Clockwork Heart",    "June & The Moths", "Lanterns",        222, 0xff453a },
	{ "Small Hours",        "June & The Moths", "Lanterns",        189, 0xff453a },
	{ "Ember",              "June & The Moths", "Lanterns",        281, 0xff453a },
};
#define TRACK_COUNT (sizeof(tracks) / sizeof(tracks[0]))

/* Playback, kept while the app is closed */
static struct {
	bool started; /* something was picked */
	bool playing, shuffle, repeat;
	unsigned track;
	uint32_t pos_ms;
	lv_timer_t *timer;
} player;

/* The library screen */
static struct {
	lv_obj_t *scr;
	lv_obj_t *eq[TRACK_COUNT]; /* bars next to the current song */
	lv_obj_t *bars[TRACK_COUNT][EQ_BARS];
	lv_obj_t *mini, *mini_title, *mini_artist, *mini_play;
} lib;

/* The Now Playing screen */
static struct {
	lv_obj_t *scr;
	lv_obj_t *cover, *hole, *album;
	lv_obj_t *title, *artist;
	lv_obj_t *slider, *elapsed, *remaining;
	lv_obj_t *play, *shuffle, *repeat;
} now;

static void now_open(void);

static void time_set(lv_obj_t *label, const char *sign, uint32_t seconds)
{
	ui_set_text_fmt(label, "%s%u:%02u", sign, seconds / 60, seconds % 60);
}

/* Showing the state */

static void eq_update(void)
{
	for (unsigned i = 0; i < TRACK_COUNT; i++) {
		bool current = player.started && i == player.track;
		lv_obj_set_hidden(lib.eq[i], !current);
		if (!current)
			continue;
		for (int b = 0; b < EQ_BARS; b++)
			lv_obj_set_height(lib.bars[i][b], player.playing ? 8 + rand() % 25 : 8);
	}
}

static void position_update(void)
{
	if (!now.scr)
		return;
	const struct track *t = &tracks[player.track];
	uint32_t s = player.pos_ms / 1000;
	/* Not while it is dragged: the labels follow the finger */
	if (!lv_obj_has_state(now.slider, LV_STATE_PRESSED)) {
		lv_slider_set_value(now.slider, (int32_t)s, LV_ANIM_OFF);
		time_set(now.elapsed, "", s);
		time_set(now.remaining, "-", t->seconds - s);
	}
}

static void toggle_style(lv_obj_t *button, bool on)
{
	lv_obj_set_style_text_color(lv_obj_get_child(button, 0),
	                            lv_color_hex(on ? UI_COLOR_ACCENT : UI_COLOR_DIM), 0);
}

static void state_update(void)
{
	const struct track *t = &tracks[player.track];
	const char *play = player.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;

	if (lib.scr) {
		lv_obj_set_hidden(lib.mini, !player.started);
		lv_label_set_text_static(lib.mini_title, t->title);
		lv_label_set_text_static(lib.mini_artist, t->artist);
		lv_label_set_text_static(lv_obj_get_child(lib.mini_play, 0), play);
		eq_update();
	}
	if (now.scr) {
		lv_obj_set_style_bg_color(now.cover, lv_color_hex(t->color), 0);
		lv_obj_set_style_bg_color(now.hole, lv_color_hex(t->color), 0);
		lv_label_set_text_static(now.album, t->album);
		lv_label_set_text_static(now.title, t->title);
		lv_label_set_text_static(now.artist, t->artist);
		lv_slider_set_range(now.slider, 0, (int32_t)t->seconds);
		lv_label_set_text_static(lv_obj_get_child(now.play, 0), play);
		toggle_style(now.shuffle, player.shuffle);
		toggle_style(now.repeat, player.repeat);
		position_update();
	}
}

/* Playback */

static void tick_cb(lv_timer_t *timer);

static void play_set(bool playing)
{
	player.playing = playing;
	if (!player.timer)
		player.timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	if (playing)
		lv_timer_resume(player.timer);
	else
		lv_timer_pause(player.timer);
}

static void track_set(unsigned track)
{
	player.started = true;
	player.track = track;
	player.pos_ms = 0;
}

/* The song after this one: false at the end of the list without repeat */
static bool next_track(void)
{
	if (player.shuffle && TRACK_COUNT > 1) {
		unsigned n = (player.track + 1 + (unsigned)rand() % (TRACK_COUNT - 1)) % TRACK_COUNT;
		track_set(n);
		return true;
	}
	if (player.track + 1 < TRACK_COUNT) {
		track_set(player.track + 1);
		return true;
	}
	track_set(0);
	return player.repeat;
}

static void tick_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	uint32_t s = player.pos_ms / 1000;
	player.pos_ms += TICK_MS;
	if (player.pos_ms >= tracks[player.track].seconds * 1000) {
		play_set(next_track());
		state_update();
		return;
	}
	if (lib.scr)
		eq_update();
	if (player.pos_ms / 1000 != s)
		position_update();
}

static void play_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(15);
	player.started = true;
	play_set(!player.playing);
	state_update();
}

static void prev_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(15);
	/* Into a song: back to its start */
	if (player.pos_ms >= 3000 || player.track == 0)
		player.pos_ms = 0;
	else
		track_set(player.track - 1);
	state_update();
}

static void next_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(15);
	/* By hand it always goes on, wrapping around */
	if (!next_track())
		track_set(0);
	state_update();
}

static void shuffle_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	player.shuffle = !player.shuffle;
	state_update();
}

static void repeat_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	player.repeat = !player.repeat;
	state_update();
}

static void seek_cb(lv_event_t *e)
{
	const struct track *t = &tracks[player.track];
	uint32_t s = (uint32_t)lv_slider_get_value(now.slider);
	if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
		player.pos_ms = s * 1000;
		return;
	}
	time_set(now.elapsed, "", s);
	time_set(now.remaining, "-", t->seconds - s);
}

/* Now Playing */

static void now_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	memset(&now, 0, sizeof(now));
}

static lv_obj_t *control_create(lv_obj_t *parent, int32_t size, uint32_t color, uint32_t pressed,
                                const char *symbol, const lv_font_t *font, lv_event_cb_t cb)
{
	lv_obj_t *button = ui_button(parent, size, size, color, pressed, symbol, cb, NULL);
	lv_obj_set_style_text_font(lv_obj_get_child(button, 0), font, 0);
	return button;
}

/* A record on the album's color: rings are opaque circles, drawn as clears */
static void cover_create(lv_obj_t *parent)
{
	now.cover = ui_box(parent);
	lv_obj_set_size(now.cover, 520, 520);
	lv_obj_set_style_radius(now.cover, 32, 0);
	lv_obj_set_style_bg_opa(now.cover, LV_OPA_COVER, 0);

	lv_obj_t *disc = ui_box(now.cover);
	lv_obj_set_size(disc, 400, 400);
	lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(disc, lv_color_hex(0x1c1c1e), 0);
	lv_obj_center(disc);

	lv_obj_t *groove = ui_box(disc);
	lv_obj_set_size(groove, 300, 300);
	lv_obj_set_style_radius(groove, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(groove, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(groove, lv_color_hex(0x2c2c2e), 0);
	lv_obj_center(groove);

	lv_obj_t *inner = ui_box(groove);
	lv_obj_set_size(inner, 260, 260);
	lv_obj_set_style_radius(inner, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(inner, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(inner, lv_color_hex(0x1c1c1e), 0);
	lv_obj_center(inner);

	now.hole = ui_box(inner);
	lv_obj_set_size(now.hole, 140, 140);
	lv_obj_set_style_radius(now.hole, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(now.hole, LV_OPA_COVER, 0);
	lv_obj_center(now.hole);

	lv_obj_t *spindle = ui_box(now.hole);
	lv_obj_set_size(spindle, 20, 20);
	lv_obj_set_style_radius(spindle, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(spindle, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(spindle, lv_color_hex(0x1c1c1e), 0);
	lv_obj_center(spindle);
}

static lv_obj_t *centered_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *label = ui_one_line_label(parent, "", font);
	lv_obj_set_width(label, LV_PCT(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	return label;
}

static void now_open(void)
{
	if (now.scr)
		return;
	lv_obj_t *body;
	now.scr = ui_app_screen_create("Now Playing", NULL, &body);
	lv_obj_add_event_cb(now.scr, now_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(body, 48, 0);
	lv_obj_set_style_pad_top(body, 32, 0);
	lv_obj_set_scrollable(body, false);

	cover_create(body);

	now.album = centered_label(body, &ui_font_20, UI_COLOR_DIM);
	lv_obj_set_style_margin_top(now.album, 24, 0);
	now.title = centered_label(body, &ui_font_32, UI_COLOR_TEXT);
	lv_obj_set_style_margin_top(now.title, 24, 0);
	now.artist = centered_label(body, &ui_font_24, UI_COLOR_ACCENT);
	lv_obj_set_style_margin_top(now.artist, 8, 0);

	now.slider = lv_slider_create(body);
	lv_obj_set_size(now.slider, LV_PCT(100), 10);
	lv_obj_set_style_margin_top(now.slider, 48, 0);
	lv_obj_set_ext_click_area(now.slider, 36);
	lv_obj_set_style_bg_color(now.slider, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
	lv_obj_set_style_bg_color(now.slider, lv_color_hex(UI_COLOR_TEXT), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(now.slider, lv_color_hex(UI_COLOR_TEXT), LV_PART_KNOB);
	lv_obj_set_style_pad_all(now.slider, 10, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(now.slider, 0, LV_PART_KNOB);
	lv_obj_set_style_outline_width(now.slider, 0, LV_PART_KNOB | LV_STATE_FOCUSED);
	lv_obj_add_event_cb(now.slider, seek_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(now.slider, seek_cb, LV_EVENT_RELEASED, NULL);

	lv_obj_t *times = ui_box(body);
	lv_obj_set_size(times, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(times, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(times, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_margin_top(times, 16, 0);
	now.elapsed = ui_dim_label(times, "", &ui_font_20);
	now.remaining = ui_dim_label(times, "", &ui_font_20);

	lv_obj_t *controls = ui_box(body);
	lv_obj_set_size(controls, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_margin_top(controls, 32, 0);
	now.shuffle = control_create(controls, 80, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
	                             LV_SYMBOL_SHUFFLE, &ui_font_24, shuffle_cb);
	control_create(controls, 104, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
	               LV_SYMBOL_PREV, &ui_font_32, prev_cb);
	now.play = control_create(controls, 136, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED,
	                          LV_SYMBOL_PLAY, &ui_font_48, play_cb);
	control_create(controls, 104, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
	               LV_SYMBOL_NEXT, &ui_font_32, next_cb);
	now.repeat = control_create(controls, 80, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
	                            LV_SYMBOL_LOOP, &ui_font_24, repeat_cb);

	state_update();
	ui_push(now.scr);
}

/* Library */

static void lib_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	memset(&lib, 0, sizeof(lib));
}

static void row_cb(lv_event_t *e)
{
	unsigned i = (unsigned)(uintptr_t)lv_event_get_user_data(e);
	if (!player.started || i != player.track)
		track_set(i);
	play_set(true);
	state_update();
	now_open();
}

static void mini_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	now_open();
}

/* Small cover: a square in the album's color with a note */
static void thumb_create(lv_obj_t *parent, uint32_t color)
{
	lv_obj_t *thumb = ui_box(parent);
	lv_obj_set_size(thumb, 72, 72);
	lv_obj_set_style_radius(thumb, 12, 0);
	lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(thumb, lv_color_hex(color), 0);
	lv_obj_t *note = lv_label_create(thumb);
	lv_label_set_text_static(note, LV_SYMBOL_AUDIO);
	lv_obj_set_style_text_font(note, &ui_font_26, 0);
	lv_obj_set_style_text_color(note, lv_color_white(), 0);
	lv_obj_center(note);
}

static void row_create(lv_obj_t *card, unsigned i, bool divider)
{
	const struct track *t = &tracks[i];
	lv_obj_t *row = ui_row_create(card, ROW_H, divider, row_cb, (void *)(uintptr_t)i);
	thumb_create(row, t->color);

	lv_obj_t *text = ui_row_text(row);
	lv_obj_t *title = ui_one_line_label(text, t->title, &ui_font_26);
	lv_obj_set_width(title, LV_PCT(100));
	ui_dim_label(text, t->artist, &ui_font_20);

	/* Bars bouncing next to the song playing */
	lv_obj_t *eq = lib.eq[i] = ui_box(row);
	lv_obj_set_size(eq, 32, 32);
	lv_obj_set_flex_flow(eq, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(eq, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_column(eq, 4, 0);
	for (int b = 0; b < EQ_BARS; b++) {
		lv_obj_t *bar = lib.bars[i][b] = ui_box(eq);
		lv_obj_set_size(bar, 6, 8);
		lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_ACCENT), 0);
	}

	lv_obj_t *len = lv_label_create(row);
	time_set(len, "", t->seconds);
	lv_obj_set_style_text_font(len, &ui_font_20, 0);
	lv_obj_set_style_text_color(len, lv_color_hex(UI_COLOR_DIM), 0);
}

/* The song playing, under the list: tapping it opens Now Playing */
static void mini_create(lv_obj_t *parent)
{
	lib.mini = ui_bar_create(parent);
	lv_obj_set_style_pad_hor(lib.mini, 24, 0);
	lv_obj_set_style_pad_column(lib.mini, 20, 0);
	lv_obj_set_style_bg_color(lib.mini, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_clickable(lib.mini, true);
	lv_obj_add_event_cb(lib.mini, mini_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *text = ui_row_text(lib.mini);
	lib.mini_title = ui_one_line_label(text, "", &ui_font_24);
	lv_obj_set_width(lib.mini_title, LV_PCT(100));
	lib.mini_artist = ui_dim_label(text, "", &ui_font_20);

	lib.mini_play = control_create(lib.mini, 80, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED,
	                               LV_SYMBOL_PLAY, &ui_font_32, play_cb);
	control_create(lib.mini, 80, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
	               LV_SYMBOL_NEXT, &ui_font_26, next_cb);
}

void music_open(void)
{
	lv_obj_t *content;
	lib.scr = ui_app_screen_create("Music", NULL, &content);
	lv_obj_add_event_cb(lib.scr, lib_delete_cb, LV_EVENT_DELETE, NULL);

	/* The list scrolls above the bar of the song playing */
	lv_obj_t *list = ui_box(content);
	lv_obj_set_width(list, LV_PCT(100));
	lv_obj_set_flex_grow(list, 1);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(list, 16, 0);
	lv_obj_set_style_pad_bottom(list, 32, 0);
	lv_obj_set_style_pad_row(list, 8, 0);
	lv_obj_set_scrollable(list, true);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

	/* A section per album, in the order of tracks[] */
	lv_obj_t *card = NULL;
	for (unsigned i = 0; i < TRACK_COUNT; i++) {
		const struct track *t = &tracks[i];
		if (!card || strcmp(t->album, tracks[i - 1].album) != 0) {
			ui_section_label(list, t->album);
			card = ui_card_create(list);
		}
		bool last = i + 1 == TRACK_COUNT || strcmp(tracks[i + 1].album, t->album) != 0;
		row_create(card, i, !last);
	}

	mini_create(content);
	state_update();
	ui_push(lib.scr);
}
