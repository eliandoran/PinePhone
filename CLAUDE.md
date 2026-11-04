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
just ui [args]          # push phone-ui from the working tree to the phone, no flash (~20 s)
just ui-reset           # back to the phone-ui installed in the image
just ui-log             # follow phone-ui's output
just screenshot [file]  # the phone's display as JPEG into screenshots/ (on the phone: Power + Volume Down)
just app <name>          # build apps/<name> (external app) and install it, restarts phone-ui
just app-rm <name>       # remove an external app from the phone
just roms <files>        # game ROMs (zips unpacked) for apps/genesis, into /var/lib/genesis/roms
just bios <files>        # Sega CD BIOS for apps/genesis (zips unpacked, picked by MD5)
just wads <files>        # Doom IWADs (zips unpacked) for apps/doom, into /var/lib/doom/wads
just share-internet     # NAT the phone to the host's internet (sudo); unshare-internet
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
| `phone-ui/src/*.c`, or LVGL then rebuilt | `just ui`, no flash; `just deploy` when done |
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
- `external/board/pinephone/`: genimage layout, kernel/busybox fragments, post-build (dev SSH
  key, /data mount point), post-image (shrinks the rootfs, then genimage), rootfs overlay
  (inittab, init scripts, udhcpd, extlinux, pp-update).
- `scripts/pinephone-flash.sh`: the flasher, wrapped by the flake (shellcheck runs at build).
- `apps/<name>/`: external apps, not in the image (Makefile, `app.json`, `icon.svg` that
  `just app` renders to `icon.png`); `apps/hello` is the template, `apps/telegram` the plugin one.
- Ignored: `buildroot/`, `output/`, `dl/`, `keys/`, `result*`. The first four may be links to
  another directory (`just build-dir <dir>`), e.g. WSL's ext4 when the sources are on `/mnt/c`.

## Boot and flashing design

```
eMMC boot0:  Tow-Boot (bootloader; not visible over USB mass storage)
eMMC user:   sector 0 MBR (disk signature 0x50480001)
             4 MiB    slot A  p1 512M  PARTUUID 50480001-01
                      slot B  p2 512M  PARTUUID 50480001-02
                      data    p3 rest  PARTUUID 50480001-03, ext4 PP_DATA (created on first boot)
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
- **Trial boots**: `pp-update` marks the new slot on trial (`/var/lib/pp-update/trial` on the data
  partition holds "SLOT PREVIOUS"; without a data partition there is no trial).
  `S98pp-trial` confirms it once the USB gadget is `configured` (within 60 s), or
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

## Phone UI (phone-ui)

- Iterate on the UI with **`just ui`**, not `just deploy`: it rebuilds only phone-ui
  (`phone-ui-rebuild`, same flags as the image), uploads it to
  `/var/lib/phone-ui/dev/` and restarts it, in about 20 s. Arguments are passed on, e.g.
  `just ui --open Camera` to start in an app. It prints the start of the log; check for
  errors there or with `just ui-log`.
- The pushed build survives reboots and runs while the image's `/usr/bin/phone-ui` is the
  one it was pushed over (`S90phone-ui` compares SHA-256): flashing a new image retires it.
  If it exits with an error, the installed one starts instead (the log says so).
- Pushes are serialized (`flock`) and refuse to run next to a Buildroot build. The last
  push wins: with several agents on the UI, the phone runs whoever pushed last
  (`/var/lib/phone-ui/dev/info` has the commit, time and user).
- LVGL is linked statically, so LVGL changes go the same way after rebuilding it:
  `just make lvgl-rebuild` (`lv_conf.h`) or `lvgl-dirclean` (patches), then `just ui`.
  The init script, the kernel and other packages still need a build and a flash.
- `phone-ui --bench [ms]` prints frame and CPU statistics per screen transition.
- App icons: `icons/<app>.svg` (128 x 128, lower-case app name) is the source; after editing
  one, run `gen-icons.sh` (dev shell, resvg), which renders them at the sizes drawn into the
  committed `src/icons.c` (PNGs embedded, so `just ui` takes them). An app without an SVG
  shows its color and symbol from `home.c`.
- External apps live in `/var/lib/apps/<id>/` (data partition, kept across flashes) with an
  `app.json` and optional `icon.png`; `src/external.c` documents the format. phone-ui lists them
  after the built-in apps, found at startup. Opening one hands it the display like a VT switch:
  phone-ui closes its input devices, drops DRM master and waits; the app opens `/dev/dri/card0` and
  `/dev/input/event*` itself and draws with KMS. Holding Power 2 s kills it; it also gets SIGTERM
  when phone-ui ends (an orphan keeping DRM master leaves phone-ui unable to start: fbcon shows).
  Build them with the Buildroot toolchain, statically linked (`-static`) or with their
  libraries in `lib/`: the image's libraries change with every flash.
- Plugin apps (`"plugin": "x.so"` in `app.json`, e.g. `apps/telegram`) are shared libraries
  phone-ui loads at startup and runs in its process with its UI, like built-in apps. They build
  against `output/staging/usr/include/phone-ui` (`ui.h`, `json.h`, `abi.h`) and LVGL's headers;
  phone-ui exports its symbols and links all of LVGL. `UI_ABI` hashes those headers: a plugin
  built for others is not loaded (logged), so after changing `ui.h`, `json.h` or LVGL, run
  `just make phone-ui-rebuild` then `just app <name>` again. Their libraries go in `lib/`
  (runpath `$ORIGIN/lib`). TDLib is built for staging only, `apps/telegram` carries it.
- The web engine (WPE WebKit, GLib, ICU, fontconfig, fonts… about 165 MB) is built by Buildroot
  but not in the image: `board/pinephone/web-engine.sh` (post-fakeroot) removes it from the
  image's copy of the target, links WebKit's compiled-in folders (`/usr/libexec/wpe-webkit-2.0`,
  `/etc/fonts`, `/usr/share/fonts`, `/usr/lib/gio`…) to `/var/lib/apps/web/root/`, and fails the
  build if anything left needs a library that went with it. `apps/web` (`just app web`) packs
  the same files from `output/target` with runpaths. After rebuilding WebKit or a library it
  uses, run `just app web` again; a fresh phone has no Web until it is installed.

## Phone side

- Consoles: USB ACM gadget (ttyGS0 → host `/dev/ttyACM0`), headphone-jack UART (ttyS0,
  115200, also the kernel console at `loglevel=5`). Root has no password on consoles; SSH is
  key-only. The display has no console text, only the boot splash.
- Boot splash: `phone-ui/splash.svg`, rendered by `gen-splash.sh` (dev shell) into
  `splash.ppm` (the kernel's boot logo, copied into the tree by `external.mk`; fbcon centers
  it) and `src/splash_image.c` (phone-ui's first frame, same pixels, fading into home). Then
  `just make linux-rebuild phone-ui-rebuild all`. `quiet` would hide the logo (fbcon skips it
  at loglevel ≤ 4).
- USB network (NCM): phone `172.16.42.1`, host gets `172.16.42.2` via udhcpd, fixed MACs.
  The phone's default route is the host, with public DNS (9.9.9.9, 1.1.1.1): it reaches the
  internet once the user runs `just share-internet` (sudo, NAT with iptables, until the host
  reboots). `S10ntpd` then sets the clock.
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
- The justfile runs builds in a systemd scope capped at 32 GB of RAM, no swap (`fhs` variable):
  an uncapped WebKit build ran the host out of memory. A plain `nix run .#fhs` is not capped.
  WebKit compiles with 10 jobs (`external.mk`); if the OOM killer still hits, lower that.
