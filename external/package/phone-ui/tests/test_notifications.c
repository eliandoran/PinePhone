/* Notifications (notify.c): messages unread and missed calls not seen yet, on the lock
 * screen, through the UI (ui_test.h) and the fake modem */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "test.h"
#include "ui_test.h"

#include "modem.h"

#define CONTACTS "/var/lib/contacts/contacts.json"
#define MESSAGES "/var/lib/messages/messages.json"
#define CALLS "/var/lib/phone/calls.jsonl"
#define BEN "+1 415 555 0132"

TEST_SETUP
{
	ui_test_reset();
}

/* A file's content (static), "" if it cannot be read */
static const char *file(const char *path)
{
	static char text[65536];
	FILE *f = fopen(path, "r");
	size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
	if (f)
		fclose(f);
	text[n] = '\0';
	return text;
}

static void contacts(void)
{
	ui_test_write(CONTACTS, "[ { \"name\": \"Ben\", \"number\": \"" BEN "\" } ]");
}

/* Ben's conversation, two messages unread; an older one from Mom, read */
static void unread_messages(void)
{
	time_t now = time(NULL);
	char text[1024];
	snprintf(text, sizeof(text),
	         "[ { \"number\": \"" BEN "\", \"unread\": 2, \"messages\": [\n"
	         "    { \"text\": \"Are you there?\", \"time\": %lld },\n"
	         "    { \"text\": \"Running 10 min late\", \"time\": %lld } ] },\n"
	         "  { \"number\": \"+40744555210\", \"messages\": [\n"
	         "    { \"text\": \"Patience is the hard part\", \"time\": %lld } ] } ]\n",
	         (long long)now - 600, (long long)now - 300, (long long)now - 86400);
	ui_test_write(MESSAGES, text);
}

/* A missed call from Ben not seen yet, an older one seen, an answered one */
static void missed_call(void)
{
	time_t now = time(NULL);
	char text[1024];
	snprintf(text, sizeof(text),
	         "{ \"number\": \"0721555666\", \"time\": %lld, \"kind\": \"missed\", \"duration\": 0 }\n"
	         "{ \"number\": \"0721555666\", \"time\": %lld, \"kind\": \"incoming\", \"duration\": 30 }\n"
	         "{ \"number\": \"+14155550132\", \"time\": %lld, \"kind\": \"missed\", \"duration\": 0, \"new\": true }\n",
	         (long long)now - 7200, (long long)now - 3600, (long long)now - 60);
	ui_test_write(CALLS, text);
}

static bool registered(void)
{
	return modem_network() == MODEM_NET_HOME;
}

/* The modem with a ready SIM, registered */
static void ready(void)
{
	ui_test_start();
	contacts(); /* after: it empties /var/lib */
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(registered, 10000));
}

static void lock(void)
{
	ui_lock();
	ui_test_settle();
	ui_test_wait(1100); /* its list follows changes each second */
}

/* Lock screen */

TEST(nothing_to_show)
{
	ui_test_start();
	lock();
	CHECK(ui_test_shows("Swipe up to unlock"));
	CHECK(!ui_test_shows("Missed call"));
}

TEST(sms_received_on_the_lock_screen)
{
	ready();
	lock();
	ui_test_modem_sms("+14155550132", "Call me back");
	CHECK(ui_test_wait_for("Call me back", 5000));
	CHECK(ui_test_shows("Ben"));
	CHECK(ui_locked());
}

TEST(tapped_on_the_lock_screen_it_opens_the_thread)
{
	contacts();
	unread_messages();
	ui_test_start();
	lock();
	CHECK(ui_test_shows("Ben"));
	CHECK(ui_test_shows("Running 10 min late")); /* the last one */
	CHECK(!ui_test_shows("Are you there?"));
	CHECK(!ui_test_shows("Patience is the hard part")); /* read */
	CHECK(ui_test_tap_text("Running 10 min late"));
	CHECK(!ui_locked());
	CHECK(ui_test_shows("Are you there?")); /* the thread */
	CHECK(strstr(file(MESSAGES), "unread") == NULL);
	/* Seen: gone (the thread closed, which shows it too) */
	ui_home();
	lock();
	CHECK(!ui_test_shows("Running 10 min late"));
}

TEST(dragged_up_from_one_it_unlocks_without_opening_it)
{
	contacts();
	unread_messages();
	ui_test_start();
	lock();
	lv_obj_t *preview = ui_test_find("Running 10 min late");
	CHECK(preview != NULL);
	if (!preview)
		return;
	lv_area_t a;
	lv_obj_get_coords(preview, &a);
	ui_test_swipe(360, a.y1 + 10, 360, a.y1 - 600, 400);
	CHECK(!ui_locked());
	CHECK(ui_at_home());
	CHECK(strstr(file(MESSAGES), "\"unread\": 2") != NULL);
}

TEST(missed_call_on_the_lock_screen)
{
	ready();
	ui_test_modem_incoming("+14155550132");
	CHECK(ui_test_wait_for("Incoming call", 3000));
	ui_test_modem_remote_hang_up();
	ui_test_wait(3000);
	lock();
	CHECK(ui_test_shows("Ben"));
	CHECK(ui_test_shows("Missed call"));
	/* Tapped: Recents, which shows it */
	CHECK(ui_test_tap_text("Missed call"));
	CHECK(!ui_locked());
	CHECK(ui_test_shows("Recents"));
	CHECK(ui_test_shows("Missed"));
	lock();
	CHECK(!ui_test_shows("Missed call"));
}

TEST(missed_calls_from_one_number_together)
{
	ready();
	for (int i = 0; i < 2; i++) {
		ui_test_modem_incoming("+14155550132");
		CHECK(ui_test_wait_for("Incoming call", 3000));
		ui_test_modem_remote_hang_up();
		ui_test_wait(3000);
	}
	lock();
	CHECK(ui_test_shows("2 missed calls"));
	CHECK(!ui_test_shows("Missed call"));
}

/* Kept in the call log: still there after a restart (the file written beforehand) */
TEST(missed_call_kept_until_seen)
{
	contacts();
	missed_call();
	ui_test_start();
	lock();
	CHECK(ui_test_shows("Ben"));
	CHECK(ui_test_shows("Missed call"));
	CHECK(!ui_test_shows("0721555666")); /* seen before */
}

TEST(opening_phone_sees_missed_calls)
{
	contacts();
	missed_call();
	ui_test_start();
	CHECK(ui_test_open("Phone"));
	CHECK(strstr(file(CALLS), "\"new\"") == NULL);
	CHECK(strstr(file(CALLS), "\"missed\"") != NULL); /* still logged */
	lock();
	CHECK(!ui_test_shows("Missed call"));
}

TEST(most_recent_first)
{
	contacts();
	unread_messages(); /* 5 min ago */
	missed_call();     /* 1 min ago */
	ui_test_start();
	lock();
	lv_obj_t *call = ui_test_find("Missed call"), *message = ui_test_find("Running 10 min late");
	CHECK(call && message);
	if (!call || !message)
		return;
	lv_area_t c, m;
	lv_obj_get_coords(call, &c);
	lv_obj_get_coords(message, &m);
	CHECK(c.y1 < m.y1);
}

TEST(romanian)
{
	ui_test_write("/var/lib/phone-ui/settings", "language=ro\n");
	contacts();
	missed_call();
	ui_test_start();
	lock();
	CHECK(ui_test_shows("Apel pierdut"));
}
