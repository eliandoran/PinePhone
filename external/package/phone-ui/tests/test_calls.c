/* Voice calls (phone.c, modem.c, modem_at.c): dialed, answered, refused, the call log
 * and the board's call audio, through the UI (ui_test.h) and the fake modem */

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test.h"
#include "ui_test.h"
#include "src/misc/lv_text_private.h" /* lv_text_encoded_next() */

#include "json.h"
#include "modem/modem.h"
#include "ui_symbols.h"

#define CONTACTS "/var/lib/contacts/contacts.json"
#define CALLS "/var/lib/phone/calls.jsonl"

static bool registered(void)
{
	return modem_network() == MODEM_NET_HOME;
}

/* The modem with a ready SIM, registered */
static void ready(void)
{
	ui_test_start();
	ui_test_write(CONTACTS, "[ { \"name\": \"Ben\", \"number\": \"+1 415 555 0132\" } ]"); /* after: it empties /var/lib */
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(registered, 10000));
}

/* Phone's keypad: the number typed, then Call */
static bool dial(const char *keys)
{
	if (!ui_test_open("Phone") || !ui_test_tap_text("Keypad"))
		return false;
	for (; *keys; keys++) {
		char key[2] = { *keys == '+' ? '0' : *keys, '\0' }; /* '+': a long press on 0 */
		if (*keys == '+' ? !ui_test_long_press_text(key) : !ui_test_tap_text(key))
			return false;
	}
	return ui_test_tap_text(UI_SYMBOL_PHONE);
}

/* The round button above a caption ("Answer", "Mute") */
static bool tap_button(const char *caption)
{
	lv_obj_t *label = ui_test_find(caption);
	if (!label)
		return ui_test_tap_text(caption); /* fails, with what is shown */
	ui_test_tap(lv_obj_get_child(lv_obj_get_parent(label), 0));
	return true;
}

/* The hang-up button: the red one at the bottom of the call screen */
static bool hang_up(void)
{
	return ui_test_tap_text(UI_SYMBOL_PHONE_135);
}

static bool has_dialed(void)
{
	return ui_test_modem_dialed(0) != NULL;
}

/* The first number the modem was asked to dial, once it was (a tick later; the waits
 * are in real time too: the fake modem answers from a thread) */
static const char *dialed(void)
{
	ui_test_wait_until(has_dialed, 3000);
	return ui_test_modem_dialed(0);
}

static int call_state(void)
{
	return ui_test_modem_call(NULL);
}

static int call_wanted;

static bool call_is_wanted(void)
{
	return call_state() == call_wanted;
}

/* Waits for the modem's call to be in `state` (-1 none) */
static bool call_becomes(int state, uint32_t ms)
{
	call_wanted = state;
	if (ui_test_wait_until(call_is_wanted, ms))
		return true;
	fprintf(stderr, "  call %d, expected %d\n", call_state(), state);
	return false;
}

/* The call log's lines, the last one parsed into *last (to be freed) */
static int calls_logged(struct json **last)
{
	char line[512];
	int n = 0;
	*last = NULL;
	FILE *f = fopen(CALLS, "r");
	while (f && fgets(line, sizeof(line), f)) {
		json_free(*last);
		*last = json_parse(line);
		n++;
	}
	if (f)
		fclose(f);
	return n;
}

/* The last call logged: its number, kind and duration */
static bool logged(const char *number, const char *kind, int min_duration, int max_duration)
{
	struct json *last;
	int n = calls_logged(&last);
	const char *got_number = json_str(json_get(last, "number"), "");
	const char *got_kind = json_str(json_get(last, "kind"), "");
	long long duration = json_int(json_get(last, "duration"), -1);
	bool ok = n > 0 && strcmp(got_number, number) == 0 && strcmp(got_kind, kind) == 0 &&
	          duration >= min_duration && duration <= max_duration;
	if (!ok)
		fprintf(stderr, "  logged %d calls, the last %s %s %lld s, expected %s %s %d-%d s\n", n, got_number,
		        got_kind, duration, number, kind, min_duration, max_duration);
	json_free(last);
	return ok;
}

