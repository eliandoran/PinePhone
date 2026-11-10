# Installing

There are two ways to run the firmware:

- **[From a microSD card](#trying-it-from-a-microsd-card)**, to try it: the phone's own system
  stays as it is, and taking the card out brings it back.
- **[On the phone's storage](#installing-it-on-the-phone)** (the eMMC), faster, without a card.
  The firmware installs itself there from the card, with what you did while trying it.

Both start with a computer (Windows, macOS or Linux) and
[balenaEtcher](https://etcher.balena.io/), free, which writes disk images to cards and disks.
They need the original PinePhone (not the PinePhone Pro), charged or plugged into a charger.

Each version of the firmware is on the
[releases page](https://github.com/eliandoran/PinePhone/releases), as these files:

`pinephone-VERSION-sd.img.xz`
:   The firmware for a microSD card.

`pinephone-VERSION.img.xz`
:   The firmware for the phone's storage.

`pinephone-VERSION-rootfs.ext4.xz`
:   The same firmware without the partition table, for updating over the cable (see
    [Updating](#updating)).

`.bmap` files and `SHA256SUMS`
:   For writing with `bmaptool`, and the files' checksums.

Etcher unpacks the `.xz` files as it writes: no need to unpack them.

## Trying it from a microSD card

You need a microSD card of 8 GB or more. What it holds is erased; what the phone holds is not
touched.

1. Download `pinephone-VERSION-sd.img.xz`.
2. Put the card in the computer. In Etcher, choose **Flash from file** and pick the file, then
   **Select target** and pick the card, then **Flash!**.
3. Switch the phone off, take off its back cover and its battery, and put the card in its slot.
   Put the battery and the cover back.
4. Press ++power++. The phone starts the firmware from the card, whatever its own storage
   holds.

The first start takes a little longer: the firmware makes room for your data on the rest of
the card. Your data stays on the card, and the phone's own storage is left alone.

!!! note "A card is slower"
    From a card, the phone starts a little slower and large apps take longer to open the first
    time. For the full speed, [install it on the phone's storage](#installing-it-on-the-phone).

To go back to the phone's own system, switch it off and take the card out. To install a newer
version on the card, write its `-sd.img.xz` the same way: your data on the card stays.

## Installing it on the phone

The firmware running from a microSD card installs itself on the phone's storage (eMMC), with
your data from the card: contacts, messages, photos, settings and installed apps. It also
installs Tow-Boot, the program that starts the phone, when the phone has none. After writing
the card, no computer is needed.

!!! warning
    Installing writes over the eMMC. What another system kept there (its files, photos and
    messages) is lost. Back it up first.

1. Start the firmware from a card, as in
   [Trying it from a microSD card](#trying-it-from-a-microsd-card).
2. Open **Settings › Storage** and tap **Install on the phone**. The button is only there
   while the phone runs from a card.
3. Read what is erased, then tap **Erase and install**.
4. The screen shows each step as it goes: **Bootloader**, **System** and **Your data**. Keep the
   card in and the phone on until it is done: meanwhile ++power++ does not put the phone to
   sleep. The more data on the card, the longer the last step takes.
5. When it says **Installed**, tap **Restart**.

The phone restarts from its own storage. The card can stay in: once the phone's storage holds
the firmware, the phone starts that first. To use the card for something else, erase it in
**Settings › Storage** once restarted: its **microSD card** section offers **Erase the card**
while the card in the phone holds a copy of the system.

If the phone already had a bootloader (Tow-Boot, installed earlier), **Bootloader** says
**Kept** and it is left as it is.

If installing fails, the screen says why and **Try again** starts it over. The card is not
changed, and the phone's storage is left without a system to start, so the phone still starts
from the card.

### From a computer

Without a card for the firmware, the phone's storage can also be written from the computer,
in two steps:

1. **Tow-Boot**, the program that starts the phone, from a microSD card. Once only.
2. **The firmware**, over the USB cable.

You need, besides the phone and Etcher:

- Its USB-C cable, plugged into the computer.
- A microSD card of any size, for step 1. What it holds is erased.

#### Step 1: Tow-Boot

Tow-Boot starts the phone and lets a computer write to its storage over USB. It lives in a part
of the eMMC of its own, which installing or updating the firmware never touches. A phone that
already has Tow-Boot can skip to [step 2](#step-2-the-firmware).

1. Download Tow-Boot for the PinePhone,
   [`pine64-pinephoneA64-2023.07-007.tar.xz`](https://github.com/Tow-Boot/Tow-Boot/releases/download/release-2023.07-007/pine64-pinephoneA64-2023.07-007.tar.xz).
2. Unpack it: on Windows with [7-Zip](https://www.7-zip.org/) (open the `.tar.xz`, then the
   `.tar` inside it), on macOS by double-clicking it, on Linux with `tar -xf`. You need one
   file from it, `mmcboot.installer.img`.
3. Put the microSD card in the computer. In Etcher, choose **Flash from file** and pick
   `mmcboot.installer.img`, then **Select target** and pick the card, then **Flash!**.
4. Switch the phone off, take off its back cover and its battery, and put the card in its slot.
   Put the battery and the cover back.
5. Press ++power++. The phone vibrates and its LED turns red, then yellow: it starts from the
   card. The screen then shows Tow-Boot's installer.
6. Move to **Flash firmware to eMMC Boot** with ++volume-up++ and ++volume-down++, and choose it
   with ++power++. The installer writes Tow-Boot and says **Done!**.
7. Choose **Reboot**, then switch the phone off (hold ++power++), take the battery out and the
   card out, and put the battery back.

Without the card, the phone now starts Tow-Boot by itself: it vibrates and its LED turns red,
then yellow. Until the firmware is installed it has nothing more to start: the screen stays dark.

??? tip "Tow-Boot's own instructions"
    Tow-Boot's page for the PinePhone,
    [tow-boot.org/devices/pine64-pinephoneA64](https://tow-boot.org/devices/pine64-pinephoneA64.html),
    has the same steps and how to remove it again (**Erase firmware from eMMC Boot**).

#### Step 2: the firmware

You need `pinephone-VERSION.img.xz`, not the `-sd` one: the card's has partition IDs of its own,
and a phone with both would not know which system to start. Take the microSD card out first.

1. With the phone switched off and plugged into the computer, hold ++volume-up++ and press
   ++power++. Keep holding ++volume-up++ until the LED turns blue: the phone shows its storage
   to the computer as a USB disk.
2. In Etcher, choose **Flash from file** and pick `pinephone-VERSION.img.xz`, then
   **Select target** and pick the phone: a disk of about 31 GB.
3. Choose **Flash!**. Writing and checking take a minute or two. Windows may offer to format
   the phone's disk, which it cannot read: choose **Cancel**.
4. When Etcher says it is done, hold ++power++ until the blue LED goes off and release it
   right away, then press ++power++ briefly. The phone starts the firmware.

The first start takes a little longer: the phone makes room for your data on the rest of the
eMMC.

??? tip "On Linux, from the command line"
    Instead of Etcher, `dd` works, with the phone showing its storage as in step 1
    (`/dev/sdX` is the phone's disk, as `lsblk` lists it):

    ```
    xz -dc pinephone-VERSION.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
    ```

    or, faster, [bmaptool](https://github.com/yoctoproject/bmaptool) with the `.bmap` file
    beside the image:

    ```
    sudo bmaptool copy pinephone-VERSION.img.xz /dev/sdX
    ```

    With [Nix](https://nixos.org/download/), the flashing tool finds the phone's disk itself
    and asks before writing:

    ```
    nix run github:eliandoran/PinePhone -- --usb pinephone-VERSION.img.xz
    ```

    Writing the whole image, as Etcher and `dd` do, also clears the start of the disk, where
    some other systems keep a bootloader of their own; Tow-Boot is not there and stays. The
    Nix tool leaves that area alone.

`SHA256SUMS`, beside the files, has their checksums: `sha256sum -c SHA256SUMS` (Linux) checks
that a download arrived whole. Etcher checks what it wrote against the file.

## Updating

To install a newer version on the phone's storage, write its `pinephone-VERSION.img.xz` as in
[step 2](#step-2-the-firmware). Your data, settings and [installed apps](external/index.md) stay:
they are on the part of the eMMC the image does not cover.

??? tip "Over the cable, without restarting into Tow-Boot"
    With [Nix](https://nixos.org/download/) on a Linux computer, a phone running the firmware
    updates while it keeps running, from the release's `pinephone-VERSION-rootfs.ext4.xz`:

    1. Plug the phone in, switched on.
    2. Run:

        ```
        nix run github:eliandoran/PinePhone -- pinephone-VERSION-rootfs.ext4.xz
        ```

    3. The first time, the phone asks **Allow this computer?**: tap **Allow**. It remembers the
       computer after that.

    The phone installs the new version beside the one it runs and restarts into it. If the new
    version does not reach its home screen within a minute, the phone goes back to the previous
    one by itself.

If the phone no longer starts at all, install the firmware again as in step 2: Tow-Boot's USB
disk works whatever is on the eMMC.

## Builds between versions

The firmware is also built every night, when it changed that day, before the changes become a
version: the [nightly build](https://github.com/eliandoran/PinePhone/releases/tag/nightly) on
the releases page has the same files, replaced by each new build, and lists what changed since
the last version. It is tried less than the versions: if it misbehaves, install the latest
version again.
