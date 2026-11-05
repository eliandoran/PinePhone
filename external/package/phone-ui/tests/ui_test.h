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
#include <stddef.h>

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
/* Waits until `text` shows, at most `ms` (false, and what shows, if it does not), of
 * the tests' time and of real time (for processes, as mpg123's fake) */
bool ui_test_wait_for(const char *text, uint32_t ms);

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
/* Taps a button of a button matrix (a key, the emoji panel's emoji and tabs) by its text
 * where a finger reaches it (not under a panel), scrolled into view first */
bool ui_test_tap_button(const char *text);
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

/*
 * The network: libcurl is fake_curl.c, which answers each request from what the
 * test served for its URL, at the next poll of phone-ui (a timer). URLs are
 * fnmatch() patterns ("https://example.org/search?*"), the last served first;
 * an address nothing serves fails as offline (CURLE_COULDNT_RESOLVE_HOST, printed).
 */
void ui_test_serve(const char *url, long status, const char *type, const char *body);
void ui_test_serve_data(const char *url, long status, const char *type, const void *data, size_t size);
/* Answers the next request for the URL only, then what was served before does:
 * ui_test_serve(url, answer) then ui_test_serve_once(url, first) for a sequence */
void ui_test_serve_once(const char *url, long status, const char *type, const char *body);
/* 302 to `location`, followed by requests that follow redirects */
void ui_test_serve_redirect(const char *url, const char *location);
/* A transfer error, a CURLcode (CURLE_OPERATION_TIMEDOUT...) */
void ui_test_serve_error(const char *url, int error);
/* Requests wait (in progress) until released */
void ui_test_network_hold(bool hold);
/* Bodies arrive `bytes` at a time, one piece per poll (streamed answers); 0: whole */
void ui_test_serve_in_pieces(size_t bytes);

/* A request phone-ui made: its URL, its POST body (NULL: a GET), its headers
 * ("Name: value\n" each) */
struct ui_test_request {
	const char *url, *body, *headers;
};
unsigned ui_test_request_count(void);
/* In the order they were made, i < 0 from the last (-1); NULL if none */
const struct ui_test_request *ui_test_request(int i);
/* The last request whose URL matches the pattern, NULL if none */
const struct ui_test_request *ui_test_requested(const char *pattern);

/*
 * Sound: ALSA's PCM streams are fake_alsa.c's, a card running on the tests' clock
 * (the mixer is alsa-lib's, without a card). The microphone hears a 440 Hz tone
 * of `peak` (0, the default: silence); what playback streams receive is kept.
 */
void ui_test_microphone(int peak);
/* Opening a stream fails as without a sound card */
void ui_test_no_sound_card(bool none);
/* The open streams over- or underrun at their next use (a UI that stalled) */
void ui_test_sound_xrun(void);
/* What the last playback stream received: interleaved samples, NULL if none */
const int16_t *ui_test_played(size_t *frames, unsigned *channels, unsigned *rate);

/* A PNG of one color (0xRRGGBB), malloc'd, to serve as an image */
void *ui_test_png(int w, int h, uint32_t rgb, size_t *size);

/*
 * mpg123 --remote, which Music runs, is a fake first in PATH (ui_test.c): it logs
 * the commands it gets ("LOAD /var/lib/music/a.mp3", "PAUSE", "JUMP 30s") and
 * answers SAMPLE with the position set here. It is a process: what it gets and
 * answers takes real time, which these wait for.
 */
/* Waits (at most `ms` of the tests' time and of real time) until it got this command line */
bool ui_test_mpg123_got(const char *command, uint32_t ms);
/* Forgets the commands it got so far */
void ui_test_mpg123_forget(void);
/* Its position from now on, in samples, of the song's total */
void ui_test_mpg123_at(long long sample, long long total);
/* The song ends (at the next SAMPLE) */
void ui_test_mpg123_end(void);
/* It exits at its next command (killed, crashed) */
void ui_test_mpg123_kill(void);

/*
 * Motion sensors, as on the PinePhone: an MPU6050 accelerometer (with the trigger the
 * compass uses) and an AF8133J magnetometer, IIO devices in the sandbox's sysfs
 * (turned mount matrices, a timestamp channel) whose buffers are files in its /dev.
 * Made by ui_test_sensors() (after ui_test_reset(), before what reads them).
 */
void ui_test_sensors(bool accelerometer, bool magnetometer);
/* Samples of both: up (gravity's opposite, in g) and the magnetic field (gauss), in
 * the phone's axes (x right, y to the top edge, z out of the screen); also the
 * accelerometer's raw values in sysfs */
void ui_test_sensors_feed(const float up[3], const float field[3], unsigned samples);
/* A sensor's sysfs attribute ("iio:device1/buffer0/enable"), "" if none */
const char *ui_test_sensor_attr(const char *dev_attr);

