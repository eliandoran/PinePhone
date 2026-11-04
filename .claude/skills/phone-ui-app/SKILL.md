---
name: phone-ui-app
description: Build a new app (or a new screen of an app) for phone-ui, the LVGL feature phone UI in external/package/phone-ui/src. Use when adding an app to the menu, replacing a "Coming soon" placeholder (Gallery, Music, Files, Maps), or adding screens to an existing app.
---

# Building a phone-ui app

phone-ui is one C program (LVGL 9, OpenGL ES draw unit, 720 x 1440 at 269 dpi). Every app
is one file in `external/package/phone-ui/src/`, or a directory once it outgrows one
(`clock/`: a file per part and a private header listing them; the directory is added
to the sources in `phone-ui.mk`); `ui.c`/`ui.h` hold the shared pieces. An app that is
not in the image is a plugin in `apps/<name>/` (`apps/telegram`, see `external.c`).
Read `ui.h` first: its header comment has the rendering rules, and every helper is
documented there. Then read the existing app closest to what you are building:

| App | Shows how to |
|---|---|
| `contacts.c` | static list in cards with sections, detail screen, jumping into another app |
| `notes.c` | files in `/var/lib/<app>`, list that reloads, editor with keyboard, save on leave, confirm bar |
| `messages.c` | per-screen state for a screen opened many times, text field + keyboard, shared data (`data.h`) |
| `clock.c` | pages behind a segmented control, timers, digits that change every frame |
| `calculator.c` | fixed layout without scrolling, key grid, sizing after layout |
| `settings.c` | settings pages (cards of title/value rows), sliders, switches, reading sysfs, theme switch |
| `camera.c` | hardware (V4L2, media controller), GL textures, animations, `#if` for optional features |
| `phone.c` | dial pad, a screen with a timer that dies with it |

## Wiring a new app

1. `src/<app>.c`, starting with a comment that says what the app does and where its data
   lives (see the top of `notes.c`, `camera.c`). The package compiles `src/*.c`: no
   Makefile change.
2. `ui.h`: `void <app>_open(void);` in the "Apps (one file each)" list, with its file.
   Extra entry points other apps call (like `phone_dial()`, `messages_open_thread()`) go
   next to it.
3. `home.c`: the `ui_apps[]` entry: name, an `LV_SYMBOL_*`, an icon color, `<app>_open`.
   For a placeholder, set `open` instead of adding a row.
4. Its icon: `icons/<app>.svg` (lower-case name, 128 x 128 viewBox, rounded square rx 32 with
   transparent corners, a white glyph, no text; copy an existing one), then
   `external/package/phone-ui/gen-icons.sh` to regenerate `src/icons.c`.

## Skeleton

```c
/*
 * Foo: what it shows and does. Data in /var/lib/foo.
 */

#include <stdlib.h>
#include <string.h>

#include "ui.h"

static struct {
	lv_obj_t *value;
	lv_timer_t *timer;
} foo;

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (foo.timer)
		lv_timer_delete(foo.timer);
	memset(&foo, 0, sizeof(foo));
}

void foo_open(void)
{
	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create("Foo", &header, &body);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
	ui_header_button(header, LV_SYMBOL_PLUS, add_cb, NULL); /* optional */
	lv_obj_set_style_pad_hor(body, 16, 0);
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

	/* ... children of body ... */

	ui_push(scr);
}
```

`ui_app_screen_create()` gives the dimmed wallpaper, the header with back button and
title, and `body` (flex column, no padding, fills the rest). The status bar is on the top
layer: never draw it. `ui_push()` slides the screen in; back calls `ui_pop()`, which
deletes it. Sub-screens are built the same way and pushed on top.

## Rules (from ui.h, they matter for frame rate on the GPU draw unit)

- Fixed text: `lv_label_set_text_static()` with a literal or data that outlives the label
  (the title passed to `ui_app_screen_create()` too).
- Text from a limited set (times, counts, names): `ui_set_text()` / `ui_set_text_fmt()`.
- Text changing every frame or typed: `lv_label_set_text()`. For running digits, one
  label per character set to static strings (`clock.c`).
- Long labels: `LV_LABEL_LONG_MODE_CLIP` with a fixed height (`one_line_label()` in
  `notes.c`), never `..._DOTS` on static text.
- No `lv_list_add_*()`, no `lv_msgbox`: build lists from plain objects (card + rows) and
  confirmations as a bar of buttons (`notes.c` delete).
- No shadows (`lv_obj_set_style_shadow_width(obj, 0, 0)` on buttons, the theme adds
  them), no opa/transform/blend styles on containers. Opacity on plain fills is fine.
- Images drawn into a buffer: clear `LV_IMAGE_FLAGS_MODIFIABLE` once it is final and never
  free a buffer an image showed (see the wallpaper in `ui.c`). Live video goes through a
  GL texture and `lv_3dtexture` (`camera.c`), not a changing image.
