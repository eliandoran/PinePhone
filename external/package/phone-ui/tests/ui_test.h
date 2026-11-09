/*
 * phone-ui on the host, for tests of its screens (ui_test.c). The whole UI runs as
 * main.c starts it, on a 720 x 1440 display drawn by LVGL's OpenGL ES draw unit as on
 * the phone (here in Mesa's llvmpipe, an EGL context without a surface), with LVGL's
 * emulated input and time: time only passes in ui_test_wait() and the functions that
 * wait (each step is a frame, 16 ms). phone-ui's monotonic clock follows it, the
 * wall clock is real unless a test sets it (ui_test_wall_clock()). An LVGL error fails
 * the test.
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
#include <time.h>

#include "ui.h"

/* A function the tests' programs link wrapped (-Wl,--wrap=f), declared as f is: the
 * program's calls reach __wrap_f, defined by the fake, which may call __real_f, f itself */
#define UI_TEST_WRAP(f) __typeof__(f) __wrap_##f, __real_##f

/* Called by test.h after each test (weak there): the LVGL errors that test logged */
unsigned test_errors(void);

/* Empties /var/lib and puts a phone's hardware in /sys (the sandbox's, see
 * ui_test.c: backlight, battery, thermal zones, PMIC ADC, cpufreq); exits if not in
 * the tests' sandbox. Done by ui_test_start() if not before: call it first to change
 * files phone-ui reads at startup (settings, sysfs). */
void ui_test_reset(void);
/* What ui_test_reset() writes without emptying anything: the board's sysfs and
 * modem.conf (the simulator's, whose /var/lib is kept) */
void ui_test_board(void);
/* Starts phone-ui on the home screen, once (in TEST_SETUP, or after ui_test_reset();
 * ui_test_open() does it if nothing did) */
void ui_test_start(void);

/* Time passes: timers, animations and a frame every 16 ms */
void ui_test_wait(uint32_t ms);
/* The UI busy for `ms` (a slow frame): a single pass of the loop that long, what the
 * pointer did meanwhile read at its end */
void ui_test_stall(uint32_t ms);
/* The wall clock (time()) is `t` from now on, then runs with the tests' time
 * (ui_test_wait()), for screens that show the date or time (documentation screenshots);
 * 0 makes it real again. Before ui_test_start() for what phone-ui reads at startup. */
void ui_test_wall_clock(time_t t);
/* The phone slept `ms` (after a Power press, which sleeps and wakes at once here):
 * CLOCK_BOOTTIME and the wall clock jump ahead, CLOCK_MONOTONIC does not, as in s2idle */
void ui_test_suspended(uint32_t ms);
/* When the phone would wake for alarms.c's alarms: its timerfds on CLOCK_REALTIME_ALARM
 * and CLOCK_BOOTTIME_ALARM are wrapped (the sandbox may not set wake alarms), what they
 * were last set to kept: the wall clock's second, 0 if disarmed; CLOCK_BOOTTIME's ms, 0 */
time_t ui_test_wake_at(void);
uint64_t ui_test_wake_boot_ms(void);
/* Waits until transitions and animations are over (at most 5 s) */
void ui_test_settle(void);
/* One pass of main.c's loop, `ms` later (the simulator's, in real time) */
void ui_test_frame(uint32_t ms);
/* Waits until `text` shows, at most `ms` (false, and what shows, if it does not), of
 * the tests' time and of real time (for processes, as mpg123's fake) */
bool ui_test_wait_for(const char *text, uint32_t ms);
/* Waits until `done()`, at most `ms` of the tests' time and of real time (the fakes'
 * threads, as the modem's, answer in it): false if it never is */
bool ui_test_wait_until(bool (*done)(void), uint32_t ms);

/* The label showing exactly `text`, NULL if none; a span group (styled text) counts
 * as a label of its spans joined */
lv_obj_t *ui_test_find(const char *text);
bool ui_test_shows(const char *text);
/* The font of the label showing `text` (ui_test_find()), NULL if none */
const lv_font_t *ui_test_font(const char *text);
/* All the labels showing `text` (at most max, in the order shown), how many there are */
unsigned ui_test_find_all(const char *text, lv_obj_t **found, unsigned max);
/* A label whose text contains `part` */
bool ui_test_shows_part(const char *part);
/* A time ("12:34") shown in the status bar */
bool ui_test_status_bar_clock(void);
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
/* A screen's main action (ui_action_button()) showing `symbol`: round, larger than the
 * send button, at the bottom right, clear of the home gesture's strip */
