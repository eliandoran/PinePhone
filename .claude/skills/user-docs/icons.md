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
| `LV_SYMBOL_OK` | `:fontawesome-solid-check:` | assistant/ask.c, assistant/settings.c, compass.c, contacts.c, gallery.c, settings/datetime.c, settings/display.c, settings/language.c, settings/wifi.c |
| `LV_SYMBOL_CLOSE` | `:fontawesome-solid-xmark:` | browser/browser.c, calculator.c, modem.c |
| `LV_SYMBOL_POWER` | `:fontawesome-solid-power-off:` | power.c |
| `LV_SYMBOL_SETTINGS` | `:fontawesome-solid-gear:` | assistant/ask.c, assistant/assistant.c, assistant/conversation.c, home.c |
| `LV_SYMBOL_REFRESH` | `:fontawesome-solid-arrows-rotate:` | browser/browser.c, camera.c, power.c, weather.c |
| `LV_SYMBOL_MUTE` | `:fontawesome-solid-volume-off:` | phone.c, shade.c, volume.c |
| `LV_SYMBOL_VOLUME_MID` | `:fontawesome-solid-volume-low:` | shade.c, volume.c |
| `LV_SYMBOL_VOLUME_MAX` | `:fontawesome-solid-volume-high:` | phone.c, settings/settings.c, shade.c, volume.c |
| `LV_SYMBOL_IMAGE` | `:fontawesome-solid-image:` | files.c, home.c, settings/settings.c |
| `LV_SYMBOL_PREV` | `:fontawesome-solid-backward-step:` | music.c |
| `LV_SYMBOL_PLAY` | `:fontawesome-solid-play:` | home.c, music.c, recorder.c, snake.c |
| `LV_SYMBOL_PAUSE` | `:fontawesome-solid-pause:` | music.c, snake.c |
| `LV_SYMBOL_STOP` | `:fontawesome-solid-stop:` | recorder.c |
| `LV_SYMBOL_NEXT` | `:fontawesome-solid-forward-step:` | music.c |
| `LV_SYMBOL_LEFT` | `:fontawesome-solid-chevron-left:` | browser/browser.c, calendar.c, ui.c |
| `LV_SYMBOL_RIGHT` | `:fontawesome-solid-chevron-right:` | calendar.c, files.c, settings/settings.c |
| `LV_SYMBOL_PLUS` | `:fontawesome-solid-plus:` | books/books.c, calculator.c, clock/timer.c, contacts.c, home.c, maps.c, notes.c, phone.c |
| `LV_SYMBOL_MINUS` | `:fontawesome-solid-minus:` | books/books.c, calculator.c, clock/timer.c, maps.c |
| `LV_SYMBOL_EYE_OPEN` | `:fontawesome-solid-eye:` | home.c, settings/settings.c, shade.c |
| `LV_SYMBOL_WARNING` | `:fontawesome-solid-triangle-exclamation:` | assistant/ask.c, modem.c |
| `LV_SYMBOL_SHUFFLE` | `:fontawesome-solid-shuffle:` | music.c, settings/settings.c |
| `LV_SYMBOL_UP` | `:fontawesome-solid-chevron-up:` | home.c, keyboard.c, lock.c, widgets.c |
| `LV_SYMBOL_DOWN` | `:fontawesome-solid-chevron-down:` | home.c |
| `LV_SYMBOL_LOOP` | `:fontawesome-solid-retweet:` | music.c |
| `LV_SYMBOL_DIRECTORY` | `:fontawesome-solid-folder:` | files.c, home.c |
| `LV_SYMBOL_UPLOAD` | `:fontawesome-solid-upload:` | keyboard.c |
| `LV_SYMBOL_CALL` | `:fontawesome-solid-phone:` | contacts.c, data.c, home.c, messages.c, phone.c, settings/about.c |
| `LV_SYMBOL_BARS` | `:fontawesome-solid-bars:` | home.c |
| `LV_SYMBOL_ENVELOPE` | `:fontawesome-solid-envelope:` | contacts.c, home.c |
| `LV_SYMBOL_CHARGE` | `:fontawesome-solid-bolt:` | home.c, shade.c, ui.c, weather.c |
| `LV_SYMBOL_BELL` | `:fontawesome-solid-bell:` | clock/timer.c, home.c, settings/settings.c, settings/sound.c, shade.c |
| `LV_SYMBOL_KEYBOARD` | `:fontawesome-solid-keyboard:` | home.c, keyboard.c, phone.c |
| `LV_SYMBOL_GPS` | `:fontawesome-solid-location-arrow:` | home.c, weather.c |
| `LV_SYMBOL_FILE` | `:fontawesome-solid-file:` | books/books.c, files.c, home.c |
| `LV_SYMBOL_WIFI` | `:fontawesome-solid-wifi:` | settings/settings.c, shade.c |
| `LV_SYMBOL_BATTERY_3` | `:fontawesome-solid-battery-three-quarters:` | settings/settings.c |
| `LV_SYMBOL_USB` | `:fontawesome-brands-usb:` | shade.c, ui.c |
| `LV_SYMBOL_TRASH` | `:fontawesome-solid-trash-can:` | assistant/conversation.c, contacts.c, notes.c, phone.c, recorder.c, viewer.c |
| `LV_SYMBOL_EDIT` | `:fontawesome-solid-pen:` | assistant/assistant.c, contacts.c, files.c, home.c, messages.c |
| `LV_SYMBOL_BACKSPACE` | `:fontawesome-solid-delete-left:` | keyboard.c, phone.c |
| `LV_SYMBOL_SD_CARD` | `:fontawesome-solid-sd-card:` | settings/settings.c |
| `LV_SYMBOL_NEW_LINE` | `(none in Font Awesome 7 Free: say it in words)` | keyboard.c |
| `UI_SYMBOL_TOWER_BROADCAST` | `:fontawesome-solid-tower-broadcast:` | mobile data: the shade's tile |
| `UI_SYMBOL_TOWER_CELL` | `:fontawesome-solid-tower-cell:` | the mobile network: the shade's row |
| `UI_SYMBOL_SUN` | `:fontawesome-solid-sun:` | brightness, bright: the shade's slider, Settings' slider's end |
| `UI_SYMBOL_SUN_REGULAR` | `:fontawesome-regular-sun:` | brightness, dim: the shade's slider, Settings' slider's start |
| `UI_SYMBOL_SIM_CARD` | `:fontawesome-solid-sim-card:` | no SIM: the status bar, after the signal bars |
| `UI_SYMBOL_LOCK` | `:fontawesome-solid-lock:` | SIM locked (PIN) or blocked (PUK): the status bar, after the signal bars |
| `UI_SYMBOL_LANGUAGE` | `:fontawesome-solid-language:` | Settings: the Language row |
| `UI_SYMBOL_BOOKMARK` | `:fontawesome-solid-bookmark:` | GitHub (apps/github): a repository bookmarked, its page's header |
| `UI_SYMBOL_BOOKMARK_REGULAR` | `:fontawesome-regular-bookmark:` | GitHub (apps/github): a repository not bookmarked, its page's header |
| `UI_SYMBOL_ANGLES_LEFT` | `:fontawesome-solid-angles-left:` | GitHub (apps/github): a list's first page, its bar of pages |
| `UI_SYMBOL_ANGLES_RIGHT` | `:fontawesome-solid-angles-right:` | GitHub (apps/github): a list's last page, its bar of pages |
