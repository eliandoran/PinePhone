# Settings

Settings shows what the phone is doing (battery, connections, storage, temperatures) and
holds the few things you can change: brightness and theme, touch vibration, the time zone,
the language, Wi-Fi networks, Bluetooth devices and mobile data.

Open **Settings** from the menu. Each section shows its current value beside its name, for
example the brightness, the battery's charge or the Wi-Fi network you are on. Tap a section to
open it, and the :fontawesome-solid-chevron-left: back arrow at the top left to return.

![The Settings list](../images/generated/settings-list.webp){ width="300" }

Changes take effect at once and are kept when the phone restarts; there is no Save button.

## Display

![Display settings](../images/generated/settings-display.webp){ width="300" }

To change the brightness, drag the **Screen brightness** slider. The screen follows your
finger, and the level is kept once you lift it. The slider stops at 5%, so the screen
can never be turned completely dark.

To switch between the dark and the light theme:

1. Open **Settings › Display**.
2. Under **Appearance**, tap **Dark** or **Light**.

The :fontawesome-solid-check: tick shows the theme in use; **Dark** is the default. Every
screen is redrawn in the new colors.

**Screen** lists the display's resolution, its density and how long animations take. These
are for information only.

## Sound & vibration

**Vibrate on touch** makes the phone give a short buzz when you tap a control or a key of
the keyboard. It is on by default; turned off, the Settings list shows **Silent** beside
**Sound & vibration**.

**Test vibration** buzzes once so you can check the vibration motor, even with
**Vibrate on touch** off.

!!! warning "Not available yet"
    Ringtones cannot be chosen yet: **Ringtone** shows **Not set up yet**.

## Date & time

The top of **Settings › Date & time** shows the time, the date and the time zone with its
offset from UTC.

To set your time zone:

1. Open **Settings › Date & time**.
2. Under **Time zone**, tap your city or region. A :fontawesome-solid-check: tick marks it.

The clock in the status bar changes right away. Summer time is followed automatically in
the zones that have it. The time zone is **UTC** until you choose one.

The list has 15 zones: Los Angeles, Denver, Chicago, New York, Sao Paulo, UTC, London,
Paris and Berlin, Bucharest and Helsinki, Moscow, Dubai, India, Shanghai, Tokyo and Sydney.
If you live elsewhere, pick the zone that has the same time as yours.

The time itself cannot be set here: the phone takes it from the internet.

## Language

The phone speaks English or Romanian. To change it:

1. Open :fontawesome-solid-language: **Settings › Language**.
2. Tap **English** or **Română**. A :fontawesome-solid-check: tick marks the one in use.

Every screen is redrawn in the new language at once; **English** is the default. The
Settings list shows the language beside **Language**.

!!! note "What stays as it is"
    What comes from elsewhere is in its own language: your network's name, the network's
    answers to codes, the web pages you open, and what people write on GitHub (and GitHub's own
    messages, such as why it refused a comment). **Assistant** answers in the language you
    ask in.

### Autocorrection

