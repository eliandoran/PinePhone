/* Phone (phone.c, calls.c): dialer, call screen, recent calls, through the UI (ui_test.h) */

#include <time.h>

#include "test.h"
#include "i18n.h"
#include "ui_test.h"

#include "json.h"

#define CONTACTS "/var/lib/contacts/contacts.json"
#define CALLS "/var/lib/phone/calls.jsonl"

TEST_SETUP
{
	ui_test_start();
}

static void contacts(void)
{
	ui_test_write(CONTACTS, "[ { \"name\": \"Ben\", \"number\": \"+1 415 555 0132\" } ]");
}

/* A line of the call log `age` seconds ago */
static void call_line(FILE *f, const char *number, const char *kind, time_t age, unsigned duration)
{
	fprintf(f, "{ \"number\": \"%s\", \"time\": %lld, \"kind\": \"%s\", \"duration\": %u }\n", number,
	        (long long)(time(NULL) - age), kind, duration);
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

/* Taps the keys; '+' is a long press on 0 */
static bool dial(const char *keys)
{
	for (; *keys; keys++) {
		char key[2] = { *keys == '+' ? '0' : *keys, '\0' };
		if (*keys == '+' ? !ui_test_long_press_text(key) : !ui_test_tap_text(key))
			return false;
	}
	return true;
}

/* The row (clickable) showing `text` */
static lv_obj_t *row_of(const char *text)
{
	lv_obj_t *obj = ui_test_find(text);
	while (obj && !lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE))
		obj = lv_obj_get_parent(obj);
	return obj;
}

TEST(keypad_without_calls)
{
	CHECK(ui_test_open("Phone"));
	CHECK(ui_test_shows("Phone"));
	CHECK(ui_test_shows("Keypad"));
	CHECK(ui_test_shows("5"));
	CHECK(!ui_test_shows("No recent calls")); /* Recents not shown */
	CHECK(!ui_test_shows(LV_SYMBOL_BACKSPACE)); /* nothing to erase */
}

TEST(dial_and_call)
{
	CHECK(ui_test_open("Phone"));
	CHECK(dial("5550100"));
	CHECK(ui_test_shows("5550100"));
	CHECK(ui_test_tap_text(LV_SYMBOL_CALL));

	CHECK(ui_test_shows("5550100"));
	CHECK(ui_test_shows("No modem")); /* none in these tests: test_calls.c has calls */
	CHECK(ui_test_shows("Mute"));

	/* Logged all the same */
	struct json *last;
	CHECK_INT(calls_logged(&last), 1);
	CHECK_STR(json_str(json_get(last, "number"), NULL), "5550100");
	CHECK_STR(json_str(json_get(last, "kind"), NULL), "outgoing");
	CHECK(llabs(json_int(json_get(last, "time"), 0) - time(NULL)) < 60);
	json_free(last);

	/* Hanging up goes back to the keypad, the number still there */
	CHECK(ui_test_tap_text(LV_SYMBOL_CALL));
	CHECK(ui_test_shows("Keypad"));
	CHECK(ui_test_shows("5550100"));
	CHECK(ui_test_tap_text("Recents"));
	CHECK(ui_test_shows("5550100"));
	CHECK(ui_test_shows("Outgoing, not connected"));
}

TEST(nothing_dialed_no_call)
{
	CHECK(ui_test_open("Phone"));
	CHECK(ui_test_tap_text(LV_SYMBOL_CALL));
	CHECK(!ui_test_shows("Calling..."));
	struct json *last;
	CHECK_INT(calls_logged(&last), 0);
}

TEST(plus_by_long_press_on_zero)
{
	CHECK(ui_test_open("Phone"));
	CHECK(dial("+44"));
	CHECK(ui_test_shows("+44"));
	CHECK(dial("0"));
	CHECK(ui_test_shows("+440"));
}

TEST(backspace)
{
	CHECK(ui_test_open("Phone"));
	CHECK(dial("123"));
	CHECK(ui_test_tap_text(LV_SYMBOL_BACKSPACE));
	CHECK(ui_test_shows("12"));
	/* Held: everything */
	CHECK(ui_test_long_press_text(LV_SYMBOL_BACKSPACE));
	CHECK(!ui_test_shows("12"));
	CHECK(!ui_test_shows(LV_SYMBOL_BACKSPACE));
}