TEST(call_answered)
{
	ready();
	ui_test_modem_callee(UI_TEST_CALLEE_ANSWERS, 2000);
	CHECK(dial("5550100"));
	CHECK_STR(dialed(), "5550100");
	CHECK(ui_test_shows("Calling..."));
	CHECK(ui_test_wait_for("Ringing...", 3000));
	CHECK(ui_test_wait_for("0:00", 5000)); /* answered: how long it lasts */
	CHECK(ui_test_font("Speaker") == UI_FONT_BODY); /* the options' captions */
	CHECK(ui_test_call_audio_ran("earpiece 14", 3000));
	CHECK(ui_test_wait_for("0:03", 5000));

	CHECK(hang_up());
	CHECK(call_becomes(-1, 2000));
	CHECK_INT(ui_test_modem_hang_ups(NULL), 1);
	CHECK(ui_test_wait_for("Call ended", 2000));
	CHECK(ui_test_call_audio_ran("off", 3000));
	/* Back where it was dialed */
	CHECK(ui_test_wait_for("Recents", 5000));
	CHECK(logged("5550100", "outgoing", 3, 6));
}

TEST(other_end_hangs_up)
{
	ready();
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("0:01", 6000));
	ui_test_modem_remote_hang_up();
	CHECK(ui_test_wait_for("Call ended", 3000));
	CHECK_INT(ui_test_modem_hang_ups(NULL), 0);
	CHECK(ui_test_wait_for("Recents", 5000));
	CHECK(logged("5550100", "outgoing", 1, 4));
}

TEST(busy)
{
	ready();
	ui_test_modem_callee(UI_TEST_CALLEE_BUSY, 0);
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("Busy", 3000));
	CHECK(ui_test_wait_for("Recents", 5000));
	CHECK(logged("5550100", "outgoing", 0, 0));
}

/* The log keeps the last 500 calls: appended to until 600 lines, then rewritten with
 * them (the oldest dropped) */
TEST(log_trimmed_to_the_last_500)
{
	ui_test_reset();
	ui_test_write(CALLS, "");
	FILE *f = fopen(CALLS, "w");
	for (int i = 0; i < 599; i++)
		fprintf(f, "{\"number\":\"+407000%05d\",\"kind\":\"incoming\",\"time\":%d,\"duration\":5}\n", i,
		        1700000000 + i * 60);
	fclose(f);
	ready();
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("Ringing...", 3000));
	CHECK(hang_up());
	CHECK(ui_test_wait_for("Recents", 5000));
	struct json *last;
	CHECK_INT(calls_logged(&last), 500);
	CHECK(logged("5550100", "outgoing", 0, 0));
	json_free(last);
	f = fopen(CALLS, "r");
	char line[512] = "";
	CHECK(f && fgets(line, sizeof(line), f));
	if (f)
		fclose(f);
	CHECK(strstr(line, "\"+40700000100\"")); /* the 100 oldest gone */
	CHECK(ui_test_shows("5550100"));
}

/* Refused by the network at once (barred, no credit): NO CARRIER for ATD itself */
TEST(refused_by_the_network)
{
	ready();
	ui_test_modem_callee(UI_TEST_CALLEE_REFUSED, 0);
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("Call failed", 3000));
	CHECK(ui_test_wait_for("Recents", 5000));
	CHECK(ui_test_modem_call(NULL) < 0);
	CHECK(logged("5550100", "outgoing", 0, 0));
}

TEST(no_answer)
{
	ready();
	ui_test_modem_callee(UI_TEST_CALLEE_NO_ANSWER, 0);
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("Ringing...", 3000));
	CHECK(ui_test_wait_for("No answer", 35000));
	CHECK(logged("5550100", "outgoing", 0, 0));
}

TEST(hang_up_while_ringing)
{
	ready();
	ui_test_modem_callee(UI_TEST_CALLEE_SILENT, 0);
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("Ringing...", 3000));
	CHECK(hang_up());
	CHECK(call_becomes(-1, 2000));
	CHECK(ui_test_wait_for("Recents", 5000));
	CHECK(logged("5550100", "outgoing", 0, 0));
	CHECK(ui_test_call_audio_ran("off", 3000));
}

