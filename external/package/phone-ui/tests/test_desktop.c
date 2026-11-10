/* A monitor's own screens (desktop.c): while the phone is at home, a monitor connected
 * over USB-C shows a desktop of its own, its home (the clock, a dock) or, while the
 * phone's menu is open, its app menu; main.c mirrors the phone otherwise. Here on a
 * second display of the monitor's size, as main.c makes it. */

#include <string.h>

#include <GLES2/gl2.h>
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

/* The cursor is an arrow, its tip where the mouse points: white inside a black outline,
 * transparent around it */
static uint32_t arrow_pixel(const lv_image_dsc_t *dsc, int x, int y)
{
	const uint8_t *p = dsc->data + y * dsc->header.stride + x * 4; /* B, G, R, A */
	return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

TEST(mouse_cursor_is_an_arrow)
{
	connect();
	ui_test_mouse("Logitech USB Optical Mouse");
	ui_test_wait(1200);
	lv_obj_t *image = lv_obj_get_child(cursor(), 0);
	CHECK(image && lv_obj_check_type(image, &lv_image_class));
	const lv_image_dsc_t *dsc = lv_image_get_src(image);
	CHECK(dsc && dsc->header.cf == LV_COLOR_FORMAT_ARGB8888);
	int w = dsc->header.w, h = dsc->header.h;
	CHECK(h > w && h >= 32); /* taller than wide, seen on a monitor */
	/* Its tip on the point: the image's top left just above and left of it */
	lv_area_t a;
	lv_obj_get_coords(image, &a);
	CHECK(a.x1 <= pointed().x && a.x1 >= pointed().x - 4);
	CHECK(a.y1 <= pointed().y && a.y1 >= pointed().y - 4);
	/* Down its left edge: outline then white; the far top right corner clear */
	CHECK_INT(arrow_pixel(dsc, 2, h / 2) >> 24, 0xff);
	CHECK_INT(arrow_pixel(dsc, 2, h / 2) & 0xffffff, 0x000000);
	CHECK_INT(arrow_pixel(dsc, w / 4, h / 2), 0xffffffff);
	CHECK_INT(arrow_pixel(dsc, w - 1, 0) >> 24, 0);
	disconnect();
}

/* What the monitor shows at x, y (its display's texture, as ui_test_pixel() the phone's) */
static uint32_t monitor_pixel(int x, int y)
{
	GLint bound;
	GLuint fbo;
	uint8_t p[4];
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       lv_opengles_texture_get_texture_id(monitor), 0);
	glReadPixels(x, MONITOR_H - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)bound);
	glDeleteFramebuffers(1, &fbo);
	return (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

/* Drawn on the monitor: white where the arrow is, over the dark desktop */
TEST(mouse_cursor_shows)
{
	connect();
	ui_test_mouse("Logitech USB Optical Mouse");
	ui_test_wait(1200);
	ui_test_mouse_move(300 - MONITOR_W / 2, 700 - MONITOR_H / 2); /* on the background */
	ui_test_wait(200);
	CHECK_INT(pointed().x, 300);
	CHECK_INT(monitor_pixel(300 + 4, 700 + 20), 0xffffff);
	CHECK(monitor_pixel(300 + 60, 700 + 20) != 0xffffff); /* beside it, the background */
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

static bool monitor_shows_text(const char *text)
{
	return monitor_shows(text) != NULL;
}

/* Its clock turns with the minute, not up to a second after it */
TEST(clock_turns_with_the_minute)
{
	connect();
	CHECK(ui_test_clock_turns(monitor_shows_text));
	disconnect();
}
