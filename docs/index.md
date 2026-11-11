# PinePhone feature phone

<!-- The demo in the phone's front: stylesheets/extra.css -->
<div class="demo-phone">
<iframe src="demo/index.html?poster=../images/generated/home.webp" title="The home screen, which you can try in your browser"></iframe>
</div>

This operating system turns the original PinePhone into a simple, fast feature phone: it starts in a
few seconds, shows a home screen with the time and a menu of apps, and does the everyday
things a phone is for, without a full desktop system underneath.

## Try it in your browser

This is the phone's own software, running in your browser. Press **See it first**, then click
to tap and drag to scroll or swipe; on a phone or tablet, touch it as you would the phone.
Only the hardware is simulated: calls on its **Demo Network** ring but carry no sound, texts
get an automatic reply, any Wi-Fi password of 8 characters or more connects, and there is no
camera, no Power or volume buttons, and no sound but Music's. Weather and Maps reach the
internet through your browser, but most sites refuse to open in Web Lite. Nothing you change
is kept once you leave the page.

Books, Gallery and Music have a few things to try: books from Project Gutenberg, photos by
NASA, and songs by Kevin MacLeod ([incompetech.com](https://incompetech.com), licensed under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)).

## What it does

- **Calls, text messages and mobile data** over the [mobile network](mobile.md), up to 4G:
  [Phone](apps/phone.md), [Messages](apps/messages.md) and [Contacts](apps/contacts.md).
- **Everyday tools**: [Clock](apps/clock.md) (world clock, alarm, stopwatch and timer), [Calendar](apps/calendar.md),
  [Calculator](apps/calculator.md), [Notes](apps/notes.md), [Tasks](apps/tasks.md), [Recorder](apps/recorder.md),
  [Files](apps/files.md) and [Compass](apps/compass.md).
- **Camera and media**: [Camera](apps/camera.md), [Gallery](apps/gallery.md) and [Music](apps/music.md).
- **Internet**, over Wi-Fi or mobile data: [Web Lite](apps/web-lite.md), [Weather](apps/weather.md)
  and [Maps](apps/maps.md).
- **[Settings](apps/settings.md)** for Wi-Fi and mobile networks, the display, sound, date and
  time, battery and storage.
- A game, [Snake](apps/snake.md), and [more apps](external/index.md) that can be installed
  separately, such as [Telegram](external/telegram.md), [GitHub](external/github.md) and
  [Assistant](external/assistant.md).

To put it on your phone, see [Installing](install.md).

## Using the phone

- **Power** wakes the screen and puts it back to sleep.
- **Volume Up** and **Volume Down** change the volume, or the call's volume during a call.
- **Power + Volume Down** takes a screenshot.
- Swipe from the left edge of the screen to go back, a screen at a time.
- Swipe up from the bottom edge (the bar there) to go straight to the home screen, however
  deep you are in an app, also while typing (the bar is under the keyboard).
- On the keyboard, slide a finger along the space bar to move the cursor through the text,
  a character at a time, as on Android; the phone vibrates once as the cursor starts moving.
  Tapping it still types a space.
- Hold backspace to delete quickly: a character at a time at first, then, after about a
  second, whole words.
- Where you type prose (a note, a message, a question to the assistant), the keyboard
  corrects as you type: a strip above the keys shows the word as you typed it, then the words
  you probably meant. A space or punctuation puts in the one shown in white; tap a word in the
  strip to use it instead: it comes with a space, which punctuation typed next replaces. If a
  correction was not what you wanted, press backspace right after it: your word comes back
  and stays. Capitals carry over, and in English a lone *i* becomes *I*. In Romanian, type
  without diacritics: `stii` becomes `știi`. Names, addresses and codes are left alone, and
  the whole thing turns off with **Settings › Language › Autocorrection**.
- After you copy something (a message in [Telegram](external/telegram.md), held down), the
  keyboard offers it in any field, passwords included: tap :fontawesome-solid-paste: **Paste**
  in the strip above the keys. It is offered while you are not in the middle of a word, once
  per field. The phone forgets what was copied after an hour, and when it restarts.
- Swipe down from the top of the screen (or tap the status bar) for the quick settings; on
  the home screen, swipe down anywhere. Swipe up anywhere on the home screen for the menu of
  apps, and down from its top to close it.
  While they are open, the status bar shows the operator's name on its left. At the top are the
  time and the date, :fontawesome-solid-power-off: (the same menu as holding Power: power off or
  restart) and :fontawesome-solid-gear: (**Settings**, not from the lock screen). Below are the
  Wi-Fi, Bluetooth, mobile data, flashlight and rotation lock tiles (hold Wi-Fi or Bluetooth to
  open its settings; while connected, they show the network's or the device's name). Drag the
  brightness (:fontawesome-solid-sun:) and volume (:fontawesome-solid-volume-high:) bars under
  them to set them. Beside them, :fontawesome-solid-moon: switches between the dark and the light
  theme (only on the home screen), and :fontawesome-solid-bell: turns touch vibration on or off.
  Then come the USB connection (with a choice of what the USB is used for while a
  computer is plugged in, see [Connecting to a computer](computer.md)) and the battery. A
  tile is blue while on. The quick settings also list your [notifications](notifications.md).
- The home screen, the menu and some apps turn sideways with the phone. **Rotation lock** in
  the quick settings keeps them the way the screen is when you turn it on, upright or
  sideways, however you hold the phone; tap it again to let them turn. The phone remembers
  it when it restarts.

![The quick settings](images/generated/quick-settings.webp){ width="300" }

## Hardware

The firmware runs on the original PinePhone (Allwinner A64). The PinePhone Pro and other
phones are not supported.

"Firmware" here means what it means on a feature phone: the whole system, flashed and updated
as one image. It replaces the phone's operating system (postmarketOS, Mobian...) rather than
adding to it: nothing in it installs under another system. The bootloader is
[Tow-Boot](https://tow-boot.org/), unchanged.