/*
 * Wi-Fi: wpa_supplicant's control interface on wlan0 (fake_wpa.c), answered from a
 * thread, started by the first of these (without them, wpa_supplicant is not
 * running). It connects at once when the password matches the access point's, with
 * the address 192.168.1.42, and refuses with WRONG_KEY otherwise; SAVE_CONFIG writes
 * /var/lib/wpa_supplicant/wpa_supplicant.conf.
 */
void ui_test_wifi_running(void);
/* An access point in range: its SSID ("" hidden), signal in dBm, flags as scan results
 * show them ("[WPA2-PSK-CCMP][ESS]"), password (NULL: open) */
void ui_test_wifi_ap(const char *ssid, int signal, const char *flags, const char *password);
/* A saved network (password NULL: open), connected to if in range with that password */
void ui_test_wifi_saved(const char *ssid, const char *password);
/* An access point's frequency in MHz (2437, channel 6, unless set) */
void ui_test_wifi_frequency(const char *ssid, int mhz);
/* wlan0 is up: phone-ui's SIOCSIFFLAGS reach the fake (--wrap=ioctl), down it disconnects
 * and reports INTERFACE_DISABLED, up it connects again */
bool ui_test_wifi_up(void);

/*
 * Modem: the EG25-G's AT port (fake_modem.c), a pty at /tmp/modem answered from a
 * thread as the modem does: echo until ATE0, +CME ERROR codes, +QSIMSTAT when the card
 * goes or comes once asked for. Without ui_test_modem() there is no port (no modem:
 * powered off, or its switch off). The board's /tmp/modem.conf (MODEM_CONF of modem.o),
 * written by ui_test_reset(), is the PinePhone's on that port: "provider=at",
 * "port=/tmp/modem", "quirks=quectel"; change it before ui_test_start().
 */
enum ui_test_sim { UI_TEST_SIM_ABSENT, UI_TEST_SIM_READY, UI_TEST_SIM_PIN, UI_TEST_SIM_PUK };
/* Plugs the modem in with that SIM; plugged in already, the card changes (+QSIMSTAT
 * when it is removed or inserted) */
void ui_test_modem(enum ui_test_sim sim);
/* The port goes, as the USB device of a modem powered off */
void ui_test_modem_unplug(void);
/* It restarts with that SIM: "RDY", echo on again, nothing else said */
void ui_test_modem_restart(enum ui_test_sim sim);
/* It answers nothing (still starting) */
void ui_test_modem_silent(bool silent);
/* The next `count` AT+CPIN? answer +CME ERROR: 14 (SIM busy) */
void ui_test_modem_sim_busy(unsigned count);
/* The network, registered with a ready SIM only: its state as +CREG/+CEREG give it
 * (reported when it changes, once asked for) and the operator's name (+COPS). Unless
 * set: home, "Fake Mobile" */
enum ui_test_net {
	UI_TEST_NET_NONE = 0, /* not registered, not searching */
	UI_TEST_NET_HOME = 1,
	UI_TEST_NET_SEARCHING = 2,
	UI_TEST_NET_DENIED = 3,
	UI_TEST_NET_ROAMING = 5,
};
void ui_test_modem_network(enum ui_test_net net, const char *operator_name);
/* Registered on LTE only: +CREG (circuit switched) says not registered, +CEREG does */
void ui_test_modem_lte_only(bool only);
/* The signal while registered, in dBm, and the technology (+COPS's <AcT>): AT+CESQ
 * answers the technology's own measure, AT+CSQ the RSSI. Unless set: -80 dBm, LTE */
enum ui_test_tech {
	UI_TEST_TECH_GSM = 0,
	UI_TEST_TECH_UMTS = 2,
	UI_TEST_TECH_EDGE = 3,
	UI_TEST_TECH_HSPA = 6,
	UI_TEST_TECH_LTE = 7,
	UI_TEST_TECH_NR_NSA = 13, /* 5G on an LTE anchor */
};
void ui_test_modem_signal(int dbm, enum ui_test_tech tech);
/* AT+CESQ answers ERROR (a modem without it): AT+CSQ only */
void ui_test_modem_no_cesq(void);
/* The port also shows as a USB serial port while plugged: /dev/<tty> ("ttyUSB2"), in
 * sysfs the tty of the EG25-G's (2c7c:0125) interface 2; another name moves it */
void ui_test_modem_usb(const char *tty);

/* A file with `text`, its directories made (data before ui_test_open()) */
bool ui_test_write(const char *path, const char *text);

/* What is shown: one line per text, for messages */
void ui_test_dump(void);
/* A pixel of the display, (0, 0) top left, as the last frame drew it */
lv_color_t ui_test_pixel(int32_t x, int32_t y);
/* The display as a PNG */
bool ui_test_screenshot(const char *path);

#endif
