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
- **Internet**, over Wi-Fi or mobile data: [Web Lite](apps/web-lite.md), [Weather](apps/weather.md),
  [Maps](apps/maps.md) and [Assistant](apps/assistant.md).
- **[Settings](apps/settings.md)** for Wi-Fi and mobile networks, the display, sound, date and
  time, battery and storage.
- A game, [Snake](apps/snake.md), and more apps that can be installed separately, such as
  [GitHub](apps/github.md).

## Try it in your browser

The home screen at the top of this page is the phone's software, and it runs in your
browser: press **See it first**, then use the mouse as your finger: click to tap, and drag to
scroll or swipe. On a phone or tablet, touch the screen as you would the phone's.

It is the same software as on the phone, with pretend hardware:

- A pretend mobile network, **Demo Network**: [calls](apps/phone.md) ring and are answered, but
  carry no sound; a [text](apps/messages.md) gets an automatic reply; a code such as `*100#` gets
  a balance; mobile data turns on and off.
- Pretend Wi-Fi: four networks around, connected to **Home**. Any password of 8 characters or
  more connects.
- A few things to look at: three books from Project Gutenberg in [Books](apps/books.md),
  photos by NASA in [Gallery](apps/gallery.md), and three songs by Kevin MacLeod
  ([incompetech.com](https://incompetech.com), licensed under
  [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)) that [Music](apps/music.md) plays.
- There is no camera, and no sound but Music's.
- It reaches the internet through your browser: [Weather](apps/weather.md) and
  [Maps](apps/maps.md) work, but most web pages do not open in [Web Lite](apps/web-lite.md)
  (it shows **No connection**): sites do not let other sites' pages load them.
- The Power and volume buttons are not there.
- The battery stays at 76%.
- What you change (notes, settings) is gone when you leave the page.

## Using the phone

- **Power** wakes the screen and puts it back to sleep.
- **Volume Up** and **Volume Down** change the volume, or the call's volume during a call.
- **Power + Volume Down** takes a screenshot.
- Swipe from the left edge of the screen to go back, a screen at a time.
- Swipe up from the bottom edge (the bar there) to go straight to the home screen, however
  deep you are in an app. While the keyboard is shown, hide it first.
- On the keyboard, slide a finger along the space bar to move the cursor through the text,
  a character at a time, as on Android. Tapping it still types a space.
- Swipe down from the top of the screen (or tap the status bar) for the quick settings:
  brightness and volume, Wi-Fi, mobile data, the flashlight, vibration, the light theme, the
  rotation lock and the battery.
- The home screen, the menu and some apps turn sideways with the phone. **Rotation lock** in
  the quick settings keeps them the way the screen is when you turn it on, upright or
  sideways, however you hold the phone; tap it again to let them turn. The phone remembers
  it when it restarts.

![The quick settings](images/generated/quick-settings.webp){ width="300" }

## Notifications

Messages you have not read yet and calls you missed are listed on the lock screen, under the
clock, and in the quick settings under **Notifications**. The most recent comes first: a
message shows who sent it and what they wrote last (a blue number when there are several);
a missed call shows who called and **Missed call** in red (**2 missed calls** when they
called more than once). Calls from the same person are listed once. The lock screen lists
the four most recent, the quick settings three, then how many more there are.

- Tap one to open it: a message opens its conversation in **Messages**, a missed call opens
  **Recents** in **Phone**. On the lock screen this also unlocks the phone. Swiping up from a
  notification only unlocks.
- Tap **Clear all** in the quick settings to dismiss them all: the messages count as read,
  the calls as seen.
- While you have any, the status bar shows :fontawesome-solid-envelope: for unread messages
  and :fontawesome-solid-phone-slash: for missed calls, and the **Phone** and **Messages**
  icons on the home screen and in the menu show how many in a red badge.

A notification goes once you have seen what it is about: the conversation opened in
**Messages**, or **Recents** opened in **Phone**. Notifications stay across restarts.

![Notifications on the lock screen](images/generated/lock-notifications.webp){ width="300" }

## Hardware

The firmware runs on the original PinePhone (Allwinner A64). The PinePhone Pro and other
phones are not supported.
