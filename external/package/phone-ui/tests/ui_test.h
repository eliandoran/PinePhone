/*
 * phone-ui on the host, for tests of its screens (ui_test.c). The whole UI runs as
 * main.c starts it, on a 720 x 1440 display drawn by LVGL's OpenGL ES draw unit as on
 * the phone (here in Mesa's llvmpipe, an EGL context without a surface), with LVGL's
 * emulated input and time: time only passes in ui_test_wait() and the functions that
 * wait (each step is a frame, 16 ms). phone-ui's monotonic clock follows it, the
 * wall clock is real. An LVGL error fails the test.
 *
 * Tests run in a sandbox (the Makefile's bubblewrap): the host read-only, /var/lib a
 * directory of the build, no input devices, the hardware's sysfs directories empty.
 *
 * Finding and tapping go by text, as a user sees the screen: labels (and text areas)
 * on the screen on top or the top layer, not hidden, possibly scrolled out of view
 * (taps scroll to them first). Functions that do not find their text return false or
 * NULL and print what the screen shows instead.
 */

#ifndef PHONE_UI_UI_TEST_H
#define PHONE_UI_UI_TEST_H

#include <stdbool.h>

#include "ui.h"

/* Empties /var/lib and puts a phone's hardware in /sys (the sandbox's, see
 * ui_test.c: backlight, battery, thermal zones, PMIC ADC, cpufreq); exits if not in
 * the tests' sandbox. Done by ui_test_start() if not before: call it first to change
 * files phone-ui reads at startup (settings, sysfs). */
void ui_test_reset(void);
/* Starts phone-ui on the home screen, once (in TEST_SETUP, or after ui_test_reset();
 * ui_test_open() does it if nothing did) */
void ui_test_start(void);

/* Time passes: timers, animations and a frame every 16 ms */
void ui_test_wait(uint32_t ms);
/* Waits until transitions and animations are over (at most 5 s) */
void ui_test_settle(void);

/* The label showing exactly `text`, NULL if none */
lv_obj_t *ui_test_find(const char *text);
bool ui_test_shows(const char *text);
/* All the labels showing `text` (at most max, in the order shown), how many there are */
unsigned ui_test_find_all(const char *text, lv_obj_t **found, unsigned max);
/* A label whose text contains `part` */
bool ui_test_shows_part(const char *part);
/* Taps an object in its middle with the emulated finger (scrolled into view
 * first), then waits for what it started */
void ui_test_tap(lv_obj_t *obj);
/* Taps `text`: of several labels showing it, one a finger can tap (a key, not
 * the number it typed) */
bool ui_test_tap_text(const char *text);
/* The first object of a class shown (&lv_switch_class, &lv_slider_class...) */
lv_obj_t *ui_test_find_widget(const lv_obj_class_t *cls);
/* All the objects of a class shown (at most max, in the order shown), how many */
unsigned ui_test_find_widgets(const lv_obj_class_t *cls, lv_obj_t **found, unsigned max);
/* Taps a point of the display */
void ui_test_tap_at(int32_t x, int32_t y);
/* The finger from (x1, y1) to (x2, y2) in `ms` (a frame at least), then let go and
 * what it started is over: LVGL's gestures, drags */
void ui_test_swipe(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t ms);
/* Holds the finger on it past the long press time */
void ui_test_long_press(lv_obj_t *obj);
bool ui_test_long_press_text(const char *text);
/* Taps the text field showing `text` or this placeholder ("Message") */
bool ui_test_tap_field(const char *text);
/* Goes back with the header's back button */
bool ui_test_back(void);
/* Opens an app from the menu, as a user does: home, menu, its tile */
bool ui_test_open(const char *app);

/* Types on the on-screen keyboard (shown), key by key, switching to capitals and
 * symbols as needed: ASCII, "\n" and "\b" (backspace) */
bool ui_test_type(const char *text);
/* Presses a key of the on-screen keyboard by its text, e.g. LV_SYMBOL_KEYBOARD (hide) */
bool ui_test_key(const char *key);
/* The text area with the focus (the keyboard types into it), NULL if none */
lv_obj_t *ui_test_textarea(void);
/* Its text, NULL if none */
const char *ui_test_typed(void);

/* The phone's buttons, by their evdev codes (KEY_POWER, KEY_VOLUMEUP, KEY_VOLUMEDOWN
 * of <linux/input-event-codes.h>): pressed or released, read in the next frame.
 * Power is on another device than the volume keys: both can be held at once. */
void ui_test_button(unsigned code, bool pressed);
/* Pressed for `ms`, released, then what it started is over */
void ui_test_button_press(unsigned code, uint32_t ms);

/* The commands phone-ui runs (poweroff, reboot) are fakes that only note that they
 * ran: their names, one per line, "" if none */
const char *ui_test_ran(void);
/* The fakes fail from now on (or succeed again) */
void ui_test_commands_fail(bool fail);

/* A file with `text`, its directories made (data before ui_test_open()) */
bool ui_test_write(const char *path, const char *text);

/* What is shown: one line per text, for messages */
void ui_test_dump(void);
/* A pixel of the display, (0, 0) top left, as the last frame drew it */
lv_color_t ui_test_pixel(int32_t x, int32_t y);
/* The display as a PNG */
bool ui_test_screenshot(const char *path);

#endif
