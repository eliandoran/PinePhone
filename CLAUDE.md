# PinePhone feature phone firmware

From-scratch firmware for the original PinePhone (Allwinner A64, 3 GB / 32 GB eMMC):
Buildroot + mainline Linux, with a barebones LVGL feature phone UI planned on top
(DRM/KMS + evdev, modem driven with AT commands, ALSA only, BusyBox init).

The host runs NixOS. All tooling comes from the flake; Buildroot runs inside an FHS
environment (`nix run .#fhs`) because it expects a conventional Linux host.

## Commands

Run `nix develop` first (provides `just`, sunxi-tools, ssh, tio).

```
just build              # build the firmware into output/images/
just flash              # over the air if the phone runs our firmware, otherwise via USB
just deploy             # build, then flash
just flash-usb          # force the full-image USB path (Volume Up at power-on, or FEL)
just expose             # only expose the eMMC as a USB disk, write nothing
just ssh [cmd]          # root shell on the phone (172.16.42.1) using keys/pinephone
just console            # serial console over the USB gadget (/dev/ttyACM0), auto-reconnects
just logs               # follow the kernel log over SSH from boot start, across reboots
just shell              # interactive shell in the Buildroot FHS environment
just menuconfig         # then `just savedefconfig` to keep the changes
just linux-menuconfig   # then copy the options into board/pinephone/linux.fragment
just make <target>      # any Buildroot target, e.g. just make linux-rebuild
```

### Rebuilding after changes

Buildroot only rebuilds what is not built yet; it does not track changes. `just build`
always regenerates the rootfs and image (overlay, post-build, genimage) cheaply.

| Changed | Run |
|---|---|
| rootfs overlay, post-build.sh, extlinux.conf, genimage.cfg | `just build` |
| `linux.fragment` or `linux.defconfig` | `just make linux-reconfigure all` |
| kernel source | `just make linux-rebuild all` |
| `busybox.fragment` | `just make busybox-reconfigure all` |
| new package in the defconfig | `just build` |
| option of an already built package | `just make <pkg>-reconfigure all` |
| patch in `external/patches/<pkg>/` | `just make <pkg>-dirclean` then `just build` (patches apply only on extract) |
| toolchain/arch, or a package removed | `just make clean` then `just build` |

`<pkg>-rebuild`/`<pkg>-reconfigure` only rebuild that package; the trailing `all` regenerates
the rootfs and `pinephone.img`. Without it the image is stale (check timestamps in `output/images/`).

Editing `pinephone_defconfig` makes the Makefile regenerate `output/.config`, dropping
unsaved menuconfig changes.

## Layout

- `flake.nix`: flasher (`nix run .`), FEL U-Boot, FHS build env (`.#fhs`), dev shell.
- `Makefile`: downloads Buildroot 2026.08.1 into `buildroot/`, builds with
  `BR2_EXTERNAL=external` into `output/`, downloads go to `dl/`. Generates `keys/pinephone`.
- `external/configs/pinephone_defconfig`: Bootlin glibc toolchain, latest kernel (7.1.x) with
  `board/pinephone/linux.defconfig` + `board/pinephone/linux.fragment`, dropbear, mdev.
- `external/board/pinephone/`: genimage layout, kernel/busybox fragments, post-build
  (installs the dev SSH key), rootfs overlay (inittab, USB gadget, udhcpd, extlinux, pp-update).
- `scripts/pinephone-flash.sh`: the flasher, wrapped by the flake (shellcheck runs at build).
- Ignored: `buildroot/`, `output/`, `dl/`, `keys/`, `result*`.

## Boot and flashing design

```
eMMC boot0:  Tow-Boot (bootloader; not visible over USB mass storage)
eMMC user:   sector 0 MBR (disk signature 0x50480001)
             4 MiB    slot A  p1 512M  PARTUUID 50480001-01
                      slot B  p2 512M  PARTUUID 50480001-02
                      free (reserved for a data partition)
```

- Tow-Boot boots `/boot/extlinux/extlinux.conf` from the partition with the MBR
  **bootable flag**; U-Boot only scans bootable partitions when any are flagged.
  It picks `allwinner/sun50i-a64-pinephone-1.x.dtb` from `/boot` for the board revision.
