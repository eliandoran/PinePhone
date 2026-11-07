---
name: user-docs
description: Write or update the user documentation (docs/, built with Zensical from zensical.toml). Use after any user-facing change (a new app or screen, a setting, a gesture or key, changed behavior, installing or flashing steps), when adding or reorganizing pages, adding screenshots, showing the UI's icons inline (Font Awesome shortcodes, icons.md), or using Zensical/Markdown features (admonitions, tabs, keys, navigation).
---

# User documentation

`docs/` is the documentation for the people who use the phone, built by Zensical
(`zensical.toml` at the repository root). Developer material does not go here: it lives in
`CLAUDE.md`, the skills, code comments, and `notes/` (investigations, not published).

```
just docs              # preview at http://localhost:8000, reloads on save (dev shell)
just docs 0.0.0.0:8000 # reachable from another machine
just docs-build        # into site/ (ignored), --strict: a broken page link fails it
```

On WSL, `just docs` in WSL is reachable from the Windows browser at `http://localhost:8000`.

## When to write

Every user-facing change comes with its documentation in the same change: a new app or
screen, a new or renamed setting, a key or gesture, a behavior the user notices (what a toggle
does, a default), a removed feature. Update the page that covers it, or add one. Internal
changes (refactors, tests, the harness, build plumbing) need none.

Before writing, read the code for what the user actually sees: the labels exactly as
displayed (`ui_set_text`, `lv_label_set_text*`, `ui_test_tap_text()` in the tests is a good
index of them), defaults, and what each control does. Do not document what you have not
checked in the code; say so in your reply if something could only be confirmed on the phone.

## Writing for users

- The reader holds the phone, not the source: no file names, functions, AT commands, sysfs,
  `just` targets (except on pages about installing the firmware, which are for people with a
  computer at hand).
- Name things as the screen does, in **bold**: **Settings › Network › Mobile data**. Use
  `›` between levels, as the UI does.
- Task first: "To turn on mobile data, …", then steps as a numbered list, one action each.
- Say what happens and why it matters ("Data roaming is off by default: data abroad can cost"),
  defaults included. Short sentences, present tense, "you".
- Limitations honestly, in a `!!! warning` or a plain sentence: what does not work yet.
- One page per app or topic: built-in apps in `docs/apps/<app>.md` (under **Apps** in the nav),
  apps installed separately (`apps/<name>`, plugins) in `docs/external/<app>.md` (under **External
  apps**, listed on its `index.md`),
  one section per screen or task. Keep the home page (`docs/index.md`) an overview linking to them.
  A change to an app updates its page (and its screenshots' tests, which fail when the screen
  no longer matches).

## Zensical specifics

The config is TOML under `[project]` (not MkDocs' `mkdocs.yml`). Pages are Markdown in `docs/`.

**Navigation is explicit**: `nav` in `zensical.toml` lists the pages. A page missing from it
is still built but not in the menu, and no warning says so. Add every new page:

```toml
nav = [
  { "Home" = "index.md" },
  { "Apps" = [
    { "Messages" = "apps/messages.md" },
  ] },
]
```

Links between pages are relative paths to the `.md` file (`[Messages](apps/messages.md)`,
`../index.md`). Zensical checks them; `just docs-build` checks images itself.

These work with the default configuration (checked by building, Zensical 0.0.72):

````markdown
!!! note "Optional title"
    Indented body. Also tip, warning, danger, info, example.

??? tip "Collapsed until clicked"
    Body.

=== "Wi-Fi"
    Content tab.
=== "Mobile data"
    Another tab.

Press ++power++, or ++power+volume-down++ for a screenshot.

![The home screen](images/home.jpg){ width="300" }

- [x] task list item

:fontawesome-solid-trash-can: :fontawesome-regular-sun:  (icons: see Icons below)

Term
:   Definition list.
````

Tables, fenced code with a copy button (`content.code.copy`), and search are on. Theme
options go under `[project.theme]` (`features`, `palette`). Prefer the defaults; add an
extension or feature only when a page needs it, and build to check it.

## Icons

A control drawn as an icon is shown as that icon in the text, the one the screen shows, so
the reader matches the page to the phone. Zensical bundles Font Awesome 7 Free (the UI's
fonts have 7.2), as shortcodes: `:fontawesome-solid-<name>:`, `-regular-`, `-brands-`.

- **Which icon:** the label's text in the code is a symbol (`LV_SYMBOL_TRASH`,
  `UI_SYMBOL_TOWER_BROADCAST`); [icons.md](icons.md) gives its shortcode and where phone-ui
  uses it. Never pick an icon by its looks or name: LVGL's "edit" is `pen`, "refresh" is
  `arrows-rotate`, "list" is `table-list`.