bool ui_test_action_button(const char *symbol);
/* `text` (the end of a list), its list scrolled to the end, above that button */
bool ui_test_clear_of_action_button(const char *text, const char *symbol);
/* Taps a point of the touchscreen (upright: turned, the display's point is elsewhere;
 * taps on objects find it) */
void ui_test_tap_at(int32_t x, int32_t y);
/* The finger from (x1, y1) to (x2, y2) in `ms` (a frame at least), then let go and
 * what it started is over: LVGL's gestures, drags */
void ui_test_swipe(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t ms);
/* Two fingers on a horizontal line centered on (x, y), `from` px apart, moved apart (or
 * together) to `to` px in `ms`, as the touchscreen's LVGL driver hands them to the
 * pointer's gesture recognizers: a pinch. Then lifted; with `lift` false the pointer
 * is let go but the recognizers never hear of it (an end LVGL missed) */
void ui_test_pinch(int32_t x, int32_t y, int32_t from, int32_t to, uint32_t ms, bool lift);
/* The list in the middle of the screen pulled down from its top and let go, far
 * enough to load it again (ui_pull_refresh()) */
void ui_test_pull_down(void);
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
/* Presses a key of the on-screen keyboard by its text, e.g. UI_TEST_KEY_HIDE */
bool ui_test_key(const char *key);
#define UI_TEST_KEY_HIDE LV_SYMBOL_DOWN /* the key that hides the keyboard: a chevron */
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
/* A plugin app installed in /var/lib/apps that phone-ui does not load is what the test
 * expects (one built for other headers): the program is not stopped over it */
void ui_test_plugin_may_not_load(void);

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
/* Answers only the requests whose body has `asked` in it (one GraphQL query), before
 * what was served for the URL alone, even later; the others get that */
void ui_test_serve_asked(const char *url, const char *asked, long status, const char *type, const char *body);
/* 302 to `location`, followed by requests that follow redirects */
void ui_test_serve_redirect(const char *url, const char *location);
/* A transfer error, a CURLcode (CURLE_OPERATION_TIMEDOUT...) */
void ui_test_serve_error(const char *url, int error);
/* More headers for the last answer served ("Set-Cookie: a=b; Path=/\n" each), given to
 * CURLOPT_HEADERFUNCTION by curl_easy_perform() (plugins' threads) */
void ui_test_serve_headers(const char *headers);
/* Requests wait (in progress) until released */
void ui_test_network_hold(bool hold);
/* Only those whose URL matches the pattern wait, until ui_test_network_hold(false) */
void ui_test_network_hold_at(const char *pattern);
/* Bodies arrive `bytes` at a time, one piece per poll (streamed answers); 0: whole */
void ui_test_serve_in_pieces(size_t bytes);

/* An IMAP server (fake_imap.c, Email) listening on the loopback address at the URL's port
 * (imap://127.0.0.1:10143/; plain: libetpan connects to it as to a real one), answered
 * from a thread: greets, logs `user` in with `password` (NO to any other), SELECTs INBOX
 * and answers FETCH of sequence ranges with the messages added, oldest first, since the
 * server was made; UID FETCH of one message's BODYSTRUCTURE and BODY.PEEK[section], UID
 * STORE of \Seen, STATUS, NOOP, LOGOUT too. Subjects and names go quoted, or as literals
 * ({n}CRLF...) as Dovecot sends long and 8-bit ones when ui_test_imap_literals().
 * ui_test_imap_commands(): what phone-ui sent, a line each without the tags;
 * ui_test_imap_logins(): how many succeeded. */
void ui_test_imap_server(const char *url, const char *user, const char *password);
/* A folder besides INBOX (the attributes as LIST gives them: "\\Sent", "\\Noselect"; ""
 * none), the delimiter "/": messages added after it go there, until the next */
void ui_test_imap_folder(const char *name, const char *attributes);
void ui_test_imap_message(const char *from_name, const char *from_address, const char *subject,
                          const char *internaldate, bool seen);