- **Over the air** (`just flash` with the phone up): `rootfs.ext4` is streamed over SSH to
  `pp-update`, which writes the inactive slot, verifies SHA-256, rewrites that slot's
  `root=PARTUUID=…`, moves the bootable flag (one sector write) and reboots.
- **USB fallback**: hold Volume Up while powering on → Tow-Boot USB mass storage
  (blue LED, shows up as `1f3a:1010`, which `lsusb` mislabels as "fastboot"). FEL
  (`1f3a:efe8`) loads a RAM-only U-Boot that runs `ums 0 mmc 1` (eMMC is mmc 1).
  The flasher writes sector 0 and everything from 4 MiB, nothing in between.
  To leave it: hold Power until the blue LED goes off and release right away (a short press does
  nothing; holding longer seems to restart it and then cut it off mid-boot, unconfirmed), then
  press Power briefly to boot.
- **Trial boots**: `pp-update` marks the new slot on trial (`/var/lib/pp-update/trial` holds the
  previous slot). `S98pp-trial` confirms it once the USB gadget is `configured` (within 60 s), or
  moves the flag back and restarts via SysRq (watchdog as backstop); a second unconfirmed boot also
  rolls back. Needs the phone plugged into the host. Kernel panics before user space are not
  covered (needs a bootloader boot counter). The flasher uploads `pp-update` from this tree before
  each update and reports confirmed / rolled back.
- Flag and slot writes use `dd conv=fsync`: plain `sync` does not flush the eMMC write cache for
  raw block devices, so a hard power-off lost a flag change once.

Rules:
- Never write the eMMC boot0 partition or the first 4 MiB (except sector 0). The
  bootloader is what keeps the phone flashable.
- Writing to the eMMC needs `sudo`; Claude cannot enter the password, the user runs it.

## Phone side

- Consoles: display (tty1), USB ACM gadget (ttyGS0 → host `/dev/ttyACM0`), headphone-jack
  UART (ttyS0, 115200). Root has no password on consoles; SSH is key-only.
- USB network (NCM): phone `172.16.42.1`, host gets `172.16.42.2` via udhcpd, fixed MACs.
- USB-C (PHY 0) is shared by the MUSB gadget controller and the EHCI0/OHCI0 host controllers.
  Mainline enables both; EHCI0/OHCI0 probing sets the PHY to host ("Changing dr_mode to 1"),
  overriding `dr_mode = "peripheral"`, and MUSB sits in `a_wait_vrise`: no gadget on the host.
  `external/patches/linux/0002-…` disables EHCI0/OHCI0. `0001-…` keeps `vcc-1v8-typec`
  (ANX7688 USB-C bridge supply, unclaimed in mainline) on; it is not known to be required.
- Boot diagnostics (S99bootlog, development only) go to `/var/lib/bootlog/boot-N.txt`. Read them
  without sudo: Volume Up, `just expose`, `udisksctl mount -b /dev/sdX1 -o ro,noload` (plain `ro` still replays the journal).

## Gotchas

- Flakes only see git-tracked files: `git add` new files before `nix build`/`nix run`.
- Only one Buildroot build at a time in `output/` (concurrent builds corrupt each other,
  e.g. `host-python-installer` "File already exists"). Check `pgrep -a make` first.
- When running a build in the background, don't pipe through `tail`: it hides make's exit code.
- The user may run builds themselves; don't start one without asking.

## Kernel config

- `linux.defconfig` is trimmed for the PinePhone: arm64 defconfig with every platform except
  ARCH_SUNXI disabled, then `localmodconfig` against the modules loaded on the phone
  (`/proc/modules`), then the fragment. 85 modules instead of ~1400.
- `linux.fragment` is the keep list and is applied on top: drivers needed to boot and reach USB
  (built in), PinePhone hardware not in use yet (modem, Wi-Fi/BT, sensors, vibrator, battery,
  audio) and what is explicitly off (PCI, ACPI, virtualization, NUMA). New hardware support goes
  here, so a future re-trim keeps it.
- localmodconfig only keeps modules loaded at that moment: hardware that is powered later
  (e.g. the modem) must be in the fragment or it is dropped.
