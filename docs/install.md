# Installing

The firmware goes on the phone's internal storage (eMMC) from a computer, over the USB cable. No
SD card is needed. You need a computer running Linux with [Nix](https://nixos.org/download/),
which provides the flashing tool, and the original PinePhone.

## What you need first

The phone must start from its own bootloader, [Tow-Boot](https://tow-boot.org/devices/pine64-pinephoneA64.html),
installed in the eMMC's boot partition. The firmware image does not include one. Flashing the
firmware never touches Tow-Boot, and Tow-Boot is what lets you flash the phone again whatever
is installed. To install it, follow Tow-Boot's instructions for the PinePhone (an SD card you
start the phone from once).

!!! warning
    Installing writes over the eMMC. What another system kept there (its files, photos and
    messages) is lost. Back it up first.

## Downloading the firmware

Each version is on the [releases page](https://github.com/eliandoran/PinePhone/releases), as
four files:

`pinephone-VERSION.img.xz`
:   The whole firmware, for installing it the first time.

`pinephone-VERSION-rootfs.ext4.xz`
:   The same firmware without the partition table, for updating a phone that already runs it.

`pinephone-VERSION.img.bmap`
:   Which parts of the image hold data, for `bmaptool` (see **Without Nix** below).

`SHA256SUMS`
:   Their checksums: `sha256sum -c SHA256SUMS` checks that a download arrived whole.

There is no need to unpack them: the flashing tool does it.

## Installing it the first time

1. Plug the phone into the computer with the USB-C cable, the phone switched off.
2. On the computer, in the folder you downloaded to, run:

    ```
    nix run github:eliandoran/PinePhone -- --usb pinephone-VERSION.img.xz
    ```

3. Hold ++volume-up++ and press ++power++. The phone's LED turns blue: Tow-Boot shows its storage
   to the computer as a USB disk.
4. The tool finds the disk, shows it and asks before writing: type `yes`. Writing asks for
   your password (`sudo`).
5. When it says **Done**, hold ++power++ until the blue LED goes off and release it right
   away, then press ++power++ briefly. The phone starts the firmware.

The first start takes a little longer: the phone makes room for your data on the rest of the
eMMC.

??? tip "Without Nix"
    Any tool that writes a disk image works, `dd` for instance, with the phone showing its
    storage as in step 3 (`/dev/sdX` is the phone's disk, as `lsblk` lists it):

    ```
    xz -dc pinephone-VERSION.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
    ```

    or, faster, with [bmaptool](https://github.com/yoctoproject/bmaptool) and the `.bmap` file
    beside the image:

    ```
    sudo bmaptool copy pinephone-VERSION.img.xz /dev/sdX
    ```

    Writing the whole image also clears the start of the disk, where some other systems keep
    a bootloader of their own; Tow-Boot is not there and stays. The Nix tool leaves that area
    alone.

## Updating

A phone running the firmware updates over the USB cable without the button combination, while
it keeps running:

1. Plug the phone in, switched on.
2. Run, with the newer version's file:

    ```
    nix run github:eliandoran/PinePhone -- pinephone-VERSION-rootfs.ext4.xz
    ```

3. The first time, the phone asks **Allow this computer?**: tap **Allow**. It remembers the
   computer after that.

The phone installs the new version beside the one it runs and restarts into it. If the new
version does not reach its home screen within a minute, the phone goes back to the previous
one by itself. Your data, settings and [installed apps](external/index.md) stay.

If the phone no longer starts at all, install it again as the first time: the USB disk of
step 3 works whatever is on the eMMC.

## Builds between versions

Every change to the firmware is also built on its own, before it becomes a version: in the
repository's [Image workflow](https://github.com/eliandoran/PinePhone/actions/workflows/image.yml),
each run keeps the same files for 30 days (downloading them needs a GitHub account). They are
tried less than the versions on the releases page.