TEST(call_a_contact)
{
	ready();
	CHECK(dial("+14155550132"));
	CHECK(ui_test_shows("Ben"));
	CHECK_STR(dialed(), "+14155550132");
}

TEST(mute_and_speaker)
{
	ready();
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("0:00", 6000));
	CHECK(ui_test_call_audio_ran("earpiece 14", 3000));
	bool muted = false;

	CHECK(tap_button("Mute"));
	for (int i = 0; i < 20 && (ui_test_modem_call(&muted), !muted); i++)
		ui_test_wait(100);
	CHECK(muted);
	CHECK(tap_button("Mute"));
	for (int i = 0; i < 20 && (ui_test_modem_call(&muted), muted); i++)
		ui_test_wait(100);
	CHECK(!muted);

	CHECK(tap_button("Speaker"));
	CHECK(ui_test_call_audio_ran("speaker 14", 3000));
	CHECK(tap_button("Speaker"));
	CHECK(ui_test_call_audio_ran("earpiece 14", 3000));
	CHECK(hang_up());
	CHECK(ui_test_call_audio_ran("off", 3000));
}

/* A Bluetooth headset for calls (the Hands-Free profile, bluez-alsa's HFP-AG PCMs) */

#define HEADSET "F0:AE:66:E8:A4:BB"

static void headset(bool connected)
{
	ui_test_reset();
	ui_test_write("/var/lib/phone-ui/settings", "bluetooth=1\n");
	ui_test_bt_running(true);
	ui_test_bt_paired(HEADSET, "soundcore R50i", connected);
	ui_test_bt_audio(HEADSET, 48000);
	ui_test_bt_headset(HEADSET);
}

TEST(call_on_headset)
{
	headset(true);
	ready();
	ui_test_wait(1100); /* the headset seen (each second) */
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("0:00", 6000));
	CHECK(ui_test_call_audio_ran("bluetooth 14", 3000));

	CHECK(tap_button("Speaker"));
	CHECK(ui_test_call_audio_ran("speaker 14", 3000));
	CHECK(tap_button("Bluetooth"));
	CHECK(ui_test_call_audio_ran("bluetooth 14", 3000));
	CHECK(tap_button("Bluetooth"));
	CHECK(ui_test_call_audio_ran("earpiece 14", 3000));
	CHECK(tap_button("Bluetooth"));
	CHECK(ui_test_call_audio_ran("bluetooth 14", 3000));
	CHECK(hang_up());
	CHECK(ui_test_call_audio_ran("off", 3000));
}

TEST(headset_comes_and_goes_during_call)
{
	headset(false);
	ready();
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("0:00", 6000));
	CHECK(ui_test_call_audio_ran("earpiece 14", 3000));
	CHECK(!ui_test_shows("Bluetooth"));

	ui_test_bt_connected(HEADSET, true);
	CHECK(ui_test_call_audio_ran("bluetooth 14", 3000));
	CHECK(ui_test_wait_for("Bluetooth", 2000));

	ui_test_bt_connected(HEADSET, false);
	CHECK(ui_test_call_audio_ran("earpiece 14", 3000));
	ui_test_wait(1100);
	CHECK(!ui_test_shows("Bluetooth"));
	CHECK(hang_up());
}

/* Speakers for music only (A2DP): calls stay on the phone */
TEST(call_not_on_music_speaker)
{
	ui_test_reset();
	ui_test_write("/var/lib/phone-ui/settings", "bluetooth=1\n");
	ui_test_bt_running(true);
	ui_test_bt_paired(HEADSET, "soundcore R50i", true);
	ui_test_bt_audio(HEADSET, 48000);
	ready();
	ui_test_wait(1100);
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("0:00", 6000));
	CHECK(ui_test_call_audio_ran("earpiece 14", 3000));
	CHECK(!ui_test_shows("Bluetooth"));
	CHECK(hang_up());
}