Below the languages, under **Keyboard**, the **Autocorrection** switch turns the keyboard's
corrections on or off (on by default). With it on, where you type prose (a note, a message)
the keyboard shows a strip of words above the keys and puts the likeliest in when you type a
space; see [Using the phone](../index.md#using-the-phone). It works in English and Romanian.

## Battery

![Battery settings](../images/generated/settings-battery.webp){ width="300" }

The top of **Settings › Battery** shows the charge and the battery's state, such as
**Discharging** or **Charging**. While charging, it adds how long until the battery is full
when the phone can estimate it, starting about 20 seconds after you plug in the charger.
Plugged in once full, it says **Full**. The phone then waits for the battery to drop a
little before charging again, so it can say **Not charging** at 99% for a while. The
battery icon shows the bolt whenever the phone is plugged in.

To show the charge as a number in the status bar, turn on **Show percentage in status
bar**. The number then shows inside the battery icon, which grows a little wider, and the
bolt moves just after it while the phone is plugged in. It is off by default.

**Last 24 hours** charts the charge over the past day, one bar per quarter of an hour:
green while charging, blue otherwise, red below 15%. The phone does not measure the charge
while it sleeps, so lighter bars bridge those times with a straight line from the charge
before to the charge after: they show how much it used while asleep, not when. The chart
only covers the time the phone was on; a gap means it was switched off. The **Screen**
strip below the bars marks when the screen was on.

**Battery use** shows where the charge went since you last unplugged the phone, as
Android does: each app and each part of the phone, the most first, with its share of the
charge used. Above the list, the phone says how much of the battery it used and over how
long, such as "26% used in 3 h 34 min". While the phone is plugged in, the same list shows
under **Last time on battery**, until you unplug it again.

- **Apps** count for the work they did, including while you were not looking at them
  (Telegram keeps receiving once opened), and for the data they moved over the mobile
  network or Wi-Fi. Below an app's name, how long it was on screen.
- :fontawesome-solid-sun: **Screen**: the display and its light, more at a higher
  brightness. Below it, how long the screen was on.
- :fontawesome-solid-clock: **Idle**: the phone awake with the screen off and nothing
  running, such as while it waits a few seconds before sleeping.
- :fontawesome-solid-moon: **Asleep**: the phone sleeping, and for how long.
- :fontawesome-solid-gear: **System**: the home screen, the lock screen and the phone's
  own services.
- :fontawesome-solid-tower-cell: **Mobile network** and :fontawesome-solid-wifi:
  **Wi-Fi**: data the phone moved while no app was busy, for its own connections.

The phone measures the current it draws while awake, and learns over time how much the
screen, the processor and the radios each take, so the shares get closer to the truth the
more you use it on battery. They remain an estimate: it cannot tell for certain which app
the data was for, and while asleep it only sees the charge drop.

**Details** lists:

- **Health** and **Technology**, as the battery reports them.
- **Voltage** and **Current**: the current is negative while the battery is discharging.
- **Charger**: **Not connected**, or what is plugged in (**Computer** or **Charger**), the
  kind of USB connection and how much current the phone draws from it.
- **Temperature** of the battery and, when the phone can read them, the temperatures
  between which it charges. The battery does not charge outside them.
- **Design capacity**: what the battery holds when new.

## Wi-Fi

![Wi-Fi settings, connected to Home](../images/generated/settings-wifi.webp){ width="300" }

**Settings › Wi-Fi** shows the switch, the connection and, below **Networks**, the networks in
range, the strongest first. Each shows whether it needs a password (**Secured**) or not
(**Open**) and how good its signal is (**Excellent**, **Good**, **Fair** or **Weak**). The
network you are on has a :fontawesome-solid-check: tick; networks the phone knows show
**Saved**. The list refreshes every few seconds.

To connect to a new network:

1. Open **Settings › Wi-Fi**.
2. Under **Networks**, tap the network's name.
3. If it is secured, type its password (at least 8 characters) and tap the
   :fontawesome-solid-check: tick at the top right, or the Enter key.

![Typing a network's password](../images/generated/settings-wifi-password.webp){ width="300" }

**Status** shows **Connecting to …**, then **Connected to …**. The phone saves the network
and connects to it by itself from then on. An open network connects as soon as you tap it.

If the password is wrong, **Status** shows **Wrong password for …** and the network is not
saved: tap it again to retry.

To switch to another saved network, tap it in the list.

While connected, the card shows the connection's details: **IP address**, **Router**,
**Frequency** (2.4, 5 or 6 GHz and the channel), **Signal**, **Link speed** and
**Security**.

To make the phone forget the network you are on (it will not connect to it again until you
enter its password again), tap **Forget this network**.

To turn Wi-Fi off, turn off the **Wi-Fi** switch at the top: **Status** shows **Off** and the
list is hidden. Turn it on again and the phone reconnects to a saved network. The Wi-Fi tile
of the quick settings does the same, and the phone remembers the choice when it restarts.

!!! warning "Limitations"
    - Networks that hide their name are not listed and cannot be added.
    - Enterprise networks (a user name and password, as at many offices and universities)
      and old WEP networks show **Not supported** and cannot be joined.

## Bluetooth

![Bluetooth settings](../images/generated/settings-bluetooth.webp){ width="300" }

Bluetooth is off by default. To turn it on, open **Settings › Bluetooth** and tap the
**Bluetooth** switch. The phone remembers it when it restarts.

While this page is open, the phone looks for devices around it and other devices can find
it: **Status** says **Visible as PinePhone**. Once you leave the page, it stops looking and
is hidden again, but stays on and connected to its devices.

- **Paired devices**: the devices the phone knows, each with **Connected** or **Not
  connected**. Tap one to connect to it or to disconnect it.
- **Devices nearby**: the devices found around the phone, strongest signal first, with how
  strong it is (**Excellent**, **Good**, **Fair**, **Weak**). Devices that do not give a
  name are left out. Until one is found, it says **Searching...**.

To pair with a device:

1. Put the device in pairing mode (see its manual; for headphones, often holding its power
   button).
2. Open **Settings › Bluetooth**, with Bluetooth on.
3. Tap the device under **Devices nearby**. It shows **Pairing...**.
4. If the device shows a number, the phone shows **Pair with** and the device's name with
   the same number. Check that both show the same number, then tap **Pair**. Tap **Cancel**
   if they differ.

![Confirming the code](../images/generated/settings-bluetooth-pair.webp){ width="300" }

Once paired, the device moves to **Paired devices** and the phone connects to it. If
pairing fails or is cancelled, **Status** says **Could not pair with** and the device's
name. A device that pairs from its own side (a computer, another phone) asks the same
question on the phone while this page is open.

The quick settings have a **Bluetooth** tile too: tap it to turn Bluetooth on or off, hold it
to open this page.

In the Settings list, **Bluetooth** shows **Off**, **On**, or the name of the device it is
connected to. While Bluetooth is on, the status bar shows :fontawesome-brands-bluetooth-b: the
Bluetooth symbol, dim, and lit while a device is connected.

With a speaker or headphones connected, **Music**, **Recorder** and the games (**Doom**,
**Genesis**) play on them. When they disconnect, the sound moves back to the phone's speaker
and the song or game goes on from where it was; it moves to them the same way when they
connect.

While they play, the volume keys and the volume slider of the quick settings change their
volume (the keys' slider then shows :fontawesome-brands-bluetooth: the Bluetooth symbol), and
follow the speaker's own volume buttons. The phone keeps its own level for when they
disconnect. Their play / pause, next and previous buttons control **Music**, from any app.

!!! warning "Limitations"
    - Alarms and the other sounds of the phone stay on its own speaker. Calls play on a
      headset (see [During a call](phone.md#during-a-call)), but its buttons or gestures for
      calls and the voice assistant do nothing yet: answer and hang up on the phone.
    - While **Settings › Bluetooth** is open and looking for devices, Wi-Fi slows down and
      can drop. Leave the page once your device is paired.
    - Devices that need a code typed on the phone (some keyboards) cannot be paired yet.
    - A paired device cannot be removed from the phone yet.

## Network

![Network settings](../images/generated/settings-network.webp){ width="300" }

**Settings › Network** gathers the phone's connections:

- **Connections**: each connection that has an address (Wi-Fi, mobile data, USB to a
  computer) with that address. Without any, **USB** says **Not connected**, or **Charger
  only** when a charger is plugged in.
- **Wireless**: the Wi-Fi network you are on and **Bluetooth** (**Off**, **On** or the device
  it is connected to); tap either to open its settings.
- **Mobile**: the mobile **Network** you are registered on (with "(roaming)" abroad, or
  **No SIM card**, **No service** and the like), the switches for mobile data, and the
  SIM's **Phone number** and the modem's **IMEI**. The phone number shows **Unknown** when
  the SIM does not hold it.

In the Settings list, **Network** shows how the phone is online: **Wi-Fi**, **USB** or
**Offline**.

### Mobile data

Mobile data is off by default. To use the internet over the mobile network:

1. Open **Settings › Network**.
2. Under **Mobile**, turn on **Mobile data**.

The **Mobile data** tile in the quick settings is the same switch. When Wi-Fi is connected,
the phone uses Wi-Fi rather than mobile data.

### Data roaming

**Data roaming** is off by default too: data on a foreign network can be expensive. With it
off, mobile data stops while you are roaming, even with **Mobile data** on. To use mobile
data abroad, turn on both **Mobile data** and **Data roaming**. While you are roaming, the
status bar shows **R** after the signal bars.

!!! warning "Not tested yet"
    Data roaming has not been tested on a foreign network yet.

### Access point name (APN)

Most networks tell the phone which APN to use, so the field is empty and shows
**Automatic**. If your operator gave you an APN (for example for a prepaid plan):

1. Open **Settings › Network**.
2. Tap the field under **Access point name (APN)**.
3. Type the APN and press Enter.

The APN is saved when you press Enter, hide the keyboard or leave the page, and the modem
reconnects with it. It may contain letters, digits, dots and dashes only. To go back to the
network's own, clear the field.

!!! warning "Not tested yet"
    Setting an APN has not been tested with a network that requires one.

More about the status bar's signs, roaming and text messages: [Mobile network](../mobile.md).

## Storage

**Settings › Storage** shows how full the phone's two storage areas are:

- **System**: the phone's software. It is read-only and replaced by each update.
- **Data**: your contacts, messages, notes, photos, music, recordings, settings and
  installed apps. It is kept across updates.

Each shows the space used out of the total, a bar and how much is free. The Settings list
shows the free space on **Data**.

## About phone

**Settings › About phone** identifies the phone and shows how it is running:

- The model, and under **Mobile** the **Phone number** and **IMEI** (when the modem is on).
- **Software**: the Linux kernel's version, the LVGL graphics library's version, the
  **Boot slot** (the phone keeps two copies of its software, **A** and **B**, and updates
  the one not in use) and the **Uptime** since it was switched on.
- **Hardware**: the chip, the processor and its current speed, the graphics processor and
  the free memory.
- **Temperature** of the processor, the graphics processor, the power chip and the
  battery. The processor's turns orange when it nears the point where the phone slows down
  to cool, red past it.

## Developer mode

**Settings › Developer mode** (:fontawesome-solid-code:) helps find out why the phone or its
connection feels slow, or why the screen turns the wrong way, and controls which computers
can reach the phone over USB.

### Frame rate

To see how smoothly the screen draws, turn on **Show frame rate** under **Performance**.
A counter appears in the bottom right corner, over every screen and app: the frames drawn
per second (up to 60 when scrolling or animating; it drops when nothing changes, which is
normal), how busy the processor is, and its current speed, for example
**60 FPS, 35% CPU, 1152 MHz**. It is off by default, and stays as you set it after a restart.

### Allowed computers

Under **Computers on USB**, **Allowed computers** shows how many computers you allowed to
control the phone over USB (see [Computers on USB](../index.md#computers-on-usb)). Tap it for
the list: each computer's name and the fingerprint of its key.

To take a computer's access away:

1. Tap the computer.
2. Tap **Revoke access**, or **Cancel** to keep it.

It can no longer connect: the next time it asks, the phone shows **Allow this computer?**
again. A connection it already has open goes on until it ends.

### Speed test

To see how fast the internet connection is (Wi-Fi, mobile data, or the computer's over USB,
whichever the phone uses), tap **Speed test** under **Network**, then **Start speed test**.
The phone downloads from Cloudflare's speed test for up to 10 seconds, showing **Download**
as it goes, then uploads for about 4 seconds and shows **Upload**, in Mbit/s (kbit/s when
slower than 1 Mbit/s). Without a connection, **Download** says **No connection**. Going back
stops the test.

!!! note "On mobile data"
    A test downloads up to 25 MB and uploads up to 5 MB, which counts toward your data
    allowance.

### Screen rotation

To troubleshoot the automatic screen rotation, tap **Accelerometer** under **Sensors**. It
shows the motion sensor's readings as they come, which edge of the phone points up, and
which way the screen turns. Tilt the phone and watch **Up** and **Turns to** change. Without
a working sensor it shows **No accelerometer**.