- Fonts: `&ui_font_16/20/24/26/32/48` only (24 is the default). They have ASCII, Latin-1,
  a few symbols and LVGL's `LV_SYMBOL_*`; other characters do not render. Adding some means
  editing and running `gen-fonts.sh`.

## Look and feel

- Colors: `lv_color_hex(UI_COLOR_TEXT/DIM/CARD/PRESSED/LINE/BUTTON/...)`, read when the
  screen is created (a theme switch rebuilds the screens). `UI_COLOR_ACCENT` for
  highlights; app-specific colors as `#define COLOR_*` at the top of the file
  (`0xff453a` red, `0x30d158`/`0x34c759` green, as the other apps).
- Plain containers: `lv_obj_create()` + `lv_obj_remove_style_all()`, then set what is
  needed. Turn off `clickable`/`scrollable` on containers that should not take taps or
  scroll, so the body scrolls instead.
- Lists: body with `pad_hor` 16, cards with radius 24 and `UI_COLOR_CARD`, rows 112-120 px
  high with `pad_hor` 20-28, a 1 px bottom border in `UI_COLOR_LINE` except on the last
  row, `UI_COLOR_PRESSED` and radius 24 only in `LV_STATE_PRESSED` (a border with a radius
  is a texture of the whole row: scrolling Files went from 43 to 55 FPS without). Copy `row_create()`/`section_create()` from
  `contacts.c` or `notes.c` (each app keeps its own copy).
- Empty state: a centered `UI_COLOR_DIM` label ("No notes") with a top margin.
- Touch targets at least ~72 px; header buttons are 80 x 72; round action buttons 96-136.
- Text sizes: 32 titles and big values, 26 row titles, 20 secondary lines (`UI_COLOR_DIM`).
- `ui_vibrate(15)` on key-like presses and destructive actions.

## State and lifecycle

- A screen that exists once (the app's main screen): a `static struct` of its objects,
  cleared in its `LV_EVENT_DELETE` handler, which also deletes its timers and closes
  devices (`clock.c`, `camera.c`).
- A screen that can be pushed several times or from other apps (an editor, a
  conversation): `calloc` a struct, pass it as event `user_data`, free it on
  `LV_EVENT_DELETE` (`notes.c` editor, `messages.c` thread).
- Timers: `lv_timer_create()` and delete it when the screen is deleted (`screen_timer()`
  in `settings.c`). No threads: poll with a timer, and keep each callback short (the UI
  runs in the same loop).
- Coming back to a screen: a `static unsigned <app>_version` bumped on change, compared in
  the screen's `LV_EVENT_SCREEN_LOAD_START` handler, which rebuilds the list with
  `lv_obj_clean()` when it differs (`notes.c`).
- Saving when leaving: `LV_EVENT_SCREEN_UNLOAD_START` (runs before the previous screen's
  load start, so it sees the change), and again on delete.
- Text input: a textarea plus `ui_keyboard_create(body, ta)` added last to `body`;
  `ui_keyboard_show()` on click, hide on `LV_EVENT_CANCEL`, submit on `LV_EVENT_READY`
  for one-line fields.
- Sizes that depend on layout: `lv_obj_update_layout(scr)` before reading them.

## Data and hardware

- Persistent data: `/var/lib/<app>/` (the data partition; the root filesystem is
  read-only). `mkdir()` it before the first write. Write through `<file>.new`, `fflush`,
  `fsync`, `rename`: power can go at any moment (`note_write()` in `notes.c`).
- Preferences: `ui_setting_get/set[_int]("key", ...)` (one shared file), not a file of
  your own.
- Contacts and conversations are made up in `data.c` until there is an address book and a
  modem driver; use `data.h` rather than a new copy.
- Hardware through its kernel interface (sysfs, ioctl), opened when the screen opens and
  closed on delete. If it is missing, show a status label in the screen and `printf()` a
  line starting with `phone-ui:` or the app name (stdout goes to `/tmp/phone-ui.log`).
  Hardware that needs a kernel driver goes in `board/pinephone/linux.fragment` (see
  CLAUDE.md), which needs a build and a flash, not just `just ui`.

## Testing it

Test first (the `phone-ui-test` skill): before writing a screen, write a UI test in
`tests/test_<app>.c` of what a user does with it (open it from the menu, tap, type, back)
and what they should see or what lands in `/var/lib`, watch it fail, then build the app
until it passes. `just test <app>` runs it on the host in about a second per test, with
ASan watching. Add `<app>` to `UI_TESTS` in `tests/Makefile`.

## Trying it

```
just ui --open <Name>   # build phone-ui only, push it, start in the app (~20 s)
just ui-log             # its output
```

The tests do not see how it looks or how fast it runs, nor real hardware: that is what
the phone is for.

`--open` takes the name from `ui_apps[]`. Read the build output for `-Wall` warnings and
the log start for errors. Check both themes if the app sets colors (Settings > Display).
Per the project rules, one push and a quick check; describe what to try on the phone and
let the user look at the screen. Do not commit: leave the changes for the user to review, and commit only when asked
(message `Phone UI: <app> app`, or `<App>: <change>`).
