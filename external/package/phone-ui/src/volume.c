/*
 * Volume: the codec's DAC level (alsa-lib), before the speaker and the headphones,
 * saved in the settings. The volume keys step it and show a slider at the right
 * edge, next to them, which can be dragged until it hides again.
 *
 * Volume Down with Power held, or Power with Volume Down held, takes a screenshot
 * (screenshot.c) instead: the level goes back to what it was before that press.
 *
 * During a call with sound (ui_volume_call(), modem.c) the keys and their slider step
 * the call's level instead, saved too, which the board's audio script sets (the
 * earpiece's or loudspeaker's amplifier); the DAC is the script's until the call's
 * sound is off, then the music's level again.
 *
 * With a Bluetooth speaker or headphones playing (bt_audio_device()) and no call, the keys,
 * the slider and the quick settings' slider step its volume instead (bluez-alsa's, read
 * back each second: its own buttons change it), the DAC's level kept for when it goes.
 */

#include <linux/input-event-codes.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#include "bluetooth.h"
#include "modem.h"
#include "ui.h"

#define LEVELS UI_VOLUME_LEVELS
#define CALL_LEVEL 14 /* at first: the earpiece's amplifier at its loudest (call-audio.sh) */
#define DAC_0DB 160   /* of 0-192, 0.75 dB steps */
#define DAC_STEP 6    /* 4.5 dB per level */
#define HIDE_MS 1500
#define REPEAT_DELAY_MS 400
#define REPEAT_MS 120
#define EVDEV_KEY(code) ((code) + 0xFFFF) /* lv_evdev's key for codes it does not map */
#define CARD_POLL_MS 200
#define CARD_WAIT_MS 15000

static int level = -1;
static struct {
	bool on;
	int level;
} call;
static snd_mixer_t *mixer;
static snd_mixer_elem_t *dac;

static struct {
	lv_obj_t *card, *slider, *icon;
	lv_timer_t *hide, *repeat;
	int step; /* of the key held, 0 if none */
	int before; /* the level when it was pressed */
	bool shot;  /* Volume Down took a screenshot: ignored until released */
	bool power; /* held */
} vol;

static bool mixer_open(void)
{
	if (dac)
		return true;
	snd_mixer_selem_id_t *id;
	snd_mixer_selem_id_alloca(&id);
	snd_mixer_selem_id_set_name(id, "DAC");
	if (snd_mixer_open(&mixer, 0) < 0)
		return false;
	if (snd_mixer_attach(mixer, "hw:0") < 0 || snd_mixer_selem_register(mixer, NULL, NULL) < 0 ||
	    snd_mixer_load(mixer) < 0 || !(dac = snd_mixer_find_selem(mixer, id))) {
		printf("phone-ui: no DAC mixer control, no volume\n");
		snd_mixer_close(mixer);
		mixer = NULL;
		dac = NULL;
		return false;
	}
	return true;
}

static void level_apply(void)
{
	if (call.on || !mixer_open())
		return;
	long raw = level ? DAC_0DB - (LEVELS - level) * DAC_STEP : 0;
	snd_mixer_selem_set_playback_volume_all(dac, raw);
	snd_mixer_selem_set_playback_switch_all(dac, level > 0);
}

static void level_set(int l)
{
	l = LV_CLAMP(0, l, LEVELS);
	if (l == level)
		return;
	level = l;
	level_apply();
	ui_setting_set_int("volume", level);
}

/* The Bluetooth device's volume (0-127) in levels, -1 without one */
static int bt_level(void)
{
	int v = bt_audio_volume();
	return v < 0 ? -1 : (v * LEVELS * 2 + 127) / 254;
}

/* The music's level: the Bluetooth device's while one plays, else the DAC's */
static int music(void)
{
	return bt_level() >= 0 ? bt_level() : level;
}

static void music_set(int l)
{
	if (bt_level() < 0) {
		level_set(l);
		return;
	}
	l = LV_CLAMP(0, l, LEVELS);
	bt_audio_volume_set((l * 127 * 2 + LEVELS) / (2 * LEVELS));
}

/* The level the keys and the slider show and step: the call's during one */
static int shown(void)
{
	return call.on ? call.level : music();
}

static void shown_set(int l)
{
	if (!call.on) {
		music_set(l);
		return;
	}
	l = LV_CLAMP(1, l, LEVELS);
	if (l == call.level)
		return;
	call.level = l;
	ui_setting_set_int("call_volume", l);
	modem_call_volume_changed();
}

/* The slider */

static void icon_update(void)
{
	lv_label_set_text_static(vol.icon, shown() == 0 ? LV_SYMBOL_MUTE :
	                         shown() <= LEVELS / 2 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX);
}

static void hide(void)
{
	if (vol.hide)
		lv_timer_pause(vol.hide);
	if (vol.card) {
		lv_obj_delete(vol.card);
		vol.card = vol.slider = vol.icon = NULL;
	}
}

static void hide_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	/* Not under the finger */
	if (vol.slider && lv_obj_has_state(vol.slider, LV_STATE_PRESSED))
		return;
	hide();
}

static void slider_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	shown_set((int)lv_slider_get_value(vol.slider));
	icon_update();
	lv_timer_reset(vol.hide);
}