TEST(incoming_answered)
{
	ready();
	ui_test_modem_incoming("+14155550132");
	CHECK(ui_test_wait_for("Incoming call", 3000));
	CHECK(ui_test_wait_for("Ben", 2000)); /* the caller's number: +CLIP after RING */
	CHECK(tap_button("Answer"));
	CHECK(call_becomes(0, 2000));
	CHECK(ui_test_wait_for("0:02", 5000));
	CHECK(ui_test_call_audio_ran("earpiece 14", 3000));
	CHECK(!ui_test_shows("Incoming call"));

	ui_test_modem_remote_hang_up();
	CHECK(ui_test_wait_for("Call ended", 3000));
	ui_test_wait(3000);
	CHECK(!ui_test_shows("Call ended"));
	CHECK(logged("+14155550132", "incoming", 2, 5));
}

TEST(incoming_declined)
{
	ready();
	ui_test_modem_incoming("0721555666");
	CHECK(ui_test_wait_for("Incoming call", 3000));
	CHECK(ui_test_wait_for("0721555666", 2000));
	CHECK(tap_button("Decline"));
	CHECK(call_becomes(-1, 2000));
	CHECK_INT(ui_test_modem_hang_ups(NULL), 1);
	ui_test_wait(3000);
	CHECK(!ui_test_shows("Incoming call"));
	CHECK(logged("0721555666", "incoming", 0, 0));
}

/* The symbol on the round button above a caption, drawn by its label's font itself */
static const char *button_symbol(const char *caption)
{
	lv_obj_t *label = ui_test_find(caption);
	if (!label)
		return NULL;
	lv_obj_t *symbol = lv_obj_get_child(lv_obj_get_child(lv_obj_get_parent(label), 0), 0);
	const char *text = lv_label_get_text(symbol);
	const lv_font_t *font = lv_obj_get_style_text_font(symbol, LV_PART_MAIN);
	lv_font_glyph_dsc_t glyph;
	uint32_t i = 0;
	uint32_t letter = lv_text_encoded_next(text, &i);
	if (!lv_font_get_glyph_dsc(font, &glyph, letter, 0) || glyph.resolved_font != font || !glyph.box_w)
		return NULL;
	return text;
}

/* Decline shows the handset hung up, Answer the handset, as phones do */
TEST(incoming_buttons_symbols)
{
	ready();
	ui_test_modem_incoming("0721555666");
	CHECK(ui_test_wait_for("Incoming call", 3000));
	const char *decline = button_symbol("Decline"), *answer = button_symbol("Answer");
	CHECK(decline && !strcmp(decline, UI_SYMBOL_PHONE_135));
	CHECK(answer && !strcmp(answer, UI_SYMBOL_PHONE));
	CHECK(tap_button("Answer"));
	CHECK(call_becomes(0, 2000));
	CHECK(ui_test_wait_for("Mute", 2000));
	CHECK(!ui_test_shows("Decline"));
	CHECK(ui_test_shows(UI_SYMBOL_PHONE_135)); /* hang up */
}

TEST(incoming_missed)
{
	ready();
	ui_test_modem_incoming("0721555666");
	CHECK(ui_test_wait_for("Incoming call", 3000));
	ui_test_wait(4000); /* RING again */
	ui_test_modem_remote_hang_up();
	ui_test_wait(3000);
	CHECK(!ui_test_shows("Incoming call"));
	CHECK(logged("0721555666", "missed", 0, 0));
	unsigned answered;
	CHECK_INT(ui_test_modem_hang_ups(&answered), 0);
	CHECK_INT(answered, 0);
	/* In Recents */
	CHECK(ui_test_open("Phone"));
	CHECK(ui_test_shows("Missed"));
}

TEST(incoming_hidden_number)
{
	ready();
	ui_test_modem_incoming(NULL);
	CHECK(ui_test_wait_for("Incoming call", 3000));
	CHECK(ui_test_shows("Unknown number"));
	CHECK(tap_button("Decline"));
	CHECK(call_becomes(-1, 2000));
}

