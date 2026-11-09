<!-- Written by external/package/phone-ui/gen-doc-icons.py: do not edit -->
# The UI's icons as Zensical shortcodes

Each glyph phone-ui draws (`LV_SYMBOL_*` from LVGL, `UI_SYMBOL_*` from
`symbols.txt`), the shortcode that shows the same Font Awesome icon in a page, and
where phone-ui uses it. LVGL's glyphs are an older Font Awesome's: the same icon,
drawn a little differently.

| Symbol | Shortcode | Used in |
|---|---|---|
| `LV_SYMBOL_AUDIO` | `:fontawesome-solid-music:` | apps/files.c, apps/music.c, shell/home.c |
| `LV_SYMBOL_VIDEO` | `:fontawesome-solid-film:` | apps/files.c |
| `LV_SYMBOL_LIST` | `:fontawesome-solid-table-list:` | apps/books/books.c, apps/settings/settings.c, shell/home.c |
| `LV_SYMBOL_OK` | `:fontawesome-solid-check:` | apps/compass.c, apps/contacts.c, apps/gallery.c, apps/settings/datetime.c, apps/settings/display.c, apps/settings/language.c, apps/settings/wifi.c, apps/tasks.c, shell/home.c, shell/shade.c, ui/markdown.c |
| `LV_SYMBOL_CLOSE` | `:fontawesome-solid-xmark:` | apps/browser/browser.c, modem/modem.c, ui/widgets.c |
| `LV_SYMBOL_POWER` | `:fontawesome-solid-power-off:` | shell/power.c, shell/shade.c |
| `LV_SYMBOL_SETTINGS` | `:fontawesome-solid-gear:` | apps/settings/battery.c, shell/home.c, shell/shade.c |
| `LV_SYMBOL_DRIVE` | `:fontawesome-solid-inbox:` | apps/files.c |
| `LV_SYMBOL_REFRESH` | `:fontawesome-solid-arrows-rotate:` | apps/browser/browser.c, apps/camera.c, shell/power.c, ui/widgets.c |
| `LV_SYMBOL_MUTE` | `:fontawesome-solid-volume-off:` | apps/phone.c, shell/shade.c, system/volume.c |
| `LV_SYMBOL_VOLUME_MID` | `:fontawesome-solid-volume-low:` | shell/shade.c, system/volume.c |
| `LV_SYMBOL_VOLUME_MAX` | `:fontawesome-solid-volume-high:` | apps/phone.c, apps/settings/settings.c, shell/shade.c, system/volume.c |
| `LV_SYMBOL_IMAGE` | `:fontawesome-solid-image:` | apps/files.c, apps/settings/settings.c, shell/home.c |
| `LV_SYMBOL_PREV` | `:fontawesome-solid-backward-step:` | apps/music.c |
| `LV_SYMBOL_PLAY` | `:fontawesome-solid-play:` | apps/music.c, apps/recorder.c, apps/snake.c, shell/home.c |
| `LV_SYMBOL_PAUSE` | `:fontawesome-solid-pause:` | apps/music.c, apps/snake.c |
| `LV_SYMBOL_STOP` | `:fontawesome-solid-stop:` | apps/recorder.c |
| `LV_SYMBOL_NEXT` | `:fontawesome-solid-forward-step:` | apps/music.c |
| `LV_SYMBOL_LEFT` | `:fontawesome-solid-chevron-left:` | apps/browser/browser.c, ui/month.c, ui/nav.c |
| `LV_SYMBOL_RIGHT` | `:fontawesome-solid-chevron-right:` | apps/files.c, apps/settings/developer.c, apps/settings/settings.c, apps/tasks.c, ui/month.c |
| `LV_SYMBOL_PLUS` | `:fontawesome-solid-plus:` | apps/books/books.c, apps/contacts.c, apps/maps.c, apps/notes.c, apps/phone.c, apps/tasks.c, shell/home.c |
| `LV_SYMBOL_MINUS` | `:fontawesome-solid-minus:` | apps/books/books.c, apps/maps.c, apps/tasks.c |
| `LV_SYMBOL_EYE_OPEN` | `:fontawesome-solid-eye:` | shell/home.c |
| `LV_SYMBOL_WARNING` | `:fontawesome-solid-triangle-exclamation:` | modem/modem.c |
| `LV_SYMBOL_SHUFFLE` | `:fontawesome-solid-shuffle:` | apps/music.c, apps/settings/settings.c |
| `LV_SYMBOL_UP` | `:fontawesome-solid-chevron-up:` | shell/home.c, shell/lock.c, shell/shade.c, ui/keyboard.c, ui/widgets.c |
| `LV_SYMBOL_DOWN` | `:fontawesome-solid-chevron-down:` | shell/home.c, shell/shade.c, ui/keyboard.c |
| `LV_SYMBOL_LOOP` | `:fontawesome-solid-retweet:` | apps/music.c |
| `LV_SYMBOL_DIRECTORY` | `:fontawesome-solid-folder:` | apps/files.c, shell/home.c |
| `LV_SYMBOL_UPLOAD` | `:fontawesome-solid-upload:` | ui/keyboard.c |
| `LV_SYMBOL_BARS` | `:fontawesome-solid-bars:` | shell/home.c |
| `LV_SYMBOL_ENVELOPE` | `:fontawesome-solid-envelope:` | apps/contacts.c, shell/home.c, ui/status.c |
| `LV_SYMBOL_CHARGE` | `:fontawesome-solid-bolt:` | apps/weather.c, shell/home.c, shell/shade.c, ui/status.c |
| `LV_SYMBOL_BELL` | `:fontawesome-solid-bell:` | apps/clock/alarm.c, apps/clock/clock.c, apps/settings/settings.c, apps/settings/sound.c, shell/home.c, shell/shade.c, ui/status.c |
| `LV_SYMBOL_KEYBOARD` | `:fontawesome-solid-keyboard:` | apps/phone.c |
| `LV_SYMBOL_GPS` | `:fontawesome-solid-location-arrow:` | apps/weather.c, shell/home.c |
| `LV_SYMBOL_FILE` | `:fontawesome-solid-file:` | apps/books/books.c, apps/files.c, shell/home.c |
| `LV_SYMBOL_WIFI` | `:fontawesome-solid-wifi:` | apps/settings/battery.c, apps/settings/settings.c, shell/shade.c |
| `LV_SYMBOL_BATTERY_3` | `:fontawesome-solid-battery-three-quarters:` | apps/settings/settings.c |
| `LV_SYMBOL_USB` | `:fontawesome-brands-usb:` | shell/shade.c |
| `LV_SYMBOL_BLUETOOTH` | `:fontawesome-brands-bluetooth:` | apps/phone.c, apps/settings/settings.c, shell/shade.c, system/volume.c |
| `LV_SYMBOL_TRASH` | `:fontawesome-solid-trash-can:` | apps/contacts.c, apps/notes.c, apps/phone.c, apps/recorder.c, apps/tasks.c, ui/viewer.c |
| `LV_SYMBOL_EDIT` | `:fontawesome-solid-pen:` | apps/contacts.c, apps/files.c, apps/messages.c, shell/home.c |
| `LV_SYMBOL_BACKSPACE` | `:fontawesome-solid-delete-left:` | apps/phone.c, ui/keyboard.c |
| `LV_SYMBOL_SD_CARD` | `:fontawesome-solid-sd-card:` | apps/settings/settings.c |
| `LV_SYMBOL_NEW_LINE` | `(none in Font Awesome 7 Free: say it in words)` | ui/keyboard.c |
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
| `UI_SYMBOL_CLOCK_REGULAR` | `:fontawesome-regular-clock:` | Phone: its Recents page, the bar at the bottom |
| `UI_SYMBOL_USER` | `:fontawesome-solid-user:` | Contacts: Phone's and Telegram's (apps/telegram) Contacts pages, the bar at the bottom |
| `UI_SYMBOL_DIALPAD` | `(none: phone-ui's own, symbols/dialpad.svg: say it in words)` | Phone: its Keypad page, the bar at the bottom (Free has no dial pad) |
| `UI_SYMBOL_CIRCLE_INFO` | `:fontawesome-solid-circle-info:` | Phone: a recent call's contact or new contact, the end of its row |
| `UI_SYMBOL_STAR` | `:fontawesome-solid-star:` | a favorite: Contacts (the button, the Favorites heading); a repository's stars: GitHub (apps/github) |
| `UI_SYMBOL_STAR_REGULAR` | `:fontawesome-regular-star:` | not a favorite: Contacts, the button |
| `UI_SYMBOL_DIVIDE` | `:fontawesome-solid-divide:` | Calculator: its division key |
| `UI_SYMBOL_EQUALS` | `:fontawesome-solid-equals:` | Calculator: its equals key |
| `UI_SYMBOL_XMARK` | `:fontawesome-solid-xmark:` | Calculator: its multiplication key (LVGL's LV_SYMBOL_CLOSE is an older Font Awesome's, heavier than divide) |
| `UI_SYMBOL_MINUS` | `:fontawesome-solid-minus:` | Calculator: its subtraction key (as xmark) |
| `UI_SYMBOL_PLUS` | `:fontawesome-solid-plus:` | Calculator: its addition key (as xmark) |
| `UI_SYMBOL_PLUS_MINUS` | `:fontawesome-solid-plus-minus:` | Calculator: its key changing the sign |
| `UI_SYMBOL_PERCENT` | `:fontawesome-solid-percent:` | Calculator: its percent key |
| `UI_SYMBOL_BLUETOOTH_B` | `:fontawesome-brands-bluetooth-b:` | Bluetooth on: the status bar, beside Wi-Fi (LVGL's LV_SYMBOL_BLUETOOTH is shorter than its neighbors) |
| `UI_SYMBOL_BOLD` | `:fontawesome-solid-bold:` | the rich text editor's toolbar (Notes): bold |
| `UI_SYMBOL_ITALIC` | `:fontawesome-solid-italic:` | the rich text editor's toolbar (Notes): italic |
| `UI_SYMBOL_UNDERLINE` | `:fontawesome-solid-underline:` | the rich text editor's toolbar (Notes): underline |
| `UI_SYMBOL_LIST_UL` | `:fontawesome-solid-list-ul:` | the rich text editor's toolbar (Notes): a bullet list |
| `UI_SYMBOL_LIST_OL` | `:fontawesome-solid-list-ol:` | the rich text editor's toolbar (Notes): a numbered list |
| `UI_SYMBOL_LIST_CHECK` | `:fontawesome-solid-list-check:` | the rich text editor's toolbar (Notes): a to-do list |
| `UI_SYMBOL_SQUARE_REGULAR` | `:fontawesome-regular-square:` | the rich text editor (Notes): a to-do item not done |
| `UI_SYMBOL_SQUARE_CHECK` | `:fontawesome-solid-square-check:` | the rich text editor (Notes): a to-do item done |
