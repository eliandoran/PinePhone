---
name: user-docs
description: Write or update the user documentation (docs/, built with Zensical from zensical.toml). Use after any user-facing change (a new app or screen, a setting, a gesture or key, changed behavior, installing or flashing steps), when adding or reorganizing pages, adding screenshots, or using Zensical/Markdown features (admonitions, tabs, keys, icons, navigation).
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
- One page per app or topic. Keep the home page (`docs/index.md`) an overview linking to them.

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
`../index.md`). The build checks them; it does not check images.

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

:material-wifi: :fontawesome-solid-phone:  (icons)

Term
:   Definition list.
````

Tables, fenced code with a copy button (`content.code.copy`), and search are on. Theme
options go under `[project.theme]` (`features`, `palette`). Prefer the defaults; add an
extension or feature only when a page needs it, and build to check it.

## Screenshots

Images go in `docs/images/` (JPEG for screens), named after what they show
(`settings-network.jpg`). The display is 720 x 1440: always set `{ width="300" }` or so.

- From the phone: `just screenshot docs/images/<name>.jpg` (or Power + Volume Down on the
  phone, saved there).
- From the host, without the phone: a UI test can reach the screen and call
  `ui_test_screenshot()` (PNG, see the `phone-ui-test` skill); convert it with ImageMagick
  (dev shell). Do not commit a throwaway test for it.

Use them where they help someone find a control, not on every page. They go stale with UI
changes: when changing a screen, check whether a page shows it.

## Checking

Run `just docs-build`: it must end with "No issues found". For looks, `just docs` and open
the page. Commit the Markdown, images and `zensical.toml` together with the change they
document.
