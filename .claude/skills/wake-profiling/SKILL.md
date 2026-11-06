---
name: wake-profiling
description: Measure and speed up the PinePhone waking from sleep, above all for an incoming call (from the screen lighting up to the call screen), on the real phone and unattended - the phone put to sleep by an injected Power press, called from an Android phone over adb, kernel and phone-ui timelines lined up. Use when profiling resume or suspend, the display's or the modem's resume, how fast the call screen or anything shown on waking appears, or when changing power.c's sleep and wake, the backlight around sleep, ehci-platform, the panel driver or S01resume.
---

# Profiling the wake (and a call's screen)

Everything here runs without anyone at the phones: a Power press is injected into the
PinePhone, an Android phone calls it over adb, both logs are collected. Only what the
eye sees (a flash, a frame of the wrong screen) needs the user: ask them to watch a few
rounds and say what they saw.

## A round, unattended

`call-round.sh <out-dir> [sleep-seconds]` (this directory): Power pressed, asleep 12 s,
called, hung up once phone-ui logs "call screen over the lock screen", then phone-ui's
log for the round (`<out-dir>/phone-ui.log`) and `dmesg` (`<out-dir>/dmesg.txt`).

- **The caller:** an Android phone over wireless adb (`adb devices` lists it; its
  address, port and the PinePhone's number are in Claude's memory, `samsung-adb-caller`,
  or ask the user: the adb port changes whenever Wireless debugging restarts). Calls
  between the two phones are free: call the PinePhone without asking, any other number
  only if the user says so. `PINEPHONE_NUMBER` must be set; `ADB_SERIAL` picks the phone.
  Dial: `adb shell am start -a android.intent.action.CALL -d tel:<n>`, hang up:
  `adb shell input keyevent KEYCODE_ENDCALL`.
- **Sleep without a hand on Power:** writing `struct input_event`s into the Power key's
  evdev node injects them (`/dev/input/event0`, `axp20x-pek`; `/proc/bus/input/devices`
  lists them). phone-ui reads them as a real press: fade, lock screen, sleep. Waking can
  only come from outside (a call, an SMS, the user's Power press): nothing runs asleep.
- **Wait for the modem after a boot** (a flash reboots): until phone-ui logs `network:`
  (about 15 s after boot), its start-up pulses the ring line and wakes the phone before
  the call, and the round measures nothing. The script warns.
- After a reboot, turn the kernel's resume timings on again (they reset):
  `echo 1 > /sys/power/pm_print_times; echo 1 > /sys/power/pm_debug_messages` over SSH.

## Reading the timelines

- **phone-ui's timed lines** (`log.h`: `[seconds]` of `CLOCK_MONOTONIC`): sleeping, awake
  (and what woke it), RING, AT+CLCC sent and answered (asked on waking), call incoming,
  call screen over the lock screen, screen lit, wlan0 up.
- **dmesg with `pm_print_times`:** every device's suspend and resume callback and how
  long it took (`... returned 0 after N usecs`), plus `PM: early resume ... complete`,
  `PM: resume of devices complete after`, `PM: suspend exit` (user space thawed).
- **The two clocks drift apart** (printk's against `CLOCK_MONOTONIC`, by up to a second
  after a few sleeps): line them up on `PM: suspend exit` ≈ phone-ui's "awake", or on
  wlan0's `deauthenticating` ≈ phone-ui's "sleeping". Check two anchors agree.
- **The screen comes on** when the panel driver enables it, at the end of
  `sun4i-drm display-engine` resuming, but phone-ui keeps it dark (brightness 0) until
  "screen lit": that line is when the user sees it.
- `/sys/power/pm_wakeup_irq` (what woke it) is in phone-ui's "awake" line.

## Where the time went, and what was done (November 2026)

From the ring line waking the phone to the call screen visible: about 1.4 s at first.

| Cost | Cause | Fix |
|---|---|---|
| 200-400 ms | modem port polled every 200 ms: a command written at a tick, read at the next | `modem_at.c`: written at once, read every 10 ms while awaited |
| 60 ms per wake | screen time saved on waking: the first write after resume waits for the eMMC | `battery.c`: saved 2 s later |
| 80 ms, first call | call screen's wallpaper darkened at 80% on first use (720x1440) | `call_screen_prepare()` while going to sleep |
| 245 ms | modem's USB controller waited, resuming in turn, for its PHY, clocks and OHCI companion, after the display's 250 ms | `S01resume`: those and the display `power/async` (`CONFIG_PM_ADVANCED_DEBUG`) |
| 420 ms | ehci-platform resets and powers the controller down in suspend: root hub 136 ms, modem's USB reset 286 ms, and its first RING lost | `0036-…` `ehci_platform.keep_powered=1` (extlinux.conf), `power/wakeup` keeps the PHY up: the modem resumes in 15 ms, its RING arrives |
| 51 ms | TCON's DSI pixel clock set again on resume: pll-mipi's rate search tries pll-video0's rates for each of 768 factors | `0037-…`: set only when it changed |
| lock screen, then the call over it | the kernel lights the backlight as it resumes the panel | `power.c`: brightness 0 while asleep, lit by phone-ui once the calls are known and the frame is on the panel whole (two extra 1-pixel frames: the flush waits for the previous flip, the panel then receives it over a refresh) |

Now: the kernel resumes in about 200 ms, all of it the display; phone-ui gets the RING
on waking and lights the screen on the call screen 55 ms later (85 ms for the first call
after phone-ui started: the GPU uploads the wallpaper and large fonts on first use).

### The display's 197 ms

`sun4i-drm display-engine` resume, split with temporary probes (below):

- panel `prepare`: supplies, 10-20 ms, reset released, 15-20 ms (about 40 ms)
- init sequence over DSI: 2.5 ms
- "sleep out" to "display on": 120 ms, a fixed sleep in `panel-sitronix-st7703.c`
- waiting for vblanks once enabled (`drm_atomic_helper_wait_for_vblanks`): 30 ms

The 120 ms is the controller's requirement, not a guess: the ST7703 datasheet
(files.pine64.org/doc/datasheet/pinephone/ST7703_DS_v01_20160128.pdf) says "It takes
120msec to become Sleep Out mode (booster on mode) after SLPOUT command issued" (6.2.11,
pp. 89-90) and its power-on sequence puts "120ms or more" between Sleep Out and Display
On (5.6, Figure 5.32, p. 52); 15 ms after releasing reset before any command (Table
7.8, p. 186). History: the driver had 60 ms; Ondrej Jirman (2020) and Frank Oltmanns
(2023, merged) set 120 ms after corrupted displays and flicker on the PinePhone after
resume, plus 120 ms after "sleep in" before cutting power (only on going to sleep).
Do not shorten it. What is left to try, within the spec: keeping the panel powered in
sleep-in through suspend (no supplies, reset, init: about 40 ms), or letting user space
run while the panel wakes (restoring the display off the resume path: deep, phone-ui's
first flip would block).

