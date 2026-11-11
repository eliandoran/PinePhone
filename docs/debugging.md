# Logs and debugging

When something does not work, such as no network or no SIM card, start with the phone's
hardware. If it is not that, a computer can open a shell on the phone and read what the phone
and its modem report. This page is for that: you need a computer and some ease with a
terminal.

## No modem, no SIM card

First look at what the phone says: swipe down from the top of the screen, or open
**Settings › Network**. The [status bar](mobile.md#the-status-bar) shows the same thing as an
icon.

| It says | Check |
|---|---|
| **No modem** | The modem's switch (below). Then switch the phone off and on, and wait half a minute: the modem takes a few seconds to start. |
| **No SIM card** | The SIM card: in its slot, the right way up, pushed in fully. With an adapter (a nano-SIM in a larger frame), check that the card has not slipped. |
| **SIM locked (PIN)** | The card has a PIN, which this phone cannot unlock yet: turn the PIN off in another phone. |
| **SIM blocked (PUK)** | The PIN was entered wrong too often: unblock the card in another phone, with the PUK from your operator. |
| **SIM error** | Try another SIM card, to tell a faulty card from a bad contact. |
| **Searching** or **No service** | The modem and the card work: the phone looks for a network. If it never finds one, try where another phone on the same operator has signal. |

Switch the phone off and take the battery out before you touch the SIM card.

### The hardware switches

Under the back cover, the PinePhone has six small switches that cut parts of the phone off,
whatever the software does. Taking the cover off to put a SIM card in, it is easy to move one.

| Switch | When off |
|---|---|
| 1 | The modem has no power: no calls, messages or mobile data, and the phone says **No modem**. |
| 2 | No Wi-Fi or Bluetooth. |
| 3 | No microphone. |
| 4 | No rear camera. |
| 5 | No front camera. |
| 6 | The headphone jack is a serial port instead (see [A serial cable](#a-serial-cable-in-the-headphone-jack)). |

For normal use, all six are on. Switch the phone off before moving them.

## A shell on the phone

On a console the phone asks for a login: type `root` and press Enter. There is no password.

### A USB cable

The simplest way. Plugged into a computer with a USB-C cable, the phone shows up as a serial
port as well as a network:

=== "Linux"
    The port is `/dev/ttyACM0`:

    ```
    picocom /dev/ttyACM0
    ```

    Any terminal program does: `screen /dev/ttyACM0`, `tio /dev/ttyACM0`. Without permission
    on the port, add yourself to the `dialout` group (`uucp` on Arch Linux) or use `sudo`.

=== "macOS"
    ```
    screen /dev/cu.usbmodem*
    ```

=== "Windows"
    Find its port (COM3, COM4...) in Device Manager under **Ports (COM & LPT)**, then open it
    in PuTTY, as **Serial**.

Press Enter if nothing shows: the phone had asked for the login before you connected. In
picocom, ++ctrl+a++ then ++ctrl+x++ quits.

The port exists once the phone has started. If the phone does not start that far, use the
serial cable.

### A serial cable in the headphone jack

The headphone jack doubles as a serial port, the phone's console from the moment it is
switched on: you see the phone start, then the login. You need the PinePhone serial cable (or a
3.3 V USB serial adapter wired to a jack plug: not 5 V), and switch 6 off.

1. Plug the adapter into the computer and find its port:

    === "Linux"
        `ls -l /dev/serial/by-id/` lists each adapter by name, pointing at its port
        (`ttyUSB0`). Or run `sudo dmesg -w` and plug it in: a line such as
        `ch341-uart converter now attached to ttyUSB0` names it.

    === "macOS"
        `ls /dev/cu.*`, before and after plugging it in.

    === "Windows"
        Device Manager, under **Ports (COM & LPT)**.

2. Open it at 115200 baud:

    ```
    picocom -b 115200 /dev/ttyUSB0
    ```

    picocom says **Terminal ready**: that is the computer's side, not the phone's yet.

3. Plug the cable into the phone's jack, fully: the jack is stiff.
4. Switch the phone on. Within a few seconds text scrolls (the bootloader, then Linux), then
   the phone asks for the login.

| You see | Check |
|---|---|
| Nothing at all, not even while the phone starts | Switch 6 off, the plug fully in, the right port. |
| Garbled characters | The speed: 115200. |
| The phone starting, but typing does nothing | The cable's wiring (its transmit line), or the plug. |

The phone also prints its own messages on this console, in the middle of what you type. To
quiet them until the phone restarts, type `dmesg -n 1`.

### SSH

SSH needs the computer [allowed on the phone](computer.md#allowing-a-computer) once, over the USB
cable. From a Linux or macOS computer:

1. Make a key, if you have none:

    ```
    ssh-keygen -t ed25519
    ```

2. With the phone plugged in and awake, send the key to it:

    ```
    bash -c 'exec 3<>/dev/tcp/172.16.42.1/7022; head -n 1 ~/.ssh/id_ed25519.pub >&3; cat <&3'
    ```

3. The phone asks **Allow this computer?** with the key's fingerprint: check it against
   `ssh-keygen -lf ~/.ssh/id_ed25519.pub`, then tap **Allow**. The command prints `allowed`.
4. Log in:

    ```
    ssh root@172.16.42.1
    ```

Once allowed, SSH also works over Wi-Fi, at the phone's address on it (in
**Settings › Network**). The phone keeps allowed computers when it is updated. On Windows,
the USB serial port is simpler.

## What to look at

The root filesystem is read-only: nothing can be installed on it. `/var/lib` keeps what you
write there (also across updates), `/tmp` until the phone restarts.

| Command | Shows |
|---|---|
| `tail -f /tmp/phone-ui.log` | The phone's interface's log, live (++ctrl+c++ stops). |
| `grep modem /tmp/phone-ui.log` | What the phone asked the modem, and what it made of the answers. |
| `dmesg` | Linux's messages: devices found, errors. |
| `lsusb` | The modem, when it is on: `2c7c:0125 Quectel`. Missing: it has no power (switch 1) or did not start. |
| `ls /dev/ttyUSB*` | The modem's ports. |

`/tmp/phone-ui.log` starts again when the phone restarts: copy it before (`cp /tmp/phone-ui.log
/var/lib/`), or send its end with your report.

### Asking the modem

The modem answers AT commands on its second port, `/dev/ttyUSB3`, while the phone uses the
first:

```
microcom -s 115200 /dev/ttyUSB3
```

Type a command and press Enter; ++ctrl+x++ quits.

| Command | Answers |
|---|---|
| `AT+CPIN?` | The SIM card: `+CPIN: READY`, `+CPIN: SIM PIN` (locked), or `+CME ERROR: 10` (none found). |
| `AT+QSIMSTAT?` | Whether the modem detects a card in the slot: `+QSIMSTAT: 0,1` yes, `0,0` no. |
| `AT+COPS?` | The network it is registered on, if any. |
| `AT+CSQ` | The signal: the first number from 0 (none) to 31, 99 unknown. |

!!! danger "Leave the phone's port alone"
    Do not open `/dev/ttyUSB2`: the phone talks to the modem there, and what you type would
    mix with its commands. Ask, do not change settings: some stay in the modem across
    restarts and updates, and can stop calls or messages from reaching the phone.

## Reporting a problem

Open an issue on [GitHub](https://github.com/eliandoran/PinePhone/issues) with:

- What you did and what the phone showed, in its words, or a screenshot (++power+volume-down++).
- The version you installed (the file's name), and whether the phone runs it from a microSD
  card: **Settings › About phone**, **Started from**.
- For the modem or the SIM card: the output of `grep modem /tmp/phone-ui.log`, `lsusb` and
  `AT+CPIN?`.
