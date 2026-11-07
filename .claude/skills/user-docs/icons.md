<!-- Written by external/package/phone-ui/gen-doc-icons.py: do not edit -->
# The UI's icons as Zensical shortcodes

Each glyph phone-ui draws (`LV_SYMBOL_*` from LVGL, `UI_SYMBOL_*` from
`symbols.txt`), the shortcode that shows the same Font Awesome icon in a page, and
where phone-ui uses it. LVGL's glyphs are an older Font Awesome's: the same icon,
drawn a little differently.

| Symbol | Shortcode | Used in |
|---|---|---|
| `LV_SYMBOL_AUDIO` | `:fontawesome-solid-music:` | files.c, home.c, music.c |
| `LV_SYMBOL_VIDEO` | `:fontawesome-solid-film:` | files.c |
| `LV_SYMBOL_LIST` | `:fontawesome-solid-table-list:` | books/books.c, home.c, settings/settings.c |
| `LV_SYMBOL_OK` | `:fontawesome-solid-check:` | assistant/ask.c, assistant/settings.c, compass.c, contacts.c, gallery.c, home.c, settings/datetime.c, settings/display.c, settings/language.c, settings/wifi.c, shade.c, tasks.c |
| `LV_SYMBOL_CLOSE` | `:fontawesome-solid-xmark:` | browser/browser.c, calculator.c, modem.c, widgets.c |
| `LV_SYMBOL_POWER` | `:fontawesome-solid-power-off:` | power.c |
| `LV_SYMBOL_SETTINGS` | `:fontawesome-solid-gear:` | assistant/ask.c, assistant/assistant.c, assistant/conversation.c, home.c, settings/battery.c |
| `LV_SYMBOL_REFRESH` | `:fontawesome-solid-arrows-rotate:` | browser/browser.c, camera.c, power.c, widgets.c |
| `LV_SYMBOL_MUTE` | `:fontawesome-solid-volume-off:` | phone.c, shade.c, volume.c |
| `LV_SYMBOL_VOLUME_MID` | `:fontawesome-solid-volume-low:` | shade.c, volume.c |
| `LV_SYMBOL_VOLUME_MAX` | `:fontawesome-solid-volume-high:` | phone.c, settings/settings.c, shade.c, volume.c |
| `LV_SYMBOL_IMAGE` | `:fontawesome-solid-image:` | files.c, home.c, settings/settings.c |
| `LV_SYMBOL_PREV` | `:fontawesome-solid-backward-step:` | music.c |
| `LV_SYMBOL_PLAY` | `:fontawesome-solid-play:` | home.c, music.c, recorder.c, snake.c |
| `LV_SYMBOL_PAUSE` | `:fontawesome-solid-pause:` | music.c, snake.c |
| `LV_SYMBOL_STOP` | `:fontawesome-solid-stop:` | recorder.c |
| `LV_SYMBOL_NEXT` | `:fontawesome-solid-forward-step:` | music.c |
| `LV_SYMBOL_LEFT` | `:fontawesome-solid-chevron-left:` | browser/browser.c, month.c, ui.c |
| `LV_SYMBOL_RIGHT` | `:fontawesome-solid-chevron-right:` | files.c, month.c, settings/developer.c, settings/settings.c, tasks.c |
| `LV_SYMBOL_PLUS` | `:fontawesome-solid-plus:` | books/books.c, calculator.c, clock/timer.c, contacts.c, home.c, maps.c, notes.c, phone.c, tasks.c |
| `LV_SYMBOL_MINUS` | `:fontawesome-solid-minus:` | books/books.c, calculator.c, clock/timer.c, maps.c, tasks.c |
| `LV_SYMBOL_EYE_OPEN` | `:fontawesome-solid-eye:` | home.c |
| `LV_SYMBOL_WARNING` | `:fontawesome-solid-triangle-exclamation:` | assistant/ask.c, modem.c |
| `LV_SYMBOL_SHUFFLE` | `:fontawesome-solid-shuffle:` | music.c, settings/settings.c |
| `LV_SYMBOL_UP` | `:fontawesome-solid-chevron-up:` | home.c, keyboard.c, lock.c, shade.c, widgets.c |
| `LV_SYMBOL_DOWN` | `:fontawesome-solid-chevron-down:` | home.c, shade.c |
| `LV_SYMBOL_LOOP` | `:fontawesome-solid-retweet:` | music.c |
| `LV_SYMBOL_DIRECTORY` | `:fontawesome-solid-folder:` | files.c, home.c |
| `LV_SYMBOL_UPLOAD` | `:fontawesome-solid-upload:` | keyboard.c |
| `LV_SYMBOL_BARS` | `:fontawesome-solid-bars:` | home.c |
| `LV_SYMBOL_ENVELOPE` | `:fontawesome-solid-envelope:` | contacts.c, home.c, ui.c |
| `LV_SYMBOL_CHARGE` | `:fontawesome-solid-bolt:` | home.c, shade.c, ui.c, weather.c |
| `LV_SYMBOL_BELL` | `:fontawesome-solid-bell:` | clock/timer.c, home.c, settings/settings.c, settings/sound.c, shade.c, ui.c |
| `LV_SYMBOL_KEYBOARD` | `:fontawesome-solid-keyboard:` | home.c, keyboard.c, phone.c |
| `LV_SYMBOL_GPS` | `:fontawesome-solid-location-arrow:` | home.c, weather.c |
| `LV_SYMBOL_FILE` | `:fontawesome-solid-file:` | books/books.c, files.c, home.c |
| `LV_SYMBOL_WIFI` | `:fontawesome-solid-wifi:` | settings/battery.c, settings/settings.c, shade.c |
| `LV_SYMBOL_BATTERY_3` | `:fontawesome-solid-battery-three-quarters:` | settings/settings.c |
| `LV_SYMBOL_USB` | `:fontawesome-brands-usb:` | shade.c |
| `LV_SYMBOL_TRASH` | `:fontawesome-solid-trash-can:` | assistant/conversation.c, contacts.c, notes.c, phone.c, recorder.c, tasks.c, viewer.c |
| `LV_SYMBOL_EDIT` | `:fontawesome-solid-pen:` | assistant/assistant.c, contacts.c, files.c, home.c, messages.c |
| `LV_SYMBOL_BACKSPACE` | `:fontawesome-solid-delete-left:` | keyboard.c, phone.c |
| `LV_SYMBOL_SD_CARD` | `:fontawesome-solid-sd-card:` | settings/settings.c |
| `LV_SYMBOL_NEW_LINE` | `(none in Font Awesome 7 Free: say it in words)` | keyboard.c |
| `UI_SYMBOL_TOWER_BROADCAST` | `:fontawesome-solid-tower-broadcast:` | mobile data: the shade's tile |
| `UI_SYMBOL_TOWER_CELL` | `:fontawesome-solid-tower-cell:` | the mobile network: the shade's row |
| `UI_SYMBOL_SUN` | `:fontawesome-solid-sun:` | brightness, bright: the shade's slider, Settings' slider's end |
| `UI_SYMBOL_SUN_REGULAR` | `:fontawesome-regular-sun:` | brightness, dim: the shade's slider, Settings' slider's start |
| `UI_SYMBOL_MOON` | `:fontawesome-solid-moon:` | dark theme: the shade's tile, beside brightness |
| `UI_SYMBOL_SIM_CARD` | `:fontawesome-solid-sim-card:` | no SIM: the status bar, after the signal bars |
| `UI_SYMBOL_LOCK` | `:fontawesome-solid-lock:` | SIM locked (PIN) or blocked (PUK): the status bar, after the signal bars |
| `UI_SYMBOL_LANGUAGE` | `:fontawesome-solid-language:` | Settings: the Language row |
| `UI_SYMBOL_CODE` | `:fontawesome-solid-code:` | Settings: the Developer mode row |
| `UI_SYMBOL_BOOKMARK` | `:fontawesome-solid-bookmark:` | GitHub (apps/github): a repository bookmarked, its page's header |
| `UI_SYMBOL_BOOKMARK_REGULAR` | `:fontawesome-regular-bookmark:` | GitHub (apps/github): a repository not bookmarked, its page's header |
| `UI_SYMBOL_ANGLES_LEFT` | `:fontawesome-solid-angles-left:` | GitHub (apps/github): a list's first page, its bar of pages |
| `UI_SYMBOL_ANGLES_RIGHT` | `:fontawesome-solid-angles-right:` | GitHub (apps/github): a list's last page, its bar of pages |
| `UI_SYMBOL_CIRCLE_CHECK` | `:fontawesome-solid-circle-check:` | GitHub (apps/github): a check passed, in a list of pull requests and a pull request |
| `UI_SYMBOL_CIRCLE_XMARK` | `:fontawesome-solid-circle-xmark:` | GitHub (apps/github): a check failed |
| `UI_SYMBOL_CIRCLE` | `:fontawesome-solid-circle:` | GitHub (apps/github): a check pending |
| `UI_SYMBOL_CIRCLE_MINUS` | `:fontawesome-solid-circle-minus:` | GitHub (apps/github): a check skipped or cancelled |
| `UI_SYMBOL_PHONE_SLASH` | `:fontawesome-solid-phone-slash:` | missed calls not seen yet: the status bar, after the envelope |
| `UI_SYMBOL_PHONE` | `:fontawesome-solid-phone:` | calling, answering: Phone, Contacts, Messages; home, About phone (LVGL's is Font Awesome 4's, mirrored) |
| `UI_SYMBOL_PHONE_135` | `:fontawesome-solid-phone:{ style="transform: rotate(135deg)" }` | hanging up, declining: the call screen |
| `UI_SYMBOL_USB_270` | `:fontawesome-brands-usb:{ style="transform: rotate(270deg)" }` | USB plugged into a host, upright: the status bar, left of Wi-Fi |
| `UI_SYMBOL_CLOCK` | `:fontawesome-solid-clock:` | Clock: its Clock page, the bar at the bottom |
| `UI_SYMBOL_STOPWATCH` | `:fontawesome-solid-stopwatch:` | Clock: its Stopwatch page, the bar at the bottom |
| `UI_SYMBOL_HOURGLASS_HALF` | `:fontawesome-solid-hourglass-half:` | Clock: its Timer page, the bar at the bottom |
| `UI_SYMBOL_CODE_PULL_REQUEST` | `:fontawesome-solid-code-pull-request:` | GitHub (apps/github): a pull request open, draft or closed (its color says), its list's rows and its state |
| `UI_SYMBOL_CODE_MERGE` | `:fontawesome-solid-code-merge:` | GitHub (apps/github): a pull request merged |
| `UI_SYMBOL_CIRCLE_DOT_REGULAR` | `:fontawesome-regular-circle-dot:` | GitHub (apps/github): an issue open |
| `UI_SYMBOL_CIRCLE_CHECK_REGULAR` | `:fontawesome-regular-circle-check:` | GitHub (apps/github): an issue closed as done |
| `UI_SYMBOL_BAN` | `:fontawesome-solid-ban:` | GitHub (apps/github): an issue closed as not planned or a duplicate |
| `UI_SYMBOL_GHOST` | `:fontawesome-solid-ghost:` | Telegram (apps/telegram): a deleted account's avatar, in the chat list |
