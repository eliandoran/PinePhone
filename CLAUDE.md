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
just image-size [--files N] # what the root filesystem image holds per package (and its N largest files)
just flash              # over the air (phone running our firmware), fails if unreachable
just deploy             # build, then flash
just flash-usb          # the full image via USB mass storage (Volume Up at power-on, or FEL)
just expose             # only expose the eMMC as a USB disk, write nothing
just ssh [cmd]          # root shell on the phone (172.16.42.1) using keys/pinephone
just console            # serial console over the USB gadget (/dev/ttyACM0), auto-reconnects
just logs               # follow the kernel log over SSH from boot start, across reboots
just ui [args]          # push phone-ui from the working tree to the phone, no flash (~3 s)
just ui-reset           # back to the phone-ui installed in the image
just ui-log             # follow phone-ui's output
just test [filter]      # phone-ui's tests on the host (unit, and UI headless), no phone or build
just coverage           # their code coverage: per file, HTML line by line, sources without tests
just i18n [--list path] # translation coverage: texts marked for translation per file, or those not;
                        # --update: po/*.po up to date with the sources
just sim [args]         # phone-ui in a desktop window from the working tree (--open <app>, --scale f)
just screenshot [file]  # the phone's display as JPEG into screenshots/ (on the phone: Power + Volume Down)
just app <name>          # build apps/<name> (external app) and install it, restarts phone-ui
just app-rm <name>       # remove an external app from the phone
just books <files>       # ePub books for the Books app, into /var/lib/books
just roms <files>        # game ROMs (zips unpacked) for apps/genesis, into /var/lib/genesis/roms
just bios <files>        # Sega CD BIOS for apps/genesis (zips unpacked, picked by MD5)
just wads <files>        # Doom IWADs (zips unpacked) for apps/doom, into /var/lib/doom/wads
just share-internet     # NAT the phone to the host's internet (sudo); unshare-internet
just docs               # preview the user documentation (Zensical), docs-build into site/,
                        # docs-shots: its screenshots made by phone-ui's tests,
                        # docs-demo: the front page's phone-ui in the browser
just wasm               # phone-ui as WebAssembly (phone-ui/wasm/build/); wasm-serve on localhost:8080
                        # wasm-smoke: it used in headless Chromium (wasm/smoke.mjs)