/* The last message added gets a body: its text/plain and/or text/html part (NULL: none;
 * both: multipart/alternative, the HTML second), UTF-8, sent encoded */
void ui_test_imap_body(const char *plain, const char *html);
/* Or its BODYSTRUCTURE (the list after the word) and, for any BODY.PEEK[...] of it, the
 * untagged responses as they are (CRLFs, literals): other charsets, encodings, nesting.
 * A sender's address "" goes as NIL, "a@x, b@y" as several senders (the name the first's) */
void ui_test_imap_raw_body(const char *structure, const char *fetched);
/* Untagged lines (CRLFs included) sent as they are after the messages of a FETCH */
void ui_test_imap_fetch_extra(const char *lines);
/* Commands starting with `command` ("SELECT", "UID FETCH 103 (BODYSTRUCTURE)") answered
 * `answer` ("NO Mailbox is locked") with their tag; NULL: the server hangs up on them */
void ui_test_imap_refuse(const char *command, const char *answer);
/* Another greeting: "* PREAUTH ..." (logged in already, no LOGIN needed), "* BYE ..." */
void ui_test_imap_greeting_text(const char *line);
void ui_test_imap_literals(bool on);
const char *ui_test_imap_commands(void);
unsigned ui_test_imap_logins(void);

/* A request phone-ui made: its URL, its body (NULL: none), its headers
 * ("Name: value\n" each), its method ("GET", "POST", "PATCH"...; "CONNECT" a socket) */
struct ui_test_request {
	const char *url, *body, *headers, *method;
};
unsigned ui_test_request_count(void);
/* In the order they were made, i < 0 from the last (-1); NULL if none */
const struct ui_test_request *ui_test_request(int i);
/* The last request whose URL matches the pattern, NULL if none */
const struct ui_test_request *ui_test_requested(const char *pattern);
/* Connections opened so far: one per request but those that found an idle one to
 * their host in their cache (a curl share's or multi handle's), younger than its
 * CURLOPT_MAXAGE_CONN */
unsigned ui_test_connections(void);

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
/* The open streams' device goes (a USB card unplugged, a Bluetooth speaker out of range):
 * their next write fails with ENODEV, which recovering does not mend */
void ui_test_sound_lost(void);
/* The card goes (a Bluetooth speaker out of reach): the open streams fail with -ENODEV
 * and do not recover, new ones do not open */
void ui_test_sound_gone(void);
/* The ALSA device the last playback stream was opened on ("plug:default"), "" none */
const char *ui_test_played_device(void);
/* What the last playback stream received: interleaved samples, NULL if none */
const int16_t *ui_test_played(size_t *frames, unsigned *channels, unsigned *rate);

/* A PNG of one color (0xRRGGBB), malloc'd, to serve as an image */
void *ui_test_png(int w, int h, uint32_t rgb, size_t *size);

/*
 * mpg123 --remote, which Music runs, is a fake first in PATH (ui_test.c): it logs
 * the commands it gets ("LOAD /var/lib/music/a.mp3", "PAUSE", "JUMP 30s"), first its
 * arguments ("ARGS --remote -o alsa -a phone_bt:DEV=...,RATE=48000": Bluetooth), and
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
 * The touchscreen as plugin apps' threads read it (ui_touch_*(), touch.c; LVGL's pointer
 * is the emulated one): a multi-touch device at /dev/input/event1, a FIFO these write
 * reports into, in panel units (twice the display's pixels). Made by
 * ui_test_touchscreen() (after ui_test_reset()); without it there is none.
 */
void ui_test_touchscreen(void);
/* A finger (slot 0 to 9) down, or moved, at x, y of the display upright; lifted */
void ui_test_touch(int slot, int x, int y);
void ui_test_touch_up(int slot);
/* A game controller's touchpad (a DualShock 4's) at /dev/input/event3: multi-touch axes
 * too, but not on the display (INPUT_PROP_BUTTONPAD, the touchscreen INPUT_PROP_DIRECT) */
void ui_test_touchpad(void);
/* fake_wpa.c's ioctl() wrap asks: true if fd is the touchscreen or Power's device
 * (ui_test_app_power_hold()), its result in *ret */