/* Over an app: it comes back after the call */
TEST(incoming_over_an_app)
{
	ready();
	CHECK(ui_test_open("Notes"));
	ui_test_modem_incoming("0721555666");
	CHECK(ui_test_wait_for("Incoming call", 3000));
	CHECK(tap_button("Decline"));
	ui_test_wait(3000);
	CHECK(ui_test_wait_for("Notes", 5000));
}

TEST(without_modem)
{
	ui_test_start();
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("No modem", 1000));
	CHECK(!ui_test_modem_dialed(0));
	CHECK(logged("5550100", "outgoing", 0, 0));
}

TEST(without_service)
{
	ui_test_modem_network(UI_TEST_NET_SEARCHING, NULL);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	ui_test_wait(3000);
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("No service", 1000));
	CHECK(!ui_test_modem_dialed(0));
}

TEST(modem_gone_during_a_call)
{
	ready();
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("0:01", 6000));
	ui_test_modem_unplug();
	CHECK(ui_test_wait_for("Call ended", 3000));
	CHECK(ui_test_call_audio_ran("off", 3000));
	CHECK(logged("5550100", "outgoing", 1, 4));
}

/* A second call while one is on: not dialed */
TEST(one_call_at_a_time)
{
	ready();
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("0:00", 6000));
	ui_test_modem_incoming("0721555666");
	ui_test_wait(2000);
	CHECK(!ui_test_shows("Incoming call"));
	CHECK(ui_test_shows("5550100"));
}

/* The volume keys during a call: the call's level (1 to 16, 14 at first), passed to
 * the audio script and kept for the next call; the music's stays as it was */
TEST(volume_keys_during_a_call)
{
	ready();
	CHECK(dial("5550100"));
	CHECK(ui_test_call_audio_ran("earpiece 14", 6000));
	ui_test_button_press(KEY_VOLUMEUP, 50);
	CHECK(ui_test_call_audio_ran("earpiece 15", 3000));
	lv_obj_t *s = ui_test_find_widget(&lv_slider_class);
	CHECK(s && lv_slider_get_value(s) == 15);
	ui_test_button_press(KEY_VOLUMEUP, 50);
	ui_test_button_press(KEY_VOLUMEUP, 50); /* the top */
	CHECK(ui_test_call_audio_ran("earpiece 16", 3000));
	CHECK(tap_button("Speaker"));
	CHECK(ui_test_call_audio_ran("speaker 16", 3000));
	CHECK_INT(ui_volume_get(), 12);
	CHECK(hang_up());
	CHECK(ui_test_call_audio_ran("off", 3000));
	CHECK(ui_test_wait_for("Recents", 5000));

	ui_test_button_press(KEY_VOLUMEDOWN, 50); /* no call: the music's */
	CHECK_INT(ui_volume_get(), 11);
	CHECK(dial("5550100"));
	CHECK(ui_test_call_audio_ran("earpiece 16", 6000));
}

/* Not below 1: a call cannot be silenced by mistake */
TEST(call_volume_lowest)
{
	ready();
	CHECK(dial("5550100"));
	CHECK(ui_test_call_audio_ran("earpiece 14", 6000));
	ui_test_button_press(KEY_VOLUMEDOWN, 3000); /* held: repeats */
	CHECK(ui_test_call_audio_ran("earpiece 1", 3000));
	lv_obj_t *s = ui_test_find_widget(&lv_slider_class);
	CHECK(s && lv_slider_get_value(s) == 1);
	CHECK_INT(ui_volume_get(), 12);
}

static const char *tones_wanted;

static bool tones_sent(void)
{
	return strcmp(ui_test_modem_tones(), tones_wanted) == 0;
}

/* Waits for the modem to have played these keys' tones, in order */
static bool tones(const char *keys)
{
	tones_wanted = keys;
	if (ui_test_wait_until(tones_sent, 5000))
		return true;
	fprintf(stderr, "  tones sent: \"%s\", not \"%s\"\n", ui_test_modem_tones(), keys);
	return false;
}

