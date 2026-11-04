---
name: phone-ui-test
description: Write and run tests for phone-ui (external/package/phone-ui/tests): unit tests of its C code and UI tests that drive the whole UI headless on the host. Use before fixing a phone-ui bug or adding a feature (the test comes first), when adding tests for an app, when a test fails or ASan reports something, or when extending the test harness (ui_test.c).
---

# Testing phone-ui

Tests run on the host, without the phone or a Buildroot build:

```
just test [filter]   # all tests, or those whose name contains filter (every program)
just coverage        # the same with --coverage: per file, HTML, sources without tests
```

The first run builds `lvgl-host` and `litehtml-host` and downloads Mesa (the flake's
`tests` shell); later runs take a minute or two, about 1 s per UI test. Everything is
built with ASan and UBSan: a memory error fails the test with a report, which is often
the point of the test.

## Test first

For any change to phone-ui that the host can exercise:

1. **Write the test before the code.** For a bug: the steps that show it (what a user
   taps, or the function call), ending in the check that fails today. For a feature: what
   a user does with it and what they should see.
2. **Run it and watch it fail** (`just test <name>`), for the reason you expect: a
   `CHECK` with the wrong value, a text not shown, an ASan report. A test that passes
   before the fix tests something else; fix the test first.
3. **Change the code** until it passes, then run all of `just test`.
4. **Check the new code is reached** when it has branches (`just coverage`, the file's
   missing lines).
5. Commit the fix and its test; when tests find bugs in existing code, the fixes go in a
   commit of their own, before the tests (`git log` has examples).

What the host cannot show, say so and check it on the phone with `just ui`: how it looks
(beyond texts and positions), frame rate, real hardware (modem, camera, audio, sensors,
vibrator), the kernel, init scripts, boot. When the same hardware keeps coming up, a fake
for it in the harness is worth adding (see "Extending the harness").

## Which kind of test

- **Unit test** (`TESTS` in the Makefile): a function or module on its own, e.g. `json.c`,
  the address book in `data.c`, the calculator's state machine. Fast, precise, for logic
  with many cases (parsing, formatting, edge values).