bool fake_input_ioctl(int fd, unsigned long request, void *arg, int *ret);

/*
 * External apps (external.c): programs in /var/lib/apps/<id>/ (a test installs a shell
 * script) that phone-ui runs with the display and the input handed over, waiting for them
 * to exit: opening one returns once it has. The input's emulated devices are left unread
 * meanwhile (main.c's ui_input_stop(), ui_test.c's).
 */
/* Times an app had the display and phone-ui took it back (main.c's ui_display_hand_over()) */
unsigned ui_test_display_handed_over(void);
/* Power held for `ms` of the tests' time once the next app has the display and the file
 * `when` exists (the app made it: it is up), then let go: from a thread (the UI's loop
 * waits for the app), on Power's device as external.c reads it (/dev/input/event0, a
 * FIFO): pressed, then a repeat each 100 ms of the tests' time, which goes on only once
 * phone-ui read the last one; after, with the real time until the app has exited */
void ui_test_app_power_hold(const char *when, uint32_t ms);

/*
 * The vibration motor, as the PinePhone's gpio-vibrator: an input device at
 * /dev/input/event2 taking a force feedback rumble effect, then played (a file the
 * plays are written to). Made by ui_test_vibrator() (after ui_test_reset(), before the
 * first vibration: phone-ui looks for it once); without it there is none.
 */
void ui_test_vibrator(void);
/* How many times it vibrated, the last one's length in ms in *ms (may be NULL) */
unsigned ui_test_vibrations(unsigned *ms);
/* As fake_input_ioctl(), for the vibrator */
bool fake_vibrator_ioctl(int fd, unsigned long request, void *arg, int *ret);

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
/* Connecting to it takes time: STATUS says ASSOCIATING with its SSID until no longer slow,
 * then it connects */
void ui_test_wifi_slow(const char *ssid, bool slow);
/* wpa_supplicant hung: commands get no reply (phone-ui gives up on each after 1 s) */
void ui_test_wifi_hung(bool hung);
/* wlan0 is up: phone-ui's SIOCSIFFLAGS reach the fake (--wrap=ioctl), down it disconnects
 * and reports INTERFACE_DISABLED, up it connects again */
bool ui_test_wifi_up(void);
/* How many times phone-ui took wlan0 down */
int ui_test_wifi_downs(void);

/*
 * Bluetooth: BlueZ (fake_bluez.c) on a dbus-daemon of the test's own (/tmp/bus, which
 * bluetooth.o takes for the system bus), org.bluez answered from a thread, started by
 * the first of these (without them, bluetoothd is not running). The adapter hci0
 * ("PinePhone"); devices in range are listed once discovering (as BlueZ: with their
 * RSSI only while it discovers). Pairing one with a passkey asks phone-ui's agent to
 * confirm it (RequestConfirmation), without one it pairs at once; paired, it connects.
 */
void ui_test_bt_running(bool powered);
/* A device in range: address, name (NULL: none), RSSI in dBm, passkey (0: none) */
void ui_test_bt_device(const char *address, const char *name, int rssi, unsigned passkey);
/* A device paired earlier, connected or not */
void ui_test_bt_paired(const char *address, const char *name, bool connected);
bool ui_test_bt_powered(void);
bool ui_test_bt_discovering(void);
bool ui_test_bt_discoverable(void);
bool ui_test_bt_is_paired(const char *address);
bool ui_test_bt_is_connected(const char *address);
/* The capability of the agent phone-ui registered ("DisplayYesNo"), "" none */
const char *ui_test_bt_agent(void);
/* A device an A2DP sink (speakers, headphones): while connected, bluez-alsa has its PCM,
 * its stream at `rate` */
void ui_test_bt_audio(const char *address, unsigned rate);
/* A device a headset for calls too (the Hands-Free profile): while connected, bluez-alsa
 * has its HFP-AG PCMs */
void ui_test_bt_headset(const char *address);
/* A paired device connecting or going, from its side */
void ui_test_bt_connected(const char *address, bool connected);
/* How pairing a device asks phone-ui's agent, an Agent1 method: "RequestConfirmation"
 * (`code` its passkey, as ui_test_bt_device() with one), "RequestAuthorization" (no code),
 * "DisplayPasskey" or "DisplayPinCode" (`code` to type on the device: paired once
 * ui_test_bt_typed()), "RequestPinCode" (a code typed on the phone) */