/* Keypad during a call: each key's tone to the other end (menus: "press 1"), the keys
 * shown; Hide for the buttons again */
TEST(keypad_sends_tones)
{
	ready();
	CHECK(dial("+14155550132"));
	CHECK(ui_test_wait_for("0:00", 6000));
	CHECK(tap_button("Keypad"));
	CHECK(ui_test_shows("Hide"));
	CHECK(!ui_test_shows("Mute"));
	CHECK(ui_test_shows("Ben")); /* until a key is pressed */
	CHECK(ui_test_tap_text("1"));
	CHECK(ui_test_tap_text("#"));
	CHECK(ui_test_tap_text("0"));
	CHECK(tones("1#0"));
	CHECK(ui_test_shows("1#0"));
	CHECK(ui_test_shows_part(":0")); /* the duration still */

	CHECK(ui_test_tap_text("Hide"));
	CHECK(ui_test_shows("Mute"));
	CHECK(!ui_test_shows("Hide"));
	CHECK(ui_test_shows("Ben"));
	CHECK(tap_button("Keypad"));
	CHECK(ui_test_shows("1#0")); /* kept for the call */
	CHECK(ui_test_tap_text("*"));
	CHECK(tones("1#0*"));
}

/* Pressed faster than the modem plays them: none lost, in order */
TEST(keypad_fast_presses)
{
	ready();
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("0:00", 6000));
	CHECK(tap_button("Keypad"));
	const char *keys = "2580*#9";
	for (const char *k = keys; *k; k++) {
		char text[2] = { *k, '\0' };
		CHECK(ui_test_tap_text(text));
	}
	CHECK(tones(keys));
}

/* Before the other end answers there is no one to hear them: not sent */
TEST(keypad_while_ringing)
{
	ready();
	ui_test_modem_callee(UI_TEST_CALLEE_ANSWERS, 4000);
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("Ringing...", 6000));
	CHECK(tap_button("Keypad"));
	CHECK(ui_test_tap_text("5"));
	CHECK(ui_test_wait_for("0:01", 8000));
	CHECK_STR(ui_test_modem_tones(), "");
	CHECK(ui_test_tap_text("7"));
	CHECK(tones("7"));
}

/* The call ends with the keypad open: why, without it */
TEST(keypad_when_the_call_ends)
{
	ready();
	CHECK(dial("5550100"));
	CHECK(ui_test_wait_for("0:00", 6000));
	CHECK(tap_button("Keypad"));
	CHECK(ui_test_tap_text("3"));
	ui_test_modem_remote_hang_up();
	CHECK(ui_test_wait_for("Call ended", 3000));
	CHECK(!ui_test_shows("Hide"));
	CHECK(ui_test_shows("5550100"));
}

/* Asleep behind the lock screen (Power), woken by the call: it shows over the lock screen,
 * which is back once the call ended */
TEST(incoming_over_the_lock_screen)
{
	ready();
	ui_test_write("/sys/power/state", "");
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(300);
	ui_test_settle();
	CHECK(ui_locked());
	ui_test_modem_incoming("0721555666");
	CHECK(ui_test_wait_for("Incoming call", 3000));
	CHECK(!ui_test_shows("Swipe up to unlock"));
	CHECK(!ui_locked());
	CHECK(tap_button("Decline")); /* reached: not under the lock screen */
	CHECK(call_becomes(-1, 2000));
	ui_test_wait(3000);
	CHECK(!ui_test_shows("Incoming call"));
	CHECK(ui_locked());
	CHECK(ui_test_shows("Swipe up to unlock"));
}

/* Declined over the lock screen: the call screen goes to the lock screen, never showing
 * the screen under it (home) as it slides away */
TEST(declined_over_the_lock_screen_no_home_between)
{
	ready();
	ui_test_write("/sys/power/state", "");
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(300);
	ui_test_settle();
	ui_test_modem_incoming("0721555666");
	CHECK(ui_test_wait_for("Incoming call", 3000));
	CHECK(tap_button("Decline"));
	bool between = false;
	for (int i = 0; i < 250 && !ui_locked(); i++) {
		ui_test_wait(16);
		if (!ui_locked() && !ui_test_shows("Incoming call") && !ui_test_shows("Call ended"))
			between = true;
	}
	CHECK(ui_locked());
	CHECK(!between);
}

