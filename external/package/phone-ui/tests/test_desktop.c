/* A monitor's own screens (desktop.c): while the phone is at home, a monitor connected
 * over USB-C shows a desktop of its own, its home (the clock, a dock) or, while the
 * phone's menu is open, its app menu; main.c mirrors the phone otherwise. Here on a
 * second display of the monitor's size, as main.c makes it. */

#include <string.h>

#include <linux/input-event-codes.h>

#include "test.h"
#include "ui.h"
#include "ui/ui_internal.h"
#include "ui_test.h"

#define MONITOR_W 1920
#define MONITOR_H 1080

static lv_display_t *monitor;

TEST_SETUP
{
	ui_test_reset();
}

/* The monitor, its desktop started */
static void connect(void)
{
	ui_test_start();
	monitor = lv_opengles_texture_create(MONITOR_W, MONITOR_H);
	desktop_start(monitor);
	ui_test_settle();
}

static void disconnect(void)
{
	desktop_stop();
	lv_display_delete(monitor);
	monitor = NULL;
}

/* A label showing `text` on what the monitor shows */
static lv_obj_t *find_in(lv_obj_t *obj, const char *text)
{
	if (lv_obj_check_type(obj, &lv_label_class) && !lv_obj_is_hidden(obj) &&
	    strcmp(lv_label_get_text(obj), text) == 0)
		return obj;
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = find_in(lv_obj_get_child(obj, i), text);
		if (found)
			return found;
	}
	return NULL;
}

static lv_obj_t *monitor_shows(const char *text)
{
	return find_in(lv_display_get_screen_active(monitor), text);
}

/* The phone at home: the desktop's home, as wide as the monitor, the dock's apps */
TEST(home)
{
	connect();
	CHECK(desktop_shown());
	lv_obj_t *scr = lv_display_get_screen_active(monitor);
	CHECK_INT(lv_obj_get_width(scr), MONITOR_W);
	CHECK(monitor_shows("Apps"));
	CHECK(monitor_shows("Phone"));
	CHECK(!monitor_shows("Calculator")); /* in the menu only */
	CHECK(ui_at_home()); /* the phone at its own home, as before */
	disconnect();
}

/* The phone's menu open: the desktop's app menu, every app in its section */
TEST(menu_follows_the_phone)
{
	connect();
	ui_open_menu();
	ui_test_settle();
	CHECK(desktop_shown());
	CHECK(monitor_shows("Calculator"));
	CHECK(monitor_shows("Notes"));
	CHECK(monitor_shows("Games"));
	ui_home();
	ui_test_settle();
	CHECK(!monitor_shows("Calculator"));
	CHECK(monitor_shows("Apps"));
	disconnect();
}

/* An app's tile picked on the monitor opens it on the phone; meanwhile the monitor
 * mirrors the phone again */
TEST(app_opened_from_the_monitor)
{
	connect();
	ui_open_menu();
	ui_test_settle();
	lv_obj_t *label = monitor_shows("Notes");
	CHECK(label);
	lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, NULL);
	ui_test_settle();
	CHECK(!ui_at_home());
	CHECK(!desktop_shown());
	CHECK(ui_test_shows("Notes")); /* its header on the phone */
	disconnect();
}

/* A mouse (USB on a dock, or Bluetooth) points on the monitor: its cursor appears in
 * the middle, found within a second, and goes where it is moved, kept on the monitor */

static lv_obj_t *cursor(void)
{
	return mouse_cursor();
}

/* Where the cursor's tip is (its top left) */
static lv_point_t pointed(void)
{
	lv_area_t a;
	lv_obj_get_coords(cursor(), &a);
	return (lv_point_t){ a.x1, a.y1 };
}

/* The cursor moved onto the middle of `obj`, then clicked */
static void click(lv_obj_t *obj)
{
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	lv_point_t p = pointed();
	ui_test_mouse_move((a.x1 + a.x2) / 2 - p.x, (a.y1 + a.y2) / 2 - p.y);
	ui_test_wait(100);
	ui_test_mouse_button(BTN_LEFT, true);
	ui_test_wait(100);
	ui_test_mouse_button(BTN_LEFT, false);
	ui_test_settle();
}

TEST(mouse_moves_its_cursor)
{
	connect();
	CHECK(!cursor() || lv_obj_is_hidden(cursor())); /* no mouse yet */
	ui_test_mouse("Logitech USB Optical Mouse");
	ui_test_wait(1200);
	CHECK(cursor() && !lv_obj_is_hidden(cursor()));
	CHECK_INT(pointed().x, MONITOR_W / 2);
	CHECK_INT(pointed().y, MONITOR_H / 2);
	ui_test_mouse_move(100, -40);
	ui_test_wait(100);
	CHECK_INT(pointed().x, MONITOR_W / 2 + 100);
	CHECK_INT(pointed().y, MONITOR_H / 2 - 40);
	ui_test_mouse_move(-5000, 5000); /* past the corner: in it */
	ui_test_wait(100);
	CHECK_INT(pointed().x, 0);
	CHECK_INT(pointed().y, MONITOR_H - 1);
	ui_test_mouse_gone();
	ui_test_wait(1200);
	CHECK(lv_obj_is_hidden(cursor()));
	disconnect();
}

/* Clicked: Apps opens the menu (the phone's too), an app there opens on the phone */
TEST(mouse_opens_apps)
{
	connect();
	ui_test_mouse("Logitech USB Optical Mouse");
	ui_test_wait(1200);
	CHECK(monitor_shows("Apps"));
	click(lv_obj_get_parent(monitor_shows("Apps")));
	CHECK(nav_menu_on_top());
	CHECK(monitor_shows("Calculator"));
	click(lv_obj_get_parent(monitor_shows("Notes")));
	CHECK(!ui_at_home());
	CHECK(ui_test_shows("Notes"));
	disconnect();
}

/* The wheel scrolls what is under the cursor: the menu, longer than the monitor */
TEST(mouse_wheel_scrolls)
{
	connect();
	ui_open_menu();
	ui_test_settle();
	ui_test_mouse("Logitech USB Optical Mouse");
	ui_test_wait(1200);
	lv_obj_t *grid = lv_obj_get_parent(lv_obj_get_parent(monitor_shows("Notes")));
	CHECK(lv_obj_get_scroll_bottom(grid) > 0);
	ui_test_mouse_wheel(-3); /* towards the user: down the menu */
	ui_test_settle();
	CHECK(lv_obj_get_scroll_y(grid) > 0);
	disconnect();
}

/* Disconnected while the menu shows: nothing left (LeakSanitizer), the phone as it was */
TEST(disconnected)
{
	connect();
	ui_open_menu();
	ui_test_settle();
	disconnect();
	CHECK(!desktop_shown());
	ui_home();
	ui_test_settle();
	CHECK(ui_at_home());
}