void ui_test_bt_pairing(const char *address, const char *method, const char *code);
void ui_test_bt_typed(const char *address);
/* The device gives up pairing: the agent told (Cancel), Pair fails */
void ui_test_bt_cancel(const char *address);
/* Nothing to connect to once paired (a phone, a computer): Connect fails */
void ui_test_bt_no_profiles(const char *address);
/* bluetoothd stops (the agent released, org.bluez gone from the bus) until
 * ui_test_bt_running() again */
void ui_test_bt_exit(void);
/* The system bus goes (dbus-daemon restarted): every connection closed, until
 * ui_test_bt_running() starts it again, the devices kept */
void ui_test_bt_bus_lost(void);
/* Its PCM's volume, 0-127 (127 when it connects), -1 muted */
int ui_test_bt_volume(const char *address);
/* Its volume changed on the device (its buttons) */
void ui_test_bt_set_volume(const char *address, int volume);

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
/* Before each frame (ui_test_frame()): what phone-ui wrote answered and the events due
 * sent, so the modem keeps pace with the tests' time however loaded the host is */
void ui_test_modem_sync(void);
/* It restarts with that SIM: "RDY", echo on again, nothing else said */
void ui_test_modem_restart(enum ui_test_sim sim);
/* It answers nothing (still starting) */
void ui_test_modem_silent(bool silent);
/* The next `count` AT+CPIN? answer +CME ERROR: 14 (SIM busy) */
void ui_test_modem_sim_busy(unsigned count);
/* The next AT+CPIN? answered with `line` instead ("+CPIN: NOT READY", "+CME ERROR: 13",
 * "+CME ERROR: SIM not inserted" as modems without AT+CMEE=1 say; "" only OK), then OK
 * unless an error */
void ui_test_modem_sim_answer(const char *line);
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
/* The data session (AT$QCRMCALL, started while registered): whether it is up, and how
 * many times it started (starts, if not NULL) */
bool ui_test_modem_data(unsigned *starts);
/* The next `count` starts answer ERROR (as after the modem's USB reset) */
void ui_test_modem_data_fails(unsigned count);
/* The session ends on the modem's side, unsaid (AT$QCRMCALL? tells) */
void ui_test_modem_data_drop(void);
/* Plugged in next (ui_test_modem(), before it) with a session up, as the modem keeps the one
 * an earlier phone-ui started: phone-ui ends without ending it */
void ui_test_modem_data_left(void);
/* The modem's IMEI (AT+CGSN), and its SIM's own number (AT+CNUM) unless changed:
 * another SIM's, or NULL for none stored on it */
#define UI_TEST_IMEI "867698041234567"
#define UI_TEST_NUMBER "+40712345678"
void ui_test_modem_number(const char *number);
/* The modem's data context 1's APN, as it kept it (before ui_test_modem(): it is the
 * network's at power-up); "" the network's own */
void ui_test_modem_context_apn(const char *apn);
/* The APN the network attached with, which data sessions get ("internet" for an
 * empty one; "" detached), and how many times it was attached again (AT+CGATT=1) */
const char *ui_test_modem_apn(unsigned *reattached);
/* SMS, in PDU mode (AT+CMGF=0) with a store of its own. A message received from
 * `sender` ("+40744555210", or a name: "Vodafone") with the time now: stored, then
 * +CMTI once AT+CNMI asked for it (as the modem; listed by AT+CMGL, deleted by
 * AT+CMGD). GSM 7-bit for plain ASCII text, UCS-2 otherwise */
void ui_test_modem_sms(const char *sender, const char *text);
/* A part of a long one: its text, reference, how many parts, which one (from 1) */
void ui_test_modem_sms_part(const char *sender, const char *text, unsigned ref, unsigned total, unsigned seq);
/* How many messages (and parts) it still stores */
unsigned ui_test_modem_sms_stored(void);
/* A message received while phone-ui lists the store next (AT+CMGL): +CMTI after the
 * list, before its OK, the message not in it */