TEST(name_of_the_number_dialed)
{
	contacts();
	CHECK(ui_test_open("Phone"));
	CHECK(dial("+1415555013"));
	CHECK(!ui_test_shows("Ben"));
	CHECK(dial("2"));
	CHECK(ui_test_shows("Ben"));

	CHECK(ui_test_tap_text(LV_SYMBOL_CALL));
	CHECK(ui_test_shows("Ben"));
	CHECK(ui_test_shows("+1 415 555 0132"));
}

TEST(recents)
{
	contacts();
	ui_test_write(CALLS, "");
	FILE *f = fopen(CALLS, "w");
	call_line(f, "+44 7700 900123", "incoming", 3 * 3600, 65);
	call_line(f, "+14155550132", "missed", 2 * 3600, 0);
	call_line(f, "+1 415 555 0132", "missed", 3600, 0);
	call_line(f, "07726", "outgoing", 60, 0);
	fputs("{ \"number\": \"999\", \"ti", f); /* cut by a power loss */
	fclose(f);

	/* With calls, Phone opens on them */
	CHECK(ui_test_open("Phone"));
	CHECK(ui_test_shows("Recents"));
	CHECK(!ui_test_shows("999"));
	CHECK(ui_test_shows("07726"));
	CHECK(ui_test_shows("Outgoing, not connected"));
	/* Two missed calls in a row from the same number (written either way): one row */
	CHECK(ui_test_shows("Ben (2)"));
	CHECK(ui_test_shows("Missed"));
	CHECK(ui_test_shows("+44 7700 900123"));
	CHECK(ui_test_shows("Incoming, 1:05"));

	/* Newest first */
	lv_obj_t *newest = row_of("07726"), *ben = row_of("Ben (2)"), *oldest = row_of("+44 7700 900123");
	CHECK(newest && ben && oldest);
	if (newest && ben && oldest)
		CHECK(lv_obj_get_y(newest) < lv_obj_get_y(ben) && lv_obj_get_y(ben) < lv_obj_get_y(oldest));
}

TEST(call_from_recents)
{
	ui_test_write(CALLS, "");
	FILE *f = fopen(CALLS, "w");
	call_line(f, "+44 7700 900123", "incoming", 3600, 30);
	fclose(f);
	CHECK(ui_test_open("Phone"));
	CHECK(ui_test_tap_text("+44 7700 900123"));
	CHECK(ui_test_shows("No modem"));
	struct json *last;
	CHECK_INT(calls_logged(&last), 2);
	CHECK_STR(json_str(json_get(last, "number"), NULL), "+44 7700 900123");
	json_free(last);

	/* Back on the recents: the new call on top, as a row of its own */
	CHECK(ui_test_tap_text(LV_SYMBOL_CALL));
	CHECK(ui_test_shows("Outgoing, not connected"));
	CHECK(ui_test_shows("Incoming, 0:30"));
}

TEST(info_of_an_unknown_number)
{
	ui_test_write(CALLS, "");
	FILE *f = fopen(CALLS, "w");
	call_line(f, "+44 7700 900123", "missed", 3600, 0);
	fclose(f);
	CHECK(ui_test_open("Phone"));
	lv_obj_t *row = row_of("+44 7700 900123");
	CHECK(row);
	if (!row)
		return;
	/* The (i) button at the end of the row: a new contact with the number */
	ui_test_tap(lv_obj_get_child(row, -1));
	CHECK(ui_test_shows("New contact"));
	CHECK(ui_test_shows("+44 7700 900123"));
}

TEST(clear_recents)
{
	ui_test_write(CALLS, "");
	FILE *f = fopen(CALLS, "w");
	call_line(f, "07726", "outgoing", 60, 0);
	fclose(f);
	CHECK(ui_test_open("Phone"));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text("Cancel"));
	CHECK(ui_test_shows("07726"));

	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH "  Clear recent calls"));
	CHECK(ui_test_shows("No recent calls"));
	CHECK(!ui_test_shows(LV_SYMBOL_TRASH)); /* nothing left to clear */
	struct json *last;
	CHECK_INT(calls_logged(&last), 0);
}

/* In Romanian, switched to meanwhile */
TEST(romanian)
{
	ui_language_set("ro", NULL);
	ui_test_settle();
	CHECK(ui_test_open("Telefon"));
	CHECK(ui_test_shows("Telefon"));
	CHECK(ui_test_shows("Tastatură"));
	CHECK(ui_test_tap_text("Recente"));
	CHECK(ui_test_shows("Niciun apel recent"));
}