## Probing deeper

- **The kernel has no dynamic debug nor ftrace.** Temporary `pr_info("TIMING ...")`
  lines in the extracted tree (`output/build/linux-<ver>/`, copy the originals aside
  first), `just make linux-rebuild all`, `just flash`, a round, then copy the originals
  back and rebuild. Grep for `pr_info("TIMING`, not `TIMING`: the kernel uses the word.
  In `drm_atomic_helper_commit_tail_rpm()` they log every frame: remove them soon.
- **Kernel patches** in `external/patches/linux/` only apply when Buildroot extracts:
  to try one, apply it by hand to the extracted tree (`patch -p1`) and
  `linux-rebuild all`; it is then the same as after a dirclean.
- **Runtime switches first:** `power/async` and `power/wakeup` of a device,
  `/sys/module/<driver>/parameters/`, before rebuilding anything; `devmem` reads
  registers (the CCU's PLLs at 0x01C20000 + offset) to check that a change keeps them.
- **phone-ui:** temporary timed `printf`s (`log_time()`), pushed with `just ui` (20 s,
  no flash). Frames: LVGL's display events (`LV_EVENT_REFR_START`, `RENDER_START`,
  `RENDER_READY`, `FLUSH_START`, `FLUSH_FINISH`, `INVALIDATE_AREA` with the area) show
  each frame's layout, draw and flush, and what was redrawn when. `just test` refuses
  plain string literals passed to functions (the i18n check): `printf` formats pass.
- **What the user saw** is the last word: lit while a frame's flip had just started,
  the lower part still showed the lock screen; the logs said all was drawn.
- Test first all the same (the `phone-ui-test` skill): the host shows the order of
  things (the screen dark until the call shows, a command written on waking), the phone
  shows the time.

## Gotchas

- `lv_tick_get() | 1` (a "set" tick that is never 0) is one ahead of now on even ticks:
  `lv_tick_elaps()` of it in the same millisecond wraps to a huge value. Keep a flag.
- `just test <filter>` takes a substring, not a pattern; a worktree under `/tmp` cannot be
  tested (the sandbox mounts its own `/tmp`: tests reading the sources find nothing).
- Another session may work in the same tree and stage files: check `git status` (an
  `MM`) and `git diff --cached` before committing, stage only your hunks.
- Not measured yet: what `keep_powered` costs asleep (the phone stays on USB for SSH;
  the PMIC has no charge counter). A USB power meter on a fully charged phone shows the
  whole phone's draw.