void ui_test_modem_sms_while_listing(const char *sender, const char *text);
/* A message of 8-bit data (a WAP push, a SIM's configuration), stored and reported */
void ui_test_modem_sms_data(const char *sender);
/* The i-th message sent (AT+CMGS, its parts joined), from the first: its text, and
 * its number in *number; NULL if fewer were sent */
const char *ui_test_modem_sms_sent(unsigned i, const char **number);
/* The next `count` parts sent are refused (+CMS ERROR: 500) */
void ui_test_modem_sms_fails(unsigned count);
/* SMS not ready yet: the next `count` AT+CMGF answer +CMS ERROR: 314 (SIM busy) */
void ui_test_modem_sms_busy(unsigned count);
/* The domain SMS are sent in, the modem's NV item sms_domain_pref (AT+QNVFR, AT+QNVFW;
 * kept across restarts): 0 circuit switched (SGs on LTE), 1 IMS (the EG25-G's, unless
 * set). `refused`: the network refuses SMS over IMS (+CMS ERROR: 350), as Digi's.
 * `fixed`: writes answer OK but do not take */
void ui_test_modem_sms_domain(unsigned domain, bool ims_refused, bool fixed);
/* The NV item now, and how many times AT+CFUN=1,1 restarted the modem: OK, then
 * nothing answered for 2 s of the tests' time (a real one goes on a few seconds, then leaves
 * USB), then "RDY" */
unsigned ui_test_modem_sms_domain_now(unsigned *restarts);
/* Commands it received while restarting (not answered) */
unsigned ui_test_modem_restart_commands(void);
/* IMS (Quectel's AT+QCFG="ims", kept across restarts, taking at one): 0 as the carrier
 * profile says, 1 on (the EG25-G's, unless set), 2 off. `fixed`: writes answer OK but do
 * not take. Its value now, and how many times AT+CFUN=1,1 restarted the modem */
void ui_test_modem_ims(unsigned value, bool fixed);
unsigned ui_test_modem_ims_now(unsigned *restarts);
/* Voice calls, one at a time, with a ready SIM, registered (NO CARRIER otherwise).
 * Dialed (ATD<number>;): OK, dialing for half a second, then ringing at the other end,
 * which then does as set here (unless set: answers after 2 s; NO_ANSWER after 30 s;
 * REFUSED: the network refuses the call, NO CARRIER for ATD itself: barred, no credit) */
enum ui_test_callee {
	UI_TEST_CALLEE_ANSWERS, UI_TEST_CALLEE_BUSY, UI_TEST_CALLEE_NO_ANSWER, UI_TEST_CALLEE_SILENT,
	UI_TEST_CALLEE_REFUSED,
};
void ui_test_modem_callee(enum ui_test_callee callee, uint32_t ms);
/* A call coming from `number` ("" or NULL hidden): RING, +CLIP once AT+CLIP=1 asked for
 * it, again every 3 s until answered (ATA) or refused (AT+CHUP) */
void ui_test_modem_incoming(const char *number);
/* The modem's USB link gone and back, the modem going on (its SIM, registration, a call),
 * as on waking from sleep: away, what it says is lost (an incoming call rings unheard);
 * back, a new port, an incoming call's next RING 3 s later */
void ui_test_modem_usb_away(void);
void ui_test_modem_usb_back(void);
/* A call coming in whose first RING went unheard (said while the phone slept and its USB
 * was reset, the port kept): listed by AT+CLCC, its next RING 3 s later */
void ui_test_modem_incoming_unheard(const char *number);
/* The other end hangs up (NO CARRIER), whether answered or not */
void ui_test_modem_remote_hang_up(void);
/* The call's state as +CLCC lists it (0 active, 2 dialing, 3 ringing at the other end,
 * 4 incoming; -1 none, it also lists the data connections as calls of mode 1), and
 * whether the microphone is muted (AT+CMUT=1) */
int ui_test_modem_call(bool *muted);
/* The i-th number dialed (NULL: fewer); the calls hung up or refused (AT+CHUP), and
 * those answered (ATA) */
const char *ui_test_modem_dialed(unsigned i);
unsigned ui_test_modem_hang_ups(unsigned *answered);
/* The keys whose tones were sent during a call (AT+VTS, refused without a connected
 * one), in order */