/* Put to sleep with the shade open: the call is not under it */
TEST(incoming_over_the_shade_left_open)
{
	ready();
	ui_test_write("/sys/power/state", "");
	ui_test_tap_at(360, UI_STATUS_H / 2);
	CHECK(ui_test_wait_for("Flashlight", 2000));
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(300);
	ui_test_settle();
	CHECK(ui_locked());
	CHECK(!ui_shade_shown());
	ui_test_modem_incoming("0721555666");
	CHECK(ui_test_wait_for("Incoming call", 3000));
	CHECK(!ui_test_shows("Flashlight"));
	CHECK(tap_button("Decline")); /* reached: not under the shade */
	CHECK(call_becomes(-1, 2000));
}

/* Answered from the lock screen: locked again after the call */
TEST(incoming_answered_over_the_lock_screen)
{
	ready();
	ui_test_write("/sys/power/state", "");
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(300);
	ui_test_settle();
	ui_test_modem_incoming("0721555666");
	CHECK(ui_test_wait_for("Incoming call", 3000));
	CHECK(tap_button("Answer"));
	CHECK(call_becomes(0, 2000));
	ui_test_modem_remote_hang_up();
	CHECK(ui_test_wait_for("Call ended", 3000));
	ui_test_wait(3000);
	CHECK(ui_locked());
	CHECK(ui_test_shows("Swipe up to unlock"));
}

/* Woken for a call, the phone finds the modem's USB link reset and its first RING lost:
 * it asks the modem for its calls as soon as the port is back, rather than waiting
 * 3 s or more for the next ring */
TEST(incoming_found_when_the_modem_comes_back)
{
	ready();
	ui_test_modem_usb_away();
	ui_test_wait(500); /* phone-ui sees it gone */
	ui_test_modem_incoming("0721555666");
	ui_test_modem_usb_back();
	ui_test_wait(2000); /* tests' time only: the next RING comes after 3 s */
	CHECK(ui_test_shows("Incoming call"));
	CHECK(ui_test_shows("0721555666"));
	CHECK(tap_button("Decline"));
	CHECK(call_becomes(-1, 2000));
}

/* Woken up by the modem for a call (its ring line), the port kept through the USB reset
 * but the first RING lost: phone-ui asks for the calls as soon as it wakes rather than
 * waiting 3 s or more for the next ring */
TEST(incoming_asked_for_on_waking)
{
	ready();
	ui_test_write("/sys/power/state", "");
	ui_test_modem_incoming_unheard("0721555666");
	ui_test_button_press(KEY_POWER, 100); /* asleep, then awake at once (the fake state file) */
	ui_test_wait(1500); /* tests' time only: the next RING comes after 3 s */
	CHECK(ui_test_shows("Incoming call"));
	CHECK(ui_test_shows("0721555666"));
	CHECK(tap_button("Decline"));
	CHECK(call_becomes(-1, 2000));
}

static bool asleep(void)
{
	char state[8] = "";
	FILE *f = fopen("/sys/power/state", "r");
	if (f) {
		if (!fgets(state, sizeof(state), f))
			state[0] = '\0';
		fclose(f);
	}
	return strcmp(state, "mem") == 0;
}

/* The calls asked for on waking go to the modem at once and their answer is read as it
 * comes, not each at the port's next poll (200 ms apart): the call shows within a few
 * frames of waking */
TEST(incoming_on_waking_shown_within_frames)
{
	ready();
	ui_test_write("/sys/power/state", "");
	ui_test_modem_incoming_unheard("0721555666");
	ui_test_button_press(KEY_POWER, 100);
	CHECK(ui_test_wait_until(asleep, 1000)); /* the fake sleep returns at once: awake */
	ui_test_wait(100);
	CHECK(ui_test_shows("Incoming call"));
	CHECK(tap_button("Decline"));
	CHECK(call_becomes(-1, 2000));
}

