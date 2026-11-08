# Mobile network

The phone's modem connects to your operator's network with the SIM card. With it you send
and receive text messages and, when you turn it on, use mobile data (2G to 4G). Calls do not
work yet: see [Phone](apps/phone.md).

## The SIM card

The modem starts a few seconds after the phone. Then it reads the SIM card and registers on
your operator's network, which takes a little longer. Until then the signal bars stay dim.

A SIM card protected by a PIN cannot be unlocked on this phone yet: the status bar shows
the :fontawesome-solid-lock: lock. To use the card, turn its PIN off in another phone first.

## The status bar

The right of the status bar, before the battery, shows USB, Bluetooth and Wi-Fi, then the mobile
network. :fontawesome-brands-bluetooth: Bluetooth shows while it is on: dim, and lit while a device
(headphones, a speaker) is connected. :fontawesome-brands-usb:{ style="transform: rotate(270deg)" } USB shows while a computer is connected: dim
for a moment while the two set up their connection, then lit. Whether the phone also uses the
computer's internet is said in the quick settings.

| You see | It means |
|---|---|
| The signal bars | How strong the mobile signal is. They stay dim until the phone is registered on a network. |
| **R** after the bars | You are roaming: registered on another operator's network, usually abroad. |
| **2G**, **3G** or **4G** | Mobile data is connected, over that technology, and Wi-Fi is not. While the quick settings are open, it shows in any case. |
| :fontawesome-solid-sim-card: | No SIM card. |
| :fontawesome-solid-lock: | The SIM card is locked by its PIN, or blocked by its PUK (in red). |
| :fontawesome-solid-xmark: | No service: no network in reach, or the network refused the SIM card. |
| :fontawesome-solid-triangle-exclamation: | The SIM card has a problem, or the modem is not found (in red). |

When all is well, only the bars show. The operator's name (or what is wrong, such as **No SIM**)
takes the left of the status bar while the quick settings are open (swipe down from the top of
the screen), and is in **Settings › Network**.

## Text messages

Messages sends and receives text messages (SMS) over the mobile network, whether mobile data
is on or not. See [Messages](apps/messages.md).

A message or a call that arrives while the phone is asleep wakes it up on the lock screen.
Left there, it goes back to sleep 10 seconds later (after the call, for a call), counted
from your last touch. Unlock it, or press ++power++ to wake it yourself, and it stays awake
until you press ++power++ again.

## Codes to your operator

Codes such as **\*100#**, dialed in Phone, ask your operator for your balance, your remaining
data or a menu of its offers. See [Codes to your operator](apps/phone.md#codes-to-your-operator).

## Mobile data

Mobile data is off by default. To use the internet over the mobile network, turn on the
:fontawesome-solid-tower-broadcast: **Mobile data** tile in the quick settings, or
**Settings › Network › Mobile data** (the same switch).

Once connected, the status bar shows the technology (**4G**, for example). When Wi-Fi is
connected too, the phone uses Wi-Fi, and the technology goes from the status bar.

## Roaming

![Roaming with mobile data on](images/generated/mobile-roaming.webp){ width="300" }

When you are on another operator's network, the status bar shows **R**, and the operator's
name (with the quick settings open, and in **Settings › Network**) has "(roaming)" after it.
Text messages work as at home.

Mobile data stops while you are roaming, unless **Settings › Network › Data roaming** is on
too. It is off by default: data on a foreign network can be expensive.

!!! warning "Not tested yet"
    Data roaming has not been tested on a foreign network yet.

## Access point name (APN)

Most operators give the phone the access point name (APN) it needs for mobile data, so the
field in **Settings › Network** is empty and shows **Automatic**. If your operator tells you
to set one, see [Settings](apps/settings.md#access-point-name-apn).

!!! warning "Not tested yet"
    Setting an APN has not been tested with a network that requires one.