just shell              # interactive shell in the Buildroot FHS environment
just menuconfig         # then `just savedefconfig` to keep the changes
just linux-menuconfig   # then copy the options into board/pinephone/linux.fragment
just make <target>      # any Buildroot target, e.g. just make linux-rebuild
```

### Rebuilding after changes

Buildroot only rebuilds what is not built yet; it does not track changes. `just build`
always regenerates the rootfs and image (overlay, post-build, genimage) cheaply. phone-ui is
the exception: `phone-ui.mk` copies and builds it again when `src/`, the `.mk` or the init
script is newer than the copy. Its `Makefile.build` compiles only the sources that changed (a
header they include, or the flags), in parallel, objects in `output/build/phone-ui-1.0/obj/`.

| Changed | Run |
|---|---|
| `phone-ui/src/*.c` | `just ui`, no flash; `just deploy` when done (rebuilds phone-ui itself) |
| LVGL, then rebuilt | `just ui`; for the image `just make phone-ui-rebuild all` |
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
- `Makefile`: downloads Buildroot 2026.08.1 into `buildroot/` with our patches to Buildroot itself
  (`external/buildroot-patches/`: unpacked again when one changes), builds with
  `BR2_EXTERNAL=external` into `output/`, downloads go to `dl/`. Generates `keys/pinephone`.
- `external/configs/pinephone_defconfig`: Bootlin glibc toolchain, latest kernel (7.1.x) with
  `board/pinephone/linux.defconfig` + `board/pinephone/linux.fragment`, dropbear, mdev.
- `external/board/pinephone/`: genimage layout, kernel/busybox fragments, post-build (dev SSH
  key, /data mount point), post-image (shrinks the rootfs, then genimage), rootfs overlay
  (inittab, init scripts, udhcpd, extlinux, pp-update).
- `scripts/pinephone-flash.sh`: the flasher, wrapped by the flake (shellcheck runs at build).
- `apps/<name>/`: external apps, not in the image (Makefile, `app.json`, `icon.svg` that
  `just app` renders to `icon.png`); `apps/hello` is the template, `apps/telegram` the plugin one.
- `docs/` + `zensical.toml`: user-facing documentation (Zensical, from the dev shell).
- `notes/`: developer notes and investigations (e.g. `wifi-drops.md`), not published.
- `demo/`: books, photos and music anyone may share (sources and licenses in its README), for the
  browser demo, the simulator and Gallery's screenshots in the documentation.
- Ignored: `buildroot/`, `output/`, `dl/`, `keys/`, `result*`, `site/`. The first four may be links to
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
  If the phone refuses `keys/pinephone` (an image built from another checkout, with its own
  `keys/`), the flasher logs in on the USB serial console (`/dev/ttyACM0`, `PINEPHONE_CONSOLE`;
  under WSL with the phone attached to Windows, its COM port through `powershell.exe`)
  and bind-mounts a `/tmp` copy of `authorized_keys` with our key added, until the next reboot.
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
  (`Makefile.build` with the flags Buildroot last built it with, saved in its `obj/buildroot.mk`;
  `phone-ui-rebuild` until there are some, or after `phone-ui.mk` or the configuration changed), uploads it to
  `/var/lib/phone-ui/dev/` and restarts it, in about 3 s (it prints each phase; outside the FHS
  environment when the toolchain runs without it: nix-ld on NixOS). Arguments are passed on, e.g.
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
- **Test first.** Before fixing a phone-ui bug or adding a feature, write the test that shows it
  (UI test of what a user does, or unit test), run it and see it fail for the expected reason,
  then change the code until it passes and `just test` passes. The `phone-ui-test` skill has
  the procedure and the harness. What the host cannot show (looks, frame rate, real hardware,
  kernel, boot) is checked on the phone, and said so.
- Simulator: `just sim` runs the UI tests' phone-ui (`tests/sim.c` on `ui_test.c`: same display,
  sandbox, modem and Wi-Fi fakes) in an SDL3 window in real time, with the host's libcurl and ALSA.
  The mouse is the finger, the keyboard types into the focused field; End is Power, Page Up/Down
  the volume keys, Escape back, F5 an SMS, F6 the charger, F12 a screenshot, Ctrl+=/- zoom (a
  fixed-size window, 80% of the screen's height, app ID `pinephone-sim`). `/var/lib` is kept in
  `tests/build/sim/var-lib` (`demo/`'s books, photos and music in each folder it lacks; the plugin
  apps GitHub and Terminal, on `/bin/sh`, built for the host as for their tests, in `apps/`). For
  looks and behavior; speed and hardware stay `just ui`'s.
- Tests: `phone-ui/tests/`, run with `just test [filter]` in the flake's `tests` shell (host gcc,
  ASan and UBSan, phone-ui's libraries, `lvgl-host`: the image's LVGL version, patches and
  `lv_conf.h` without NEON and with `LV_USE_TEST`; `litehtml-host`: 0.10 as in the image).
  `test.h` is the framework: `TEST(name)`, `CHECK*`, `TEST_SETUP`; each test runs in its own forked
  process. Every test program runs in a bubblewrap sandbox (Makefile `SANDBOX`): the host
  read-only, `/var/lib` and `/sys` in `tests/build/run/test_<name>/` (`var-lib`, `sys`), no devices, no
  network but loopback, `TZ=UTC0` as on the phone (phone-ui writes `/sys/power/state`,
  backlight, cpufreq; Network lists interfaces). `ui_test_reset()` empties both (only with the
  Makefile's `.phone-ui-tests` marks) and fills `/sys` with a PinePhone's: backlight 500/1000, battery 76% discharging, thermal zones,
  the PMIC's IIO ADC (a symlink through `axp813-adc`), cpufreq. Tests change files after it and
  before `ui_test_start()` for what phone-ui reads at startup (settings, sysfs).
  - Unit tests (`TESTS`) link the sources they list (`<name>_SRCS`) plus `stubs.c` (the few
    `ui.c`/`widgets.c` functions they call), or `#include` an app's `.c` to reach its `static`
    state (`test_calculator.c` presses keys through their callbacks, no display driver).
  - UI tests (`UI_TESTS`, e.g. `test_notes.c`) run all of phone-ui but `main.c`, started by
    `ui_test.c` (`ui_test.h`): the phone's OpenGL ES draw unit on Mesa's llvmpipe (surfaceless EGL,
    GLES 2, `gladLoadGLES2()` since LVGL calls GL through glad), a 720 x 1440
    `lv_opengles_texture_create()` display, LVGL's emulated pointer, time only in
    `ui_test_wait()` (16 ms frames, as `main.c`'s loop: `lv_refr_now()` skips a display without
    its refresh timer), also phone-ui's monotonic clock (UI tests link with
    `--wrap=clock_gettime`: stopwatches and timers count it; the wall clock is real unless set: `ui_test_wall_clock(t)`, `time()` wrapped too). Tests go by text as a user: `ui_test_open("Notes")` (home, menu, tile),
    `ui_test_tap_text()` (of several labels with that text, the one pressed nearest to it: the
    key, not the number it typed or an avatar's symbol), `ui_test_long_press_text()`,
    `ui_test_tap_field("Message")` (by text or placeholder), `ui_test_type()` (the on-screen
    keyboard's keys, shifting as needed), `ui_test_back()`, `ui_test_shows()`/`_part()` (labels, and span groups by their spans joined; scrolled
    out of view counts, hidden does not), `ui_test_font(text)` (its label's font), `ui_test_find_widget(&lv_slider_class)`/`_widgets()`,
    `ui_test_tap_at()`, `ui_test_swipe()` (gestures, drags; the pointer is read once a frame),
    `ui_test_button(KEY_POWER, pressed)`/`_press()` (Power and the volume keys, two key devices
    as on the phone, both wrapped by `volume.c`); `poweroff`/`reboot` are fakes first in `PATH`
    (`ui_test_ran()`); `ui_test_write()` puts data in place first. The network is
    `tests/fake_curl.c`, linked instead of libcurl (phone-ui unchanged): `ui_test_serve(url, status,
    type, body)` (fnmatch() patterns, `_once`, `_redirect`, `_error`), `ui_test_network_hold()`,
    `ui_test_serve_in_pieces()` (streams), `ui_test_requested()` (URL, method, body, headers); an
    address nothing serves is offline. Sound is `tests/fake_alsa.c`: the `snd_pcm_*` calls wrapped
    (`--wrap`, the mixer stays alsa-lib's without a card), a card on the tests' clock:
    `ui_test_microphone(peak)` (a 440 Hz tone), `ui_test_played()`, `ui_test_sound_xrun()`,
    `ui_test_no_sound_card()`. wpa_supplicant is `tests/fake_wpa.c`: its control socket on wlan0
    answered from a thread (`WPA_CTRL_DIR` is `/tmp/wpa_supplicant` for `wifi.o`), started by
    `ui_test_wifi_running()`, `_ap(ssid, signal, flags, password)`, `_saved()`, `_up()`/`_downs()` (wlan0's
    state, how often phone-ui took it down); it connects when the
    password matches, refuses with WRONG_KEY otherwise, and SAVE_CONFIG writes the real file. The modem's AT port is
    `tests/fake_modem.c`: a pty linked at `/tmp/modem` answered from a thread,
    `ui_test_modem(UI_TEST_SIM_...)` (plugs it in, then changes the card: +QSIMSTAT),
    `_unplug()`, `_usb_away()`/`_usb_back()` (its USB link reset as on waking, the modem going on: what it
    says meanwhile lost), `_incoming_unheard()` (a call whose first RING was lost, the port kept),
    `_restart()` ("RDY"), `_silent()`, `_sim_busy()`, `_network(UI_TEST_NET_..., name)`
    (+CREG/+CEREG, +COPS; home on "Fake Mobile" unless set), `_lte_only()`, `_signal(dbm, UI_TEST_TECH_...)`
    (+CESQ, +CSQ, +COPS's AcT), `_no_cesq()`, `_usb("ttyUSB3")` (also a USB serial port in sysfs,
    for `usb=`/`interface=`), `_data(&starts)` (the `AT$QCRMCALL` session), `_data_fails(n)`,
    `_data_drop()`, `_data_left()` (a session up at plug-in, an earlier phone-ui's); SMS in PDU mode with a store of its own (PDUs made and read there, not by
    `sms_pdu.c`): `_sms(sender, text)` (stored, +CMTI), `_sms_part(sender, text, ref, total, seq)`,
    `_sms_stored()`, `_sms_sent(i, &number)` (AT+CMGS, parts joined), `_sms_fails(n)`,
    `_sms_busy(n)`; USSD: `_ussd(request, m, reply)` (+CUSD a second later in AT+CSCS's character
    set, multi-line in "GSM" as the EG25-G; unscripted: +CME ERROR: 258 after the OK), `_ussd_push()`,
    `_ussd_sent(i)`, `_ussd_cancels()`, `_ussd_overlaps()` (commands while a reply was awaited),
    `_ucs2()`; without them there is no modem.
    `ui_test_reset()` writes the board's `/tmp/modem.conf` (`MODEM_CONF` for `modem.o`): the
    PinePhone's on that port, changed before `ui_test_start()`, its `data_script=` one that logs
    what it is asked (`ui_test_mobile_data_ran("up wwan0", ms)`). Music's `mpg123 --remote` is a script first in `PATH`
    (`ui_test_mpg123_got()`, `_at()`, `_end()`). `ui_test_sensors()` makes the MPU6050 and
    AF8133J (sysfs, their IIO buffers files in the sandbox's `/dev`), `ui_test_sensors_feed()`
    their samples. The cameras are `tests/fake_camera.c`: `ui_test_camera(rear, front)` makes the
    media pipeline's nodes (files in `/dev`, sysfs), their `ioctl()`s answered (`--wrap=ioctl`, through
    `fake_wpa.c`'s), frames of `ui_test_camera_scene(rgb)` on the tests' clock at the sensor's rate;
    `_stall()`, `_focus(busy_polls, reached)`, `_control(id)`, `_streaming()` ("ov5640 2592x1944 BGGR").
    The display's scanout is `tests/fake_drm.c` (libdrm wrapped, `DRM_WRAPS`): `/dev/dri/card0` serves the
    last frame to `screenshot.c`. `splash.c`'s once-per-boot mark is in `/tmp` (`MARKER`).
    Browser (`apps/web`) is tested by `test_web.c`, the first external app with tests: the Makefile
    builds `web.c` for the host as `web.so`, which the test installs in `/var/lib/apps/web` and
    `external.c` loads (the program is linked `-rdynamic` with all of LVGL, as phone-ui is). Its
    engine is `tests/fake_webkit.c`: the web view a GObject with WebKit's properties and signals, loads
    in GLib idle sources, pages from `fake_curl.c` (`<title>`, history, error page), frames EGLImages
    of the page's first `background: #rrggbb`, handed over after each frame complete;
    `ui_test_web_*()` (touches, crash, new window, views and images alive, scale, settings). It builds
    against libwpe's and WPEBackend-fdo's real headers (the flake's, the image's versions) and WebKit's
    declarations in `tests/fake_webkit/wpe/webkit.h`: a WebKit function `web.c` starts using goes there,
    as WebKit 2.50's header declares it, and in the fake.
    Terminal (`apps/terminal`, `test_terminal.c`) is built and loaded the same way, without a fake: its
    shell is the script each test writes (`TERMINAL_SHELL`, `/tmp/terminal-shell`, the tests' `PATH`),
    which prints escape sequences and waits with `read` for the test to type Enter.
    GitHub (`apps/github`, `test_github.c`) too, its API served by `fake_curl.c` through `ui_fetch_request()`.
    Telegram (`apps/telegram`, `test_telegram.c`) too, TDLib `tests/fake_tdjson.c` in the program
    (`ui_test_td_*()`: an account, its bio and photo, failures, held answers, requests sent), reached
    through `fake_tdjson/libtdjson.c`, a `libtdjson.so` the test loads first: ASan's `dlopen()`
    interceptor hides telegram.so's `$ORIGIN/lib` runpath, its soname finds it loaded.
    Emoji images come from the flake (`emoji-host`, `EMOJI_DIR` for `emoji.c`);
    `ui_test_tap_button()` taps a button-matrix button (emoji panel). The programs run `JOBS` at a time (`nproc`; the fake modem keeps pace with the tests' clock: `ui_test_modem_sync()`; their output
    in order; Mesa's shader cache in `tests/build/mesa-cache`), one make per build, building and running (`tests/build/.lock`: another building meanwhile left a plugin built for other headers; the simulator's window runs without it); a plugin that does not load stops its program at the first test. Failures print what the screen shows; `ui_test_screenshot()` saves a PNG. An LVGL error fails the test (`test_errors()`, asked
    by `test.h`, printed), e.g. a draw task the GLES draw unit does not support. About 0.15 s per test, about 30 s for all; a wait on a
    fake's thread or a process is `ui_test_wait_until()`/`_wait_for()` (real time too), not a
    loop of `ui_test_wait()`.
  `just coverage` builds them again with `--coverage` in `tests/build/coverage/` and reports with
  gcovr (generated fonts and images left out), then the external apps with tests in a second report
  (`html/apps/`, the others listed); `test.h` calls `__gcov_dump()` before a test
  process `_exit()`s, or its counts would be lost.
  CI (`.github/workflows/tests.yml`) runs `just test` then `just coverage` on pushes to main and
  pull requests touching phone-ui, LVGL, litehtml or the flake; the coverage HTML is an artifact,
  failures upload screenshots and each program's `var-lib` and `log`. Ubuntu's AppArmor blocks bubblewrap's user namespaces
  unless `kernel.apparmor_restrict_unprivileged_userns=0`. A second job runs `just wasm-smoke`.
- App icons: `icons/<app>.svg` (128 x 128, lower-case app name) is the source; after editing
  one, run `gen-icons.sh` (dev shell, resvg), which renders them at the sizes drawn into the
  committed `src/icons.c` (PNGs embedded, so `just ui` takes them). An app without an SVG
  shows its color and symbol from `home.c`.
- Fonts: text takes its role's, `UI_FONT_CAPTION`/`_BODY`/`_BODY_LARGE`/`_TITLE`/`_HEADLINE`/`_DISPLAY`
  (`ui.h`: 20, 24, 26, 26, 32, 48 px; 20 px is the smallest text read at the display's 270 ppi);
  a symbol sized to its box (a button's icon, keys, avatars) takes `ui_font_<px>`.
- Small icons are glyphs: LVGL's `LV_SYMBOL_*` (a fixed Font Awesome subset in Montserrat, the
  fonts' last fallback) and ours, `UI_SYMBOL_*` (`src/ui_symbols.h`): Font Awesome 7 Free Solid
  and Regular icons listed in `symbols.txt` (name and codepoint from fontawesome.com, `regular`
  after it for `UI_SYMBOL_<NAME>_REGULAR`, moved to U+F0000 + its codepoint since the styles
  share them, or `brands` for a Brands one; then degrees clockwise for a turned copy,
  `UI_SYMBOL_<NAME>_<DEGREES>`, e.g. the hung-up handset `UI_SYMBOL_PHONE_135`, the status
  bar's upright `UI_SYMBOL_USB_270`), added to every `ui_font_<size>` by `gen-fonts.sh`
  (dev shell, `FONTAWESOME`). Turned ones, and Solid ones on LVGL's codepoints (`phone`: LVGL's
  `LV_SYMBOL_CALL` is Font Awesome 4's mirrored handset), go into a font of their own
  (`turned.py`, fontTools) at U+100000 onward: the GLES draw unit cannot rotate a label.
  Add one there and run it rather than drawing an icon; only what shows a level (Wi-Fi, signal
  bars, the battery: `ui_battery_icon_create()`, used by the bar, the shade and Settings) is drawn. Only the Free set is in the fonts (many icons on fontawesome.com are Pro):
  list its names and codepoints with `ttx -t cmap` on `$FONTAWESOME/Font Awesome 7
  Free-Solid-900.otf` (dev shell), a comment after the line says where it is used. Labels take
  `UI_SYMBOL_*` with `#include "ui_symbols.h"`, in any `ui_font_<size>` (tests find them by that
  text too). `gen-fonts.sh` needs LVGL extracted (`output/build/lvgl-*`) and rewrites every
  `ui_font_*.c`: commit them with `symbols.txt` and `ui_symbols.h`.
- Translations (`i18n.h`, all of phone-ui and of the plugin apps):
  texts a user reads go through `_()` (`C_()` with a context, `N_()` in static tables, `P_()` a
  plural; `ui_day_name()`/`ui_month_name()` rather than strftime()'s: the phone has no locales).
  Texts kept or logged in English (an app's name, the modem's reason a message was not sent) are
  `N_()` there and `_()` where shown. Each language is a gettext catalog, `phone-ui/po/<code>.po`
  (header: `Language`, `X-Language-Name`, `Plural-Forms`), which `gen-translations.py` turns into C
  in each build (image, tests, wasm). After changing marked texts, `just i18n --update` (xgettext,
  msgmerge) adds the new ones untranslated and marks changed ones fuzzy (its guesses: check them);
  then translate them in the `.po`. `test_i18n.c` fails on a marked text without a translation or a
  translation of none. What stays as it is in every language (a product name) has `NON-NLS` in a
  comment on its line (`NON-NLS-BEGIN`/`-END` around a block). `just i18n` reports each file's
  coverage (`i18n-check.py`); `just test` fails on an unmarked text anywhere in `src/`
  or a catalog `msgfmt --check-format` refuses.
  A plugin app has its own catalogs, `apps/<name>/po/<code>.po` (same header), in phone-ui's
  current language: its Makefile runs staging's `gen-translations.py --app` and compiles that and
  its sources with `-DUI_APP_CATALOG`, which points `_()`/`C_()`/`P_()` at its `ui_app_catalog`
  (hidden: one per plugin). `just i18n --app apps/<name> [--list|--update]` as for phone-ui;
  `just test` checks every app with a `po/` (marked, each text translated, none unused), and
  a plugin the tests build (tests/Makefile) needs its catalog too (`app_catalog`).
- No board specifics in phone-ui: the modem goes through providers (`modem_provider.h`; `modem_at.c`
  speaks standard AT), chosen with their settings by the board's `/etc/phone-ui/modem.conf`
  (`board/pinephone/rootfs-overlay`: provider, the modem's USB device and AT interface, Quectel
  quirks: a `ttyUSB` number changes when the modem resets while its port is open; `boot_wait=`: seconds
  after boot within which it shows up on USB, "Starting" rather than "No modem" until then). Without it there is no modem.
  The apps only see `modem.h`. Powering the modem on is the board's init scripts' job.
  Mobile data (the shade's toggle and Settings › Network's, `mobile_data` in the settings, off by default;
  while roaming only with `data_roaming`, also off by default): the provider starts the modem's session (`AT$QCRMCALL`, Quectel quirk), the board's
  `data_script=` (`/etc/phone-ui/mobile-data.sh`) sets up `data=` (`wwan0`: raw IP, udhcpc, default
  route metric 5 between Wi-Fi's 0 and USB's 10, DNS) with `up`/`down`, and `reset` rebinds
  qmi_wwan when the modem refuses a session (it does after its USB reset in suspend).
  phone-ui does not end the session as it exits: the next one takes it over (`modem_data_left()`,
  the modem's `AT$QCRMCALL?` at the first opening, wwan0 still up), so `just ui` keeps mobile data.
  SMS: the provider's `sms_send` (`modem_at.c`: PDU mode, `sms_pdu.c` encodes and decodes,
  +CMTI then AT+CMGL, each message deleted from the modem once saved, long ones joined), to and
  from Messages' conversations in `/var/lib/messages/messages.json` (`data.c`). An SMS or a call
  wakes the phone: the modem's RI pulses PL6 low (120 ms; every 6 s while ringing), a gpio-keys
  wakeup source (`0034-…`, 1.2 board), "woken by RI" in phone-ui's log. The modem's USB host
  controller (ehci-platform) stays powered in suspend (`ehci_platform.keep_powered=1` in
  extlinux.conf, `0036-…`, its `power/wakeup` enabled keeps the PHY up): the modem gets a plain USB
  resume (15 ms) and what it said meanwhile, a call's first RING, arrives. Without it the controller
  is reset and powered down, the modem gets a USB reset (420 ms more of resume), which `option`
  takes (`reset_resume`, `0035-…`: its ports stay, `ttyUSB2` with phone-ui's open port) but what
  the modem said meanwhile is lost. So phone-ui also asks for the calls (AT+CLCC) on waking
  (`modem_woke()`, the provider's `woke`) and when the port comes back. `S01resume` marks the
  controller's suppliers and companion (PHY, clocks, OHCI1) and the display asynchronous
  (`power/async`, `CONFIG_PM_ADVANCED_DEBUG`): resumed in turn, the modem's USB waited for the
  display's 250 ms. Resume now takes about 250 ms, the display's (the panel's initialization).
  Unmeasured: what keeping the controller powered costs asleep (no charge counter, and the
  phone was on USB). Woken so (no Power press arrives after waking), phone-ui sleeps again
  once left 10 s on the lock screen without input, no call on (`power.c`, `AGAIN_MS`); woken
  by Power, only Power sleeps it.
  `sms_domain=cs` in the board's `modem.conf`: the EG25-G's firmware sends SMS over IMS on LTE
  (NV `sms_domain_pref` 01), which Digi refuses (`+CMS ERROR: 350`, sent fine on 2G); phone-ui
  writes 00 (circuit switched, SGs) when it differs and restarts the modem once. `ims=off` too
  (`AT+QCFG="ims",2`; 0 is "as the carrier profile says", which enables it): Digi connects this
  modem to IMS but releases its VoLTE calls at once (`+CEER: 5,36`, the callee sees a missed call);
  off, calls fall back to 2G as USSD does, and ring. Both written before one restart.
  The modem's second AT port (interface 3, `ttyUSB3`/`4`) answers AT commands too: use it to
  query the modem by hand while phone-ui holds interface 2's. Unsolicited lines (+CUSD, +CMTI) only
  reach the URC port (`AT+QURCCFG="urcport","usbat"`, interface 2): to see them there by hand, set
  it to `"usbmodem"` (interface 3), and back to `"usbat"` after.
  USSD (`ussd.c`, codes dialed in Phone: `*`/`#` first, `#` last): `AT+CSCS="UCS2";+CUSD=1,...` on one
  line, `AT+CSCS="GSM"` again after the reply (`+COPS` names follow the character set); nothing else
  is sent while a reply is awaited (up to 30 s), as Digi refuses a code with a late `+CME ERROR`.
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
  against `output/staging/usr/include/phone-ui` (`ui.h`, `json.h`, `i18n.h`, `abi.h`, and
  `ui_symbols.h` for the fonts' icons) and LVGL's
  headers; phone-ui exports its symbols and links all of LVGL. `UI_ABI` hashes those headers (not
  `ui_symbols.h`: a symbol added breaks no plugin; after one, `just make phone-ui-rebuild` before
  `just app <name>` uses it): a
  plugin built for others is not loaded (logged), so after changing `ui.h`, `json.h`, `i18n.h` or LVGL, run
  `just make phone-ui-rebuild` then `just app <name>` again. Their libraries go in `lib/`
  (runpath `$ORIGIN/lib`). TDLib is built for staging only, `apps/telegram` carries it.
- The web engine (WPE WebKit, GLib, ICU, fontconfig, fonts… about 165 MB) is built by Buildroot
  but not in the image: `board/pinephone/web-engine.sh` (post-fakeroot) removes it from the
  image's copy of the target, links WebKit's compiled-in folders (`/usr/libexec/wpe-webkit-2.0`,
  `/etc/fonts`, `/usr/share/fonts`, `/usr/lib/gio`…) to `/var/lib/apps/web/root/`, and fails the
  build if anything left needs a library that went with it. `apps/web` (`just app web`) packs
  the same files from `output/target` with runpaths. After rebuilding WebKit or a library it
  uses, run `just app web` again; a fresh phone has no Browser (only Web Lite) until it is installed.

## User documentation

- **Document user-facing changes.** A change a user notices (new app or screen, setting, key or
  gesture, changed behavior or default, flashing steps) updates `docs/` in the same change; the
  `user-docs` skill has the style and Zensical's specifics. Internal changes need none.
- Screenshots of phone-ui are generated, not committed: `tests/test_docs.c` and `tests/docs/<app>.c` (a UI
  test per image, not in `just test`; `just docs-shots <app>_` makes one app's) by `just docs-shots` into `docs/images/generated/` (ignored), which
  `just docs`/`docs-build` run first.
- The front page runs phone-ui in the browser (an iframe of `docs/demo/index.html`, started by a
  button): `phone-ui/wasm/` builds it with Emscripten (the flake's `wasm` shell: LVGL's GLES draw
  unit on WebGL 1, `main.c` for the canvas and pointer, `stubs.c` for the hardware a browser lacks,
  `curl.c` libcurl over `fetch()`: CORS refuses most web pages; C++ with `-fexceptions`, litehtml
  throws; `emoji.c` fetches each emoji image from `emoji/` beside the page when first drawn, empty
  files standing in until then; the page asks for `phone-ui.js` and `.wasm` by the build's hash, the
  emoji by their set's version, against stale caches; `modem.c` a demo provider in `modem_at.c`'s
  place, `wifi.c` a wpa_supplicant and `player.c` an `mpg123 --remote` (<audio>) behind wrapped
  socket calls (`sockets.c`), libmpg123 built for it; `demo.c` fetches the repository's `demo/`
  files into `/var/lib`; nothing kept), `just wasm-smoke` its smoke test (`smoke.mjs`: the page in
  headless Chromium, apps opened and texts found through `probe.c`, tapped with the mouse, the
  console's errors fail it), `just docs-demo`
  copies it there (ignored), run first by `just docs`/`docs-build`.
- Pages show the UI's icons inline (Font Awesome shortcodes); `.claude/skills/user-docs/icons.md`
  maps each `LV_SYMBOL_*`/`UI_SYMBOL_*` to one: after changing `symbols.txt` or the symbols
  phone-ui uses, run `python3 external/package/phone-ui/gen-doc-icons.py` (dev shell).
- CI (`.github/workflows/docs.yml`) builds it (`just docs-build`) and publishes it on GitHub Pages
  on every push to main (the repository's Pages source must be "GitHub Actions").

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
  (ANX7688 USB-C bridge supply) on, in case its driver fails to probe.
- The ANX7688 USB-C bridge (Megi's driver, `0018-…`–`0023-…`, 1.2 board only in the DT) handles
  the CC pins and PD: without it a USB-C host stops supplying VBUS after a replug with the phone
  running (the PMIC sees no input) until a full power-off. Its firmware is in the bridge's
  EEPROM (0x2312 on this phone). No usb-role-switch link: the port stays peripheral only.
- Modem (EG25-G): mainline's device tree has none of its lines, `S30modem` powers it with
  libgpiod's tools (`package/gpiod-tools`: Buildroot's libgpiod2 wants 5.10 headers, the toolchain
  has 5.4, libgpiod carries its own) and holds them with a `gpioset` per chip; it is on USB
  about 12 s later (AT port: interface 2's tty, `ttyUSB2` unless renumbered). Its stop (rcK) sends `AT+QPOWD` and waits for STATUS before
  cutting the supply. STATUS reads high while off with the supply on, low when on or without it.
  The kernel has only the GPIO character device's v2 interface (no v1, no sysfs).
- Charging (AXP803, `0012-…`–`0017-…`): limits for the battery's 3 kΩ thermistor (mainline's
  defaults assume 10 kΩ and refuse to charge above ~14 °C), Vbus hold 4.5 V, input limit 3 A at
  boot, battery described in the DT (4.35 V, 1.2 A). The PMIC keeps its registers across
  restarts: what one kernel or test sets stays until a full power loss.
- Boot diagnostics (S99bootlog, development only) go to `/var/lib/bootlog/boot-N.txt`, with the
  previous boot's pstore (`/sys/fs/pstore`, mounted by inittab: its console after a watchdog reset). Read them
  without sudo: Volume Up, `just expose`, `udisksctl mount -b /dev/sdX1 -o ro,noload` (plain `ro` still replays the journal).

## Gotchas

- Flakes only see git-tracked files: `git add` new files before `nix build`/`nix run`.
- Only one Buildroot build at a time in `output/` (concurrent builds corrupt each other,
  e.g. `host-python-installer` "File already exists"). Check `pgrep -a make` first.
- When running a build in the background, don't pipe through `tail`: it hides make's exit code.
- The justfile runs builds in a systemd scope capped at 32 GB of RAM, no swap (`fhs` variable):
  an uncapped WebKit build ran the host out of memory. A plain `nix run .#fhs` is not capped.
  WebKit compiles with a job per 4 GB of RAM, at most 10 (`external.mk`): the cap does not help a
  host with less than 32 GB (a 30 GB WSL VM ran out with 10). If the OOM killer still hits, lower that.
- Local-source packages are copied into `output/build/<pkg>/` without deleting: a removed
  file stays there and is still compiled (`*.c` globs). phone-ui's copy deletes them
  (`--delete` in `phone-ui.mk`); another local package would need the same, or `<pkg>-dirclean`.
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
- `axp20x_usb_power` (CONFIG_AXP20X_POWER) runs with BC1.2 detection off (`x-powers,no-bc-detection`,
  `0021-…`): detection on a connected host stops the gadget's network sending (NCM TX, no log;
  tested) and sets a 500 mA input limit the AXP803 keeps across restarts. Earlier, with detection
  on, the host's enumeration stalled. A trial rollback (a restart) does not undo PMIC state.

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