static bool lit(void)
{
	char brightness[16] = "";
	FILE *f = fopen("/sys/class/backlight/backlight/brightness", "r");
	if (f) {
		if (!fgets(brightness, sizeof(brightness), f))
			brightness[0] = '\0';
		fclose(f);
	}
	return atoi(brightness) > 0;
}

/* Woken by a call, the screen stays dark until the call screen is drawn rather than
 * showing the lock screen first */
TEST(woken_lit_on_the_call_screen)
{
	ready();
	ui_test_write("/sys/power/state", "");
	ui_test_modem_incoming_unheard("0721555666");
	ui_test_button_press(KEY_POWER, 100);
	CHECK(ui_test_wait_until(asleep, 1000));
	CHECK(!lit());
	CHECK(ui_test_wait_until(lit, 300));
	CHECK(ui_test_shows("Incoming call"));
	CHECK(tap_button("Decline"));
	CHECK(call_becomes(-1, 2000));
}

/* Woken by a call: awake while it rings, asleep again 10 s after it ended and the lock
 * screen is back */
TEST(woken_by_a_call_sleeps_again_after_it)
{
	ready();
	ui_test_write("/sys/power/state", "");
	ui_test_modem_incoming_unheard("0721555666");
	ui_test_button_press(KEY_POWER, 100);
	CHECK(ui_test_wait_until(asleep, 1000));
	ui_test_write("/sys/power/state", "");
	CHECK(ui_test_wait_for("Incoming call", 1000));
	ui_test_wait(12000);
	CHECK(!asleep());
	CHECK(ui_test_shows("Incoming call"));
	ui_test_modem_remote_hang_up();
	CHECK(call_becomes(-1, 2000));
	CHECK(ui_test_wait_until(ui_locked, 5000));
	ui_test_wait(9000);
	CHECK(!asleep());
	CHECK(ui_test_wait_until(asleep, 2000));
}

/* A command sent just before the phone slept, its answer lost with the USB reset (as
 * AT$QCRMCALL's once, mobile data starting): it does not hold the calls asked for on
 * waking back for its whole timeout */
TEST(incoming_on_waking_behind_a_lost_answer)
{
	ready();
	ui_test_write("/sys/power/state", "");
	ui_test_modem_silent(true); /* what phone-ui asks now goes unanswered */
	ui_test_modem_network(UI_TEST_NET_ROAMING, "Vodafone"); /* +CREG: it asks for the operator */
	ui_test_wait(500);
	ui_test_modem_silent(false);
	ui_test_modem_incoming_unheard("0721555666");
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(2000); /* tests' time only: the next RING comes after 3 s, the lost answer's
	                     * timeout after 5 */
	CHECK(ui_test_shows("Incoming call"));
	CHECK(tap_button("Decline"));
	CHECK(call_becomes(-1, 2000));
}

static bool call_incoming(void)
{
	return modem_call_state() == MODEM_CALL_INCOMING;
}

/* A call coming in shows at once, without the screens' slide (220 ms): it is what woke
 * the phone, the sooner the better */
TEST(incoming_shown_at_once)
{
	ready();
	ui_test_modem_incoming("0721555666");
	CHECK(ui_test_wait_until(call_incoming, 3000));
	ui_test_wait(48); /* a few frames, well within a slide */
	lv_obj_t *label = ui_test_find("Incoming call");
	CHECK(label != NULL);
	CHECK(label && lv_obj_get_screen(label) == lv_screen_active());
	CHECK(tap_button("Decline"));
	CHECK(call_becomes(-1, 2000));
}

/* A call dialed still slides in, as screens do */
TEST(dialed_slides_in)
{
	ready();
	ui_test_modem_callee(UI_TEST_CALLEE_SILENT, 0);
	CHECK(dial("5550100"));
	lv_obj_t *label = ui_test_find("Calling...");
	CHECK(label != NULL);
	CHECK(hang_up());
}
