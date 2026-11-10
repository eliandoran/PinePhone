# PinePhone feature phone

This operating system turns the original PinePhone into a simple, fast feature phone: it starts in a
few seconds, shows a home screen with the time and a menu of apps, and does the everyday
things a phone is for, without a full desktop system underneath.

<iframe src="demo/index.html?poster=../images/generated/home.webp" title="The home screen, which you can try in your browser"
        width="300" height="600" style="border: 0; max-width: 100%"></iframe>

## What it does

- **Calls, text messages and mobile data** over the [mobile network](mobile.md), up to 4G:
  [Phone](apps/phone.md), [Messages](apps/messages.md) and [Contacts](apps/contacts.md).
- **Everyday tools**: [Clock](apps/clock.md) (world clock, alarm, stopwatch and timer), [Calendar](apps/calendar.md),
  [Calculator](apps/calculator.md), [Notes](apps/notes.md), [Tasks](apps/tasks.md), [Recorder](apps/recorder.md),
  [Files](apps/files.md) and [Compass](apps/compass.md).
- **Camera and media**: [Camera](apps/camera.md), [Gallery](apps/gallery.md), [Music](apps/music.md) and [Videos](apps/videos.md).
- **Internet**, over Wi-Fi or mobile data: [Web Lite](apps/web-lite.md), [Weather](apps/weather.md)
  and [Maps](apps/maps.md).
- **[Settings](apps/settings.md)** for Wi-Fi and mobile networks, the display, sound, date and
  time, battery and storage.
- A game, [Snake](apps/snake.md), and [more apps](external/index.md) that can be installed
  separately, such as [Telegram](external/telegram.md), [GitHub](external/github.md) and
  [Assistant](external/assistant.md).

To put it on your phone, see [Installing](install.md).

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
  computer is plugged in, see [Copying files with a computer](#copying-files-with-a-computer))
  and the battery. A tile is blue while on.
- The home screen, the menu and some apps turn sideways with the phone. **Rotation lock** in
  the quick settings keeps them the way the screen is when you turn it on, upright or
  sideways, however you hold the phone; tap it again to let them turn. The phone remembers
  it when it restarts.

![The quick settings](images/generated/quick-settings.webp){ width="300" }

## Copying files with a computer

Photos, music, videos, books, notes and games go between the phone and a computer over the USB cable, as
with an Android phone:

1. Plug the phone into the computer.
2. Swipe down from the top of the screen. The :fontawesome-brands-usb: USB line says
   **Connected to a computer**, with :fontawesome-solid-chevron-down: at its end: tap the line.
3. Tap **File transfer**. The :fontawesome-solid-check: check mark moves to it (the choices are
   dimmed for a moment while the phone switches), and the USB line says
   **Computer · File transfer**.
4. On the computer, open **PinePhone** where phones and drives show up. It has a folder for
   each app's files, the same ones [Files](apps/files.md) starts with (an app you install
   adds its own, as Genesis and Doom do):

| Folder | What it holds |
|---|---|
| **Photos** | Camera's photos and screenshots, what **Gallery** shows |
| **Music** | The songs **Music** plays |
| **Videos** | The videos **Videos** plays |
| **Books** | The books **Books** lists |
| **Notes** | Your notes in **Notes**, a Markdown file each (the first line is its title); a `.md` or `.txt` file copied in becomes a note |
| **Memos** | The voice memos **Recorder** made |
| **Genesis** | Once installed, the [Genesis](external/genesis.md) app's games (in **roms**), its Sega CD BIOS and saved games |
| **Doom WADs** | Once installed, the games [Doom](external/doom.md) plays |

The rest of the phone, such as messages and Wi-Fi passwords, is not shared. Unlock the phone
to turn file transfer on: it cannot be turned on from the lock screen. It turns off when you
unplug the cable or tap **Network only**, and after the phone restarts.

!!! warning
    Only checked with a Linux computer so far (GNOME's **Files**). Windows opens phones like this
    one in File Explorer; a Mac needs an app for it, such as OpenMTP.

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
- While you have any, the left of the status bar shows :fontawesome-solid-envelope: for unread
  messages and :fontawesome-solid-phone-slash: for missed calls (the operator's name instead
  while the quick settings are open), and the **Phone** and **Messages**
  icons on the home screen and in the menu show how many in a red badge.
- A message that arrives also slides down from the top of the screen for a few seconds, with
  who sent it and what they wrote, over the lock screen too: the message wakes the phone.
  Tap it to open the conversation (on the lock screen, this unlocks the phone). It does not
  when you are in that conversation already or the quick settings are open (they list it).

A notification goes once you have seen what it is about: the conversation opened in
**Messages**, or **Recents** opened in **Phone**. Notifications stay across restarts.

Apps you install can post notifications too, for example
[Telegram](external/telegram.md#notifications)'s messages. They are listed the same way, with
the app's icon, and the status bar shows :fontawesome-solid-bell: while there are any.

![Notifications on the lock screen](images/generated/lock-notifications.webp){ width="300" }

## Computers on USB

A computer plugged into the phone's USB port can get full control of it, to install updates
and apps or copy files, as Android's USB debugging does. The first time a computer asks, the
phone shows **Allow this computer?** with the computer's name and the fingerprint of its key,
the same one the computer prints. With the phone awake:

1. Check that the fingerprint matches the one on the computer.
2. Tap **Allow**, or **Deny** to refuse.

The phone remembers allowed computers, also when its software is updated, and does not ask
again. A question left unanswered for a minute counts as **Deny**. Only computers on the USB
cable can ask, not ones on a Wi-Fi network.

An allowed computer can also put text in the phone's clipboard, such as a Wi-Fi password or a
key too long to type: from the firmware's sources, `just clipboard` sends the computer's
clipboard, and the phone's keyboard then offers it under **Paste**.

!!! warning
    Allow only your own computers: an allowed computer can read and change everything on the
    phone. To take its access away, see
    [Allowed computers](apps/settings.md#allowed-computers).

## Hardware

The firmware runs on the original PinePhone (Allwinner A64). The PinePhone Pro and other
phones are not supported.

"Firmware" here means what it means on a feature phone: the whole system, flashed and updated
as one image. It replaces the phone's operating system (postmarketOS, Mobian...) rather than
adding to it: nothing in it installs under another system. The bootloader is
[Tow-Boot](https://tow-boot.org/), unchanged.