/* Made for each showing: it takes the theme's colors of the moment */
static void show(void)
{
	if (!vol.hide)
		vol.hide = lv_timer_create(hide_cb, HIDE_MS, NULL);
	lv_timer_reset(vol.hide);
	lv_timer_resume(vol.hide);
	if (!vol.card) {
		vol.card = ui_box(lv_layer_top());
		lv_obj_set_size(vol.card, 104, LV_SIZE_CONTENT);
		lv_obj_align(vol.card, LV_ALIGN_TOP_RIGHT, -16, UI_STATUS_H + 120);
		lv_obj_set_flex_flow(vol.card, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(vol.card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_ver(vol.card, 24, 0);
		lv_obj_set_style_pad_row(vol.card, 20, 0);
		lv_obj_set_style_radius(vol.card, 52, 0);
		lv_obj_set_style_bg_opa(vol.card, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(vol.card, lv_color_hex(UI_COLOR_CARD), 0);
		/* Taps beside the slider do not reach the screen below */
		lv_obj_set_clickable(vol.card, true);

		vol.slider = lv_slider_create(vol.card);
		lv_obj_set_size(vol.slider, 56, 360); /* taller than wide: vertical */
		lv_obj_set_ext_click_area(vol.slider, 24);
		lv_obj_set_style_radius(vol.slider, 28, LV_PART_MAIN);
		lv_obj_set_style_radius(vol.slider, 28, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(vol.slider, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
		lv_obj_set_style_bg_color(vol.slider, lv_color_hex(UI_COLOR_TEXT), LV_PART_INDICATOR);
		lv_obj_set_style_bg_opa(vol.slider, LV_OPA_TRANSP, LV_PART_KNOB);
		lv_obj_set_style_pad_all(vol.slider, 0, LV_PART_KNOB);
		lv_obj_set_style_shadow_width(vol.slider, 0, LV_PART_KNOB);
		lv_obj_set_style_outline_width(vol.slider, 0, LV_PART_KNOB | LV_STATE_FOCUSED);
		lv_obj_set_style_outline_width(vol.slider, 0, LV_STATE_FOCUSED);
		lv_obj_add_event_cb(vol.slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

		vol.icon = lv_label_create(vol.card);
		lv_obj_set_style_text_font(vol.icon, &ui_font_32, 0);
		lv_obj_set_style_text_color(vol.icon, lv_color_hex(UI_COLOR_TEXT), 0);
	}
	lv_slider_set_range(vol.slider, call.on ? 1 : 0, LEVELS);
	lv_slider_set_value(vol.slider, shown(), LV_ANIM_OFF);
	icon_update();
}

/* The keys */

static void step(int dir)
{
	shown_set(shown() + dir);
	show();
}

static void repeat_cb(lv_timer_t *timer)
{
	lv_timer_set_period(timer, REPEAT_MS);
	step(vol.step);
}

static lv_indev_read_cb_t keys_read;

static void screenshot(void)
{
	vol.step = 0;
	vol.shot = true;
	if (vol.repeat)
		lv_timer_pause(vol.repeat);
	hide();
	ui_power_cancel();
	ui_screenshot();
}

/* lv_evdev's reading, then the volume keys taken from it: pressed, held, released.
 * Power is on another input device than the volume keys (the PMIC's). */
static void keys_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	keys_read(indev, data);
	if (data->key == EVDEV_KEY(KEY_POWER)) {
		bool pressed = data->state == LV_INDEV_STATE_PRESSED;
		if (pressed && !vol.power && vol.step < 0) {
			shown_set(vol.before);
			screenshot();
		} else if (pressed != vol.power) {
			ui_power_key(pressed);
		}
		vol.power = pressed;
		return;
	}
	int dir = data->key == EVDEV_KEY(KEY_VOLUMEUP) ? 1 : data->key == EVDEV_KEY(KEY_VOLUMEDOWN) ? -1 : 0;
	if (!dir)
		return;
	if (dir < 0 && vol.shot) {
		vol.shot = data->state == LV_INDEV_STATE_PRESSED;
		return;
	}
	if (data->state == LV_INDEV_STATE_PRESSED && dir < 0 && vol.power) {
		screenshot();
		return;
	}
	if (data->state == LV_INDEV_STATE_PRESSED && vol.step != dir) {
		vol.before = shown();
		vol.step = dir;
		step(dir);
		if (!vol.repeat)
			vol.repeat = lv_timer_create(repeat_cb, REPEAT_DELAY_MS, NULL);
		lv_timer_set_period(vol.repeat, REPEAT_DELAY_MS);
		lv_timer_reset(vol.repeat);
		lv_timer_resume(vol.repeat);
	} else if (data->state == LV_INDEV_STATE_RELEASED && vol.step == dir) {
		vol.step = 0;
		lv_timer_pause(vol.repeat);
	}
}

void ui_volume_keys(lv_indev_t *indev)
{
	keys_read = lv_indev_get_read_cb(indev);
	lv_indev_set_read_cb(indev, keys_read_cb);
}

/* At boot phone-ui can start before the sound card exists (S10mdev loads its
 * modules in the background): apply the saved level once it shows up. */
static void card_wait_cb(lv_timer_t *t)
{
	int *waited = lv_timer_get_user_data(t);
	*waited += CARD_POLL_MS;
	if (access("/dev/snd/controlC0", F_OK) == 0 || *waited >= CARD_WAIT_MS) {
		level_apply();
		lv_timer_delete(t);
	}
}

void ui_volume_init(void)
{
	static int waited;
	level = LV_CLAMP(0, ui_setting_get_int("volume", 12), LEVELS);
	call.level = LV_CLAMP(1, ui_setting_get_int("call_volume", CALL_LEVEL), LEVELS);
	if (access("/dev/snd/controlC0", F_OK) == 0)
		level_apply();
	else
		lv_timer_create(card_wait_cb, CARD_POLL_MS, &waited);
}

int ui_volume_get(void)
{
	return music();
}

void ui_volume_set(int l)
{
	music_set(l);
}

void ui_volume_call(bool on)
{
	if (on == call.on)
		return;
	hide(); /* its range changes */
	call.on = on;
	level_apply();
}

int ui_volume_call_level(void)
{
	return call.level;
}