const char *ui_test_modem_tones(void);
/* The board's call audio script (modem.conf's audio_script=, ui_test_reset()'s logs its
 * arguments): waits up to `ms` for it to have run with `args` ("earpiece", "speaker",
 * "off"), the log emptied up to there */
bool ui_test_call_audio_ran(const char *args, uint32_t ms);
/* USSD (AT+CUSD=1), with a ready SIM, registered: the network's reply to `request` (a
 * code, "*111#", or an answer within a session, "1"), +CUSD a second later with that
 * <m> and `reply` (for DONE, MORE, ENDED; NULL none), in AT+CSCS's character set:
 * hexadecimal UCS-2, or text with "\r\n" line ends inside the quotes. SILENT: none
 * ever. A request without one is refused after its OK (+CME ERROR: 258). The last one
 * scripted for a request wins */
enum ui_test_ussd {
	UI_TEST_USSD_SILENT = -1,
	UI_TEST_USSD_DONE = 0,        /* no answer wanted */
	UI_TEST_USSD_MORE = 1,        /* an answer wanted */
	UI_TEST_USSD_ENDED = 2,       /* by the network */
	UI_TEST_USSD_UNSUPPORTED = 4,
	UI_TEST_USSD_TIMEOUT = 5,
};
void ui_test_modem_ussd(const char *request, int m, const char *reply);
/* A USSD the network sends unasked (+CUSD now; text NULL none) */
void ui_test_modem_ussd_push(int m, const char *text);
/* The i-th request received, decoded from its character set (NULL: fewer); the
 * sessions cancelled (AT+CUSD=2); the commands received while a reply was on its way;
 * whether AT+CSCS is "UCS2" now (the operator's name in +COPS in hexadecimal too) */
const char *ui_test_modem_ussd_sent(unsigned i);
unsigned ui_test_modem_ussd_cancels(void);
unsigned ui_test_modem_ussd_overlaps(void);
bool ui_test_modem_ucs2(void);
/* A reply on its way when the session is cancelled (AT+CUSD=2) still comes, as it crosses
 * the cancel */
void ui_test_modem_ussd_crossing(void);
/* The board's mobile data script (modem.conf's data_script=, ui_test_reset()'s logs
 * its arguments): waits up to `ms` for it to have run with `args` ("up wwan0"), the
 * log emptied up to there */
bool ui_test_mobile_data_ran(const char *args, uint32_t ms);
/* The board's USB mode script (usb_mode.c, ui_test_reset()'s logs its arguments):
 * waits up to `ms` for it to have run with `args` ("mtp", "default"), the log emptied
 * up to there; _fails(): it exits with 1 (refused: a trial boot) */
bool ui_test_usb_mode_ran(const char *args, uint32_t ms);
void ui_test_usb_mode_fails(bool fail);
/* The mode an earlier phone-ui left ("mtp"), as `usb-mode.sh status` reports it */
void ui_test_usb_mode_left(const char *mode);

/*
 * Cameras (fake_camera.c): the PinePhone's media pipeline, the rear OV5640 and the
 * front GC2145 linked to the CSI bridge, /dev/video0 capturing from it. Its nodes
 * answer as the drivers do; streams deliver frames of one color on the tests'
 * clock at the sensor's rate (30 fps, 15 at the rear's full size, 16 the front's).
 * Without ui_test_camera() there are none: "Camera not found".
 */
void ui_test_camera(bool rear, bool front);
/* The color the cameras see (0xRRGGBB, 0x808080 unless set) */
void ui_test_camera_scene(uint32_t rgb);
/* Streams deliver no frames (true), or again */
void ui_test_camera_stall(bool stall);
/* The rear camera's focus status: busy for `busy_polls` reads, then reached or failed */
void ui_test_camera_focus(int busy_polls, bool reached);
/* A focus control's last value set (V4L2_CID_FOCUS_AUTO, the focus zone
 * V4L2_CID_USER_BASE | 0x1001), -1 if never; for V4L2_CID_AUTO_FOCUS_START how often */