- Local-source packages (phone-ui) are copied into `output/build/<pkg>/` without deleting:
  a file removed or moved in `src/` stays there and is still compiled (`*.c` globs), e.g. a
  "multiple definition" at link time. Delete the stale copy, or `just make <pkg>-dirclean`.
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
- `axp20x_usb_power` (CONFIG_AXP20X_POWER) breaks the USB gadget (tested). Likely cause: BC1.2 charger
  detection on the data lines; the host's enumeration stalls ("new high-speed USB device"
  and nothing more; gadget `not attached`). The AXP803 appears to keep that state across restarts:
  after a kernel with that driver ran, any kernel fails USB until a full power-off. A trial
  rollback (a restart) therefore does not recover USB from a kernel that corrupts PMIC state.

## Root filesystem and data

- The root filesystem is ext4 mounted **read-only** (`ro` in extlinux.conf, no remount in
  inittab). `post-image.sh` shrinks `rootfs.ext2` to its content plus 256 blocks of slack (room
  for pp-update to rewrite `extlinux.conf` in a fresh slot). Squashfs/erofs would need the
  bootloader to read them, so they wait for our own U-Boot.
- `S00data` creates partition 3 from the end of slot B to the end of the eMMC on first boot
  (sfdisk + partx on the in-use disk), formats it only if it has no ext4 yet, fscks and mounts
  it on `/data`, then bind-mounts `/data/var/lib` over `/var/lib` (seeded with `cp -an` from the
  image). A full USB flash rewrites sector 0 without p3; the next boot recreates the entry at
  the same start and keeps the data.
- Writable state therefore goes in `/var/lib` (persistent) or `/tmp`, `/run` (RAM).
  `/etc/dropbear` points to `/var/lib/dropbear` (post-build.sh), so the SSH host key persists.
- Updates are streamed gzip-compressed (`gzip -1` on the host, `gunzip` on the phone).
