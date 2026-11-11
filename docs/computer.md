# Connecting to a computer

Plugged into a computer with its USB-C cable, the phone charges, and the two are connected
as a small network: you can copy files both ways, and a computer you allow can install
updates and apps.

## The USB connection

Swipe down from the top of the screen: the :fontawesome-brands-usb: USB line of the quick
settings says what the cable is doing.

| It says | It means |
|---|---|
| **Not connected** | Nothing is plugged in. |
| **Charger only** | A charger, or a computer that has not set up the connection yet. |
| **Connected to a computer** | A computer is connected. |
| **Computer · Network and internet** | Connected, and the computer shares its internet: the phone uses it when it has neither Wi-Fi nor mobile data. |
| **Computer · File transfer** | Connected, with file transfer on (see below). |

While a computer is connected, the status bar shows
:fontawesome-brands-usb:{ style="transform: rotate(270deg)" } USB: dim for a moment while the
two set up their connection, then lit. **Settings › Network** shows the connection's address
under **Connections**.

The phone uses the computer's internet only when the computer shares it: from the firmware's
sources, `just share-internet` does, until the computer restarts.

## Copying files

Photos, music, books, notes and games go between the phone and a computer over the USB cable, as
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

## Allowing a computer

A computer plugged into the phone's USB port can get full control of it, to install updates
and apps or copy files, as Android's USB debugging does. The first time a computer asks, the
phone shows **Allow this computer?** with the computer's name and the fingerprint of its key,
the same one the computer prints. With the phone awake:

1. Check that the fingerprint matches the one on the computer.
2. Tap **Allow**, or **Deny** to refuse.

The phone remembers allowed computers, also when its software is updated, and does not ask
again. A question left unanswered for a minute counts as **Deny**. Only computers on the USB
cable can ask, not ones on a Wi-Fi network.

!!! warning
    Allow only your own computers: an allowed computer can read and change everything on the
    phone. To take its access away, see
    [Allowed computers](apps/settings.md#allowed-computers).

## Pasting text from the computer

An allowed computer can put text in the phone's clipboard, such as a Wi-Fi password or a key
too long to type: from the firmware's sources, `just clipboard` sends the computer's
clipboard, and the phone's keyboard then offers it under :fontawesome-solid-paste: **Paste**.