- **UI test** (`UI_TESTS`): the whole of phone-ui, as a user uses it: open an app from
  the menu, tap, type, go back, and check what the screen shows and what was written to
  `/var/lib`. For anything involving screens, navigation, events, keyboards, lifecycles
  (most bugs found so far were there: a use-after-free when sending with Enter, a crash in
  the keyboard's rollover).

Prefer the UI test when in doubt: it checks the code the user runs, and ASan sees all of it.

## Files

`external/package/phone-ui/tests/`:

| File | What |
|---|---|
| `test.h` | the framework: `TEST(name)`, `TEST_SETUP`, `CHECK`, `CHECK_INT`, `CHECK_DBL`, `CHECK_STR`, `TEST_FAIL` |
| `ui_test.h`, `ui_test.c` | the harness: phone-ui started as `main.c` does, the helpers below |
| `stubs.c` | the few `ui.c`/`widgets.c` functions unit tests call |
| `test_<name>.c` | one program per file; `test_notes.c` is the UI template, `test_json.c` the unit one |
| `Makefile` | `TESTS`, `UI_TESTS`, the sandbox (`SANDBOX`), coverage |

A new file: add its name to `TESTS` (with `<name>_SRCS`, the sources it links) or
`UI_TESTS`, and `git add` it (CI runs what is committed).

Each test runs in its own forked process: static state starts fresh, a crash fails only
that test. Checks report and go on. Names must be unique in a file (`TEST(list)` makes a
function `test_list`).

## UI tests

```c
#include "test.h"
#include "ui_test.h"

TEST_SETUP
{
	ui_test_start();
}

TEST(new_note_saved_on_back)
{
	CHECK(ui_test_open("Notes"));            /* home, menu, its tile */
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS)); /* header button */
	CHECK(ui_test_type("Shopping\nmilk"));   /* on-screen keyboard, key by key */
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Shopping"));
}
```

What the harness does (details in `ui_test.h`):

- **Display:** 720 x 1440 at 269 dpi, drawn by the phone's OpenGL ES draw unit in Mesa's
  llvmpipe. `ui_test_screenshot("build/x.png")` saves it: look at it with the Read tool
  when a test does not see what you expect.
- **Time:** LVGL's time only passes in `ui_test_wait(ms)` and the helpers that wait
  (16 ms frames): timers and animations run, a 2 s timer takes `ui_test_wait(2100)`.
  `CLOCK_MONOTONIC` and `CLOCK_BOOTTIME` follow it (UI tests link with
  `--wrap=clock_gettime`, the test's own reads too): measure intervals between taps with
  it (`now()` in `test_clock.c`); a tap acts on release and then waits, so times since a
  tap run a few frames long. The wall clock (`time()`, `CLOCK_REALTIME`) is real.
- **LVGL errors fail the test** (`test_errors()` in `ui_test.c`, asked by `test.h`): e.g.
  "OpenGLES draw unit does not support tasks of type N" (`lv_draw_task_type_t`), which
  on the phone is not drawn right either. To catch one that depends on a frame, redraw
  every frame: `lv_obj_invalidate(lv_screen_active())` in the waiting loop
  (`timer_bar_to_the_end`).
- **Sandbox** (bubblewrap): the host read-only, `/var/lib` and `/sys` are directories of
  the build, no devices, no network, `TZ=UTC0` (the phone has no zoneinfo; the host's
  `/etc/localtime` changed a test's result once). `ui_test_reset()` empties them and puts a
  PinePhone's sysfs in `/sys` (backlight 500/1000, battery 76% discharging, thermal zones,
  PMIC ADC, cpufreq). Never run a test program outside it: the harness refuses (marks in
  both directories).
- **Data before startup:** `ui_test_reset()`, write files, then `ui_test_start()` (for what
  phone-ui reads at startup: settings, sysfs). It starts once: a `TEST_SETUP` that writes
  defaults leaves starting to `ui_test_open()`, so a test can still change them
  (`test_clock.c`). Data loaded when an app first needs it
  (contacts, call log, notes) can be written after `ui_test_start()`, before opening the
  app. `ui_test_write(path, text)` makes the directories.
- **Network** (`fake_curl.c`, linked instead of libcurl): each request is answered from
  what the test served for its URL, at phone-ui's next poll (a timer: wait ~300 ms, or
  `ui_test_wait_for(text, ms)`). Unserved addresses fail as offline and are printed
  ("nothing serves ..."), which is how offline is tested.

  | Helper | Note |
  |---|---|
  | `ui_test_serve(url, status, type, body)`, `ui_test_serve_data(..., data, size)` | fnmatch() patterns (`"https://tile.openstreetmap.org/*"`); the last served wins |
  | `ui_test_serve_once(url, ...)` | answers one request, then the earlier ones do: serve the last answer first, then the first one with `_once` (a model's tool call, then its answer) |
  | `ui_test_serve_redirect(url, to)`, `ui_test_serve_error(url, CURLE_...)` | redirects are followed only by requests that ask (the browser) |
  | `ui_test_network_hold(true)` | requests stay in progress: "Updating...", "Loading...", closing a screen while it loads |
  | `ui_test_serve_in_pieces(bytes)` | a piece per poll, for streamed answers (Assistant) |
  | `ui_test_requested(pattern)`, `ui_test_request(i)`, `ui_test_request_count()` | what phone-ui asked: URL, POST body, headers |
  | `ui_test_png(w, h, rgb, &size)` | a PNG to serve (map tiles, page images) |

  Pages of the browser and map tiles are textures, not labels: check them with
  `ui_test_pixel()`, the title and address, and what was requested.
- **Sound** (`fake_alsa.c`): phone-ui's `snd_pcm_*` calls are wrapped at link time
  (`PCM_WRAPS` in the Makefile; a new one goes there and in the file) onto a card that
  runs on the tests' clock: capture delivers frames at the stream's rate since it
  started, playback consumes them and underruns when it runs out. The mixer and
  controls stay alsa-lib's, without a card (volume.c and the recorder's routing only
  log it).

  | Helper | Note |
  |---|---|
  | `ui_test_microphone(peak)` | a 440 Hz tone of that peak on every channel (0: silence, the default) |
  | `ui_test_played(&frames, &channels, &rate)` | what the last playback stream received, interleaved |
  | `ui_test_sound_xrun()` | the open streams over- or underrun at their next call (a stalled UI) |
  | `ui_test_no_sound_card(true)` | opening a stream fails ("No microphone") |
- **Music's player** (`mpg123 --remote`): a script first in `PATH` that logs its
  commands and answers from files. `ui_test_mpg123_got("LOAD /var/lib/music/a.mp3", ms)`
  waits for a command, `ui_test_mpg123_at(sample, total)` sets the position it reports,
  `_end()` ends the song, `_kill()` makes it exit, `_forget()` clears the log. It is a
  process: its answers take real time, so wait for what they cause
  (`ui_test_wait_for()` sleeps a little each frame) rather than a fixed time.
  `test_music.c` writes MP3 files (ID3v2.4 and v1 tags, a JPEG cover, silent frames).
- **Motion sensors:** `ui_test_sensors(accel, magn)` makes the MPU6050 and AF8133J
  (sysfs with turned mount matrices; their IIO buffers files in the sandbox's `/dev`);
  `ui_test_sensors_feed(up, field, n)` appends n samples of gravity's opposite (g) and
  the field (gauss) in the phone's axes, and the accelerometer's raw sysfs values.
  `ui_test_sensor_attr("iio:device1/buffer0/enable")` reads what phone-ui set.
- **Emoji:** drawn from the image's PNGs (`emoji-host` in the flake, `EMOJI_DIR`), as
  on the phone. The panel's emoji and tabs are button-matrix buttons:
  `ui_test_tap_button(text)` taps one where a finger reaches it (scrolled to it; not a
  key under the panel). Grid buttons show `ui_emoji_compose()` of the emoji.
- **One run at a time:** runs share `build/var-lib` and `build/sys`; the Makefile takes
  `flock build/.run`. Running a test program by hand next to another run, take it too.

Finding and acting go by text, as a user sees the screen:

| Helper | Note |
|---|---|
| `ui_test_shows(text)`, `ui_test_shows_part(part)` | labels on the screen on top or the top layer; hidden ones do not count, scrolled out of view does |
| `ui_test_find(text)`, `ui_test_find_all(text, found, max)` | the label objects, for positions (`lv_obj_get_coords`) or their parents |
| `ui_test_tap_text(text)` | of several labels with the text, the one a finger presses nearest (the key, not the number it typed) |
| `ui_test_long_press_text(text)`, `ui_test_tap(obj)`, `ui_test_long_press(obj)`, `ui_test_tap_at(x, y)` | taps scroll the object into view first, then wait for what they started |
| `ui_test_tap_field("Message")` | a text area by its text or placeholder |
| `ui_test_type(text)` | ASCII, `\n` (Enter), `\b` (backspace): switches shift and symbols as a user would |
| `ui_test_key(LV_SYMBOL_KEYBOARD)` | a keyboard key that is not a character (hide) |
| `ui_test_textarea()`, `ui_test_typed()` | the text area with the focus and its text (NULL: no keyboard) |
| `ui_test_find_widget(&lv_slider_class)`, `ui_test_find_widgets(cls, found, max)` | widgets without text: switches, sliders, photos (`&lv_3dtexture_class`) |
| `ui_test_swipe(x1, y1, x2, y2, ms)` | the finger across, then let go: LVGL gestures (month swipes), drags (photos) |
| `ui_test_back()` | the header's back button |
| `ui_test_button(KEY_POWER, true)`, `ui_test_button_press(KEY_VOLUMEUP, ms)` | the phone's buttons: Power and the volume keys are two devices, both can be held (screenshot combination) |
| `ui_test_ran()`, `ui_test_commands_fail(true)` | `poweroff`/`reboot` are fakes first in `PATH`: what ran, and making them fail |
| `ui_test_dump()` | prints every label shown, with its position |

Helpers that do not find their target return false or NULL and print what the screen
shows: wrap them in `CHECK()`, and read that list first when a test fails.

Things that tripped tests before:

- The same text in several places: section letters and avatar initials ("A"), the
  hang-up button and an unknown caller's avatar (`LV_SYMBOL_CALL`), a key and the number
  it typed. `ui_test_tap_text` picks the tappable one; for checks, use `ui_test_find_all`
  and pick (e.g. the least nested).
- Tapping a text area puts the cursor where the finger lands: set it
  (`lv_textarea_set_cursor_pos(ui_test_textarea(), LV_TEXTAREA_CURSOR_LAST)`) before typing.
- `ui_test_type` types exactly what it is given; a letter in a number field (dial pad) has
  no key and fails.
- Rows with only a glyph (the (i) button in Recents): find the row from its label's
  clickable ancestor and tap `lv_obj_get_child(row, -1)`.
- Symbols and non-ASCII texts are UTF-8: `LV_SYMBOL_*`, `"\xe2\x98\x85"` for a star.
- `LV_SYMBOL_LEFT` is the header's back button too (Calendar's month arrow): pick with
  `ui_test_find_all`.
- Taps and swipes end with `ui_test_settle()`, which waits for animations (at most 5 s):
  with one always running (Snake's steps), use `lv_test_mouse_move_to/press/release` and
  `ui_test_wait` (`finger()` in `test_snake.c`). Pressing during an animation the same way.
- Random state (Snake's food, seeded with the wall clock): read it from the screen
  (positions of the objects, `lv_obj_get_style_x`) and act on it.
- Empty lists: `qsort(NULL, 0, ...)` is undefined and UBSan stops the test (found in
  Notes, Files, Gallery): a test with an empty folder is worth writing.
- Missing hardware only logs (ALSA, vibrator, accelerometer): that is expected.
- Times shown come from the real clock: compute the expected text in the test
  (`strftime`), or check the part that does not depend on it.

## Unit tests

```c
#include "test.h"

#include "json.h"

TEST(numbers)
{
	struct json *v = json_parse("[42, 3.75]");
	CHECK_INT(json_int(v->child, 0), 42);
	CHECK_DBL(json_num(v->child->next, 0), 3.75);
	json_free(v);
}
```

- The sources a test links: `<name>_SRCS` in the Makefile, plus `stubs.c`. A missing
  `ui_*` function at link time goes in `stubs.c`, doing the least that works.
- `static` functions and state: `#include "../src/<app>.c"` in the test
  (`test_calculator.c`, which also clicks its keys through their callbacks).
- Hardcoded paths: unit tests do not run phone-ui's startup, so a path is made
  overridable with `#ifndef` and set for that object in the Makefile (`CONTACTS` in
  `data.c`). UI tests use the real paths inside the sandbox.

## When a test fails

- **A `CHECK`:** the line, and for `CHECK_STR`/`CHECK_INT` the value it had. For a text
  not found, the dump of what was shown. Take a screenshot if that is not enough.
- **ASan:** three stacks: the bad access, where the memory was freed, where it was
  allocated. The `../src/` frames tell what phone-ui did; read the code between the free
  and the use. A real bug: fix it in phone-ui (it is there on the phone too, silently).
- **UBSan:** `runtime error:` with the source line; it aborts the test.
- **A crash in the harness's own code** (a NULL from a helper passed on): check the helper
  first, `CHECK` it and return.
- Run one test alone: `just test <name>`. The program is
  `tests/build/test_<file>`; to run it by hand it must be in the sandbox (`make` prints
  `$(SANDBOX)` with `make -s --eval='p: ; @echo $(SANDBOX)' p` in the `tests` shell).

## Extending the harness

Add to `ui_test.h`/`ui_test.c` what several tests need, generic and named for what a user
does (`ui_test_long_press_text`, not `ui_test_dial_plus`); document it in the header.
A fake for hardware or a service: files in the sandbox's `/sys` from `ui_test_reset()`
for sysfs; a libc call phone-ui makes, wrapped at link time (`-Wl,--wrap=` in the
Makefile's `ui_test_rule`, `__wrap_`/`__real_` in `ui_test.c`, as `clock_gettime`); for
other code (network, a device), a seam in phone-ui only if it is small and keeps the
phone's behavior, and say so in the commit.
Update the Tests paragraph of CLAUDE.md when the harness changes.