int32_t ui_test_camera_control(uint32_t id);
/* What streams now: "ov5640 1280x720 UYVY", "ov5640 2592x1944 BGGR"; NULL nothing */
const char *ui_test_camera_streaming(void);
/* Whether phone-ui has /dev/video0 open */
bool ui_test_camera_open(void);
/* fake_wpa.c's ioctl() wrap asks: true if fd is a camera node, its result in *ret */
bool fake_camera_ioctl(int fd, unsigned long request, void *arg, int *ret);

/*
 * Screenshots (fake_drm.c): /dev/dri/card0 shows the frame LVGL drew last, as the
 * phone's scanout buffer (libdrm's calls wrapped); screenshot.c reads it from there.
 * Made by ui_test_reset(): removing the file is a phone without a display.
 */
enum ui_test_scanout {
	UI_TEST_SCANOUT_SHOWN,       /* the frame, as on the phone (the default) */
	UI_TEST_SCANOUT_OFF,         /* no CRTC showing a framebuffer */
	UI_TEST_SCANOUT_NO_HANDLE,   /* not DRM master nor root: no handle to it */
	UI_TEST_SCANOUT_TILED,       /* a modifier other than linear */
	UI_TEST_SCANOUT_UNMAPPABLE,  /* mapping it refused */
};
void ui_test_scanout(enum ui_test_scanout scanout);

/*
 * WebKit, for the Browser plugin app (apps/web, test_web.c only): fake_webkit.c, linked
 * instead of WPE WebKit and WPEBackend-fdo. Pages come from what the tests serve
 * (fake_curl.c), each shown as a frame of its color: its first `background: #rrggbb`,
 * white without. The view keeps a history (back goes to an entry without asking again).
 */
/* A touch the view received: its type (enum wpe_input_touch_event_type) and where, in
 * device pixels from the view's top left */
struct ui_test_touch {
	int type;
	int32_t x, y;
};
unsigned ui_test_web_touch_count(void);
const struct ui_test_touch *ui_test_web_touch(unsigned i);
/* WPEBackend-fdo does not initialize (no EGL_WL_bind_wayland_display): before opening */
void ui_test_web_broken(bool broken);
/* The web process crashes */
void ui_test_web_crash(void);
/* The page opens a new window (target=_blank, window.open) at url */
void ui_test_web_new_window(const char *url);
/* The last page given as text (webkit_web_view_load_html()), "" if none */
const char *ui_test_web_html(void);
/* Web views alive, frames exported in all, and those not released yet */
unsigned ui_test_web_views(void);
unsigned ui_test_web_frames(void);
unsigned ui_test_web_images(void);
/* Frames show the page's color with a band of UI_TEST_WEB_MARKER across, at device
 * rows UI_TEST_WEB_MARKER_Y to + _H from the top: drawn 1:1, it is there on the screen */
#define UI_TEST_WEB_MARKER 0x2468ac
#define UI_TEST_WEB_MARKER_Y 96
#define UI_TEST_WEB_MARKER_H 24
/* WebKit draws no new frame (it is busy) until let go: what is shown meanwhile */
void ui_test_web_hold_frames(bool hold);
/* The size WebKit lays the page out at (CSS pixels), and the last frame's (device pixels) */
void ui_test_web_size(int32_t *w, int32_t *h);
void ui_test_web_frame_size(int32_t *w, int32_t *h);
/* What the app told it: scale factor, activity state (wpe_view_activity_state_*),
 * user agent, whether a feature (by identifier) is on */
float ui_test_web_scale(void);
uint32_t ui_test_web_activity(void);
const char *ui_test_web_user_agent(void);
bool ui_test_web_feature(const char *identifier);

/* A file with `text`, its directories made (data before ui_test_open()) */
bool ui_test_write(const char *path, const char *text);

/* What is shown: one line per text, for messages */
void ui_test_dump(void);
/* A pixel of the display, (0, 0) top left, as the last frame drew it */
lv_color_t ui_test_pixel(int32_t x, int32_t y);
/* Whether a pixel of about `rgb` (16 per channel) is shown inside obj's area on the screen
 * (HTML pages, drawn content: not labels) */
bool ui_test_shows_color(lv_obj_t *obj, uint32_t rgb);
/* The display's pixels as LVGL keeps its texture: 720 x 1440, bottom-up, BGRA */
const uint8_t *ui_test_pixels(void);
/* The display as a PNG */
bool ui_test_screenshot(const char *path);

#endif