- **How:** the icon, then the words a reader would say, once per paragraph or step:
  "Tap the :fontawesome-solid-trash-can: trash button at the top right." Later in the same
  paragraph the words alone do ("The trash button only shows…"). Icons inside bold or a
  heading render too, but keep them out of headings.
- **Text and icon:** a control with a text label (a shade tile, **Wi-Fi**) is named by its
  text in bold; its icon may come first (:fontawesome-solid-wifi: **Wi-Fi**) where the
  icon is what one looks for.
- **Words for what it does**, not for the picture: the `pen` is the "edit button", the
  `chevron-up` beside a message field the "send button".
- **Its icon changes** (play and pause): show the one it has at that step; where the control
  itself is described, both (:fontawesome-solid-play: :fontawesome-solid-pause: **Play / pause**).
- **Typed signs** stay characters: a key that draws `+`, `−` or `×` as a symbol (Calculator)
  is written as the sign, as in "2 + 3 × 4". One icon for two controls in a paragraph (two
  back buttons): the icon on each, with words telling them apart.
- **Not glyphs:** what shows a level is drawn, not a symbol (Wi-Fi strength, signal bars,
  the battery): describe it, or show the screenshot. A symbol without a Font Awesome icon
  (`LV_SYMBOL_NEW_LINE`) is said in words. Colored round buttons drawn as shapes (Camera's
  shutter, Recorder's record button) are said in words too.

`icons.md` is generated: after changing `symbols.txt` or the symbols phone-ui uses, run
`python3 external/package/phone-ui/gen-doc-icons.py` in the dev shell. `just docs-build`
fails on a Font Awesome shortcode Zensical does not have (it would show as text).

## Screenshots

Screens of phone-ui are **generated**, not committed: `external/package/phone-ui/tests/test_docs.c`
(the shared setup and `shot()`) and `tests/docs/<app>.c` (an app page's, `#include`d by it) have
one UI test per image, which gets to the screen as a user would, checks it shows what
the page says, and saves it with `shot("<name>")`. `just docs-shots` runs it (not part of
`just test`) into `docs/images/generated/<name>.webp` (ignored by git); `just docs`,
`just docs-build` and CI run it first, so the images follow every UI change.

```markdown
![Network settings](images/generated/settings-network.webp){ width="300" }
```

To add one:

1. A `TEST(<app>_<what>)` in `tests/docs/<app>.c` (`test_docs.c` for the home page), with a
   comment naming the page that shows it. The files are one compilation unit: static helpers
   are prefixed with the app's name, and one may use another's (`files.c` uses `gallery.c`'s
   photos, included before it). Its
   `TEST_SETUP` is the same phone for every image: Saturday 14 March 2026, 09:41
   (`ui_test_wall_clock()`), registered on "Fake Mobile" with LTE, Wi-Fi "Home", battery
   76%. Add the data the screen needs (contacts, a conversation, notes) the way the app's
   own tests do (`phone-ui-test` skill), names that look real but are made up.
2. Get there with the harness's taps and swipes, `CHECK` what the page describes (the
   test fails when the screen no longer matches), then `shot("<name>")`, named after what it
   shows (`settings-network`). LVGL's FPS overlay is off unless the test turns on
   `fps_counter` (Settings › Developer mode).
3. Reference it from the page as `../images/generated/<app>-<what>.webp` (pages of apps are in
   `docs/apps/`), then `just docs-shots <app>_` (only those tests; the other images stay) and
   look at the image (Read tool).

The display is 720 x 1440: always give a width (`{ width="300" }` or so).

What the host cannot show (the camera, real calls, the phone in a hand) comes from the
phone: `just screenshot docs/images/<name>.jpg`, committed, in `docs/images/` beside
`generated/`. Use images where they help someone find a control, not on every page.

## Checking

Run `just docs-build`: it must end with "No issues found" (it also fails on an image a
page uses that is not there). For looks, `just docs` and open
the page. Commit the Markdown, images and `zensical.toml` together with the change they
document.
