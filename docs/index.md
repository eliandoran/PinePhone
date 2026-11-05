# PinePhone feature phone

This firmware turns the original PinePhone into a simple, fast feature phone: it starts in a
few seconds, shows a home screen with the time and a menu of apps, and does the everyday
things a phone is for, without a full desktop system underneath.

<iframe src="demo/index.html?poster=../images/generated/home.webp" title="The home screen, which you can try in your browser"
        width="300" height="600" style="border: 0; max-width: 100%"></iframe>

## What it does

- **Text messages and mobile data** over the [mobile network](mobile.md), up to 4G:
  [Messages](apps/messages.md) and [Contacts](apps/contacts.md). Calls in
  [Phone](apps/phone.md) do not connect yet.
- **Everyday tools**: [Clock](apps/clock.md) (world clock, stopwatch and timer), [Calendar](apps/calendar.md),
  [Calculator](apps/calculator.md), [Notes](apps/notes.md), [Recorder](apps/recorder.md),
  [Files](apps/files.md) and [Compass](apps/compass.md).
- **Camera and media**: [Camera](apps/camera.md), [Gallery](apps/gallery.md) and [Music](apps/music.md).
- **Internet**, over Wi-Fi or mobile data: [Browser](apps/browser.md), [Weather](apps/weather.md),
  [Maps](apps/maps.md) and [Assistant](apps/assistant.md).
- **[Settings](apps/settings.md)** for Wi-Fi and mobile networks, the display, sound, date and
  time, battery and storage.
- A game, [Snake](apps/snake.md), and more apps that can be installed separately.

## Try it in your browser

The home screen at the top of this page is the phone's software, and it runs in your
browser: press **See it first**, then use the mouse as your finger: click to tap, and drag to
scroll or swipe. On a phone or tablet, touch the screen as you would the phone's.

It is the same software as on the phone, with pretend hardware:

- There is no mobile network, no internet, no sound and no camera.
- The Power and volume buttons are not there.
- The battery stays at 76%.
- What you change (notes, settings) is gone when you leave the page.

## Using the phone

- **Power** wakes the screen and puts it back to sleep.
- **Volume Up** and **Volume Down** change the volume.
- **Power + Volume Down** takes a screenshot.
- Swipe down from the top of the screen (or tap the status bar) for the quick settings:
  brightness and volume, Wi-Fi, mobile data, the flashlight, vibration, the light theme and
  the battery.

![The quick settings](images/generated/quick-settings.webp){ width="300" }

## Hardware

The firmware runs on the original PinePhone (Allwinner A64). The PinePhone Pro and other
phones are not supported.
