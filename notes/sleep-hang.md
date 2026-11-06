# Frozen after waking with Wi-Fi connected: investigation notes

Status: **cause found 2026-11-06 afternoon: rtw88 (Wi-Fi) resuming inside the kernel's resume;
worked around in phone-ui** (`wifi_sleep()`, settings/wifi.c: wlan0 down before sleeping, up
after waking, as the Wi-Fi switch does). With the driver unloaded the first sleep after boot,
which froze every time, woke fine; Wi-Fi switched off had the same effect the night before.
What caught it is in "Catching it" at the end. The rtw88 bug itself is not understood yet
(worth reporting upstream).

Earlier status: **seen three times on 2026-11-06 (lit lock screen, no response), no log caught.** Once in
the morning, then twice around 10:40, each soon after a boot or a phone-ui restart, once in deep
sleep and once in s2idle: not specific to deep sleep. Not reproduced with the logging below armed
(it slows resume down: possibly a race it hides). Kernel 7.1.13, the image's rtw88 except where
noted. Since 2026-11-06 phone-ui sleeps with `mem`, deep sleep through Crust (see "Deep sleep").

Read the later sections first: "Later findings" corrects parts of the technique below (pstore was
never mounted, the watchdog runs during sleep, USB not coming back looks like a hang from the PC).

## Symptom (2026-11-06, morning)

Wi-Fi connected, Power pressed (phone-ui: fade out, lock screen, `freeze` into
`/sys/power/state`, `power.c`), then Power again: the display came back lit, showing the lock
screen, and nothing responded. From the PC:

- The USB gadget was still enumerated (ACM on COM37 and NCM). The serial port took writes, but
  no getty answered, not even a login prompt.
- NCM: Windows got only a link-local 169.254.x address, so udhcpd was not answering.
- Wi-Fi (192.168.0.18): no ARP reply.

Reading of it: the lit panel means the display's resume had run (the kernel turns the backlight
on with the panel, `power.c`'s comment). User space restarts only after every device has resumed,
so a driver whose resume never returns freezes phone-ui, getty and udhcpd together, with the last
frame still on the panel. A kernel fully locked up would look the same from outside. No log: the
kernel console is on the headphone UART only, and a long Power press cuts power through the PMIC,
which also clears ramoops (see "Testing technique").

State at the time, which may matter:

- **The debug rtw88 modules, loaded by hand from `/tmp/wifimods`** (`CONFIG_RTW88_DEBUG`,
  debugfs, `debug_mask` set), with patch 0033 applied by hand ([wifi-drops.md](wifi-drops.md)),
  and the loggers `thermlog.sh`/`wifiwatch.sh` reading debugfs every few seconds.
- Asleep for an unknown time, probably longer than the tests below.
- Woken by Power (axp803, IRQ 56).

## Earlier traces (2026-11-04, an earlier session)

`/var/lib/sleeptest.d/` on the phone, scripts not kept:

- `log-deep`: a **deep** sleep with the Wi-Fi driver bound was refused:
  `rtw88_8723cs: Failed to host PM flag MMC_PM_KEEP_POWER`, `pm_generic_suspend returns -22`.
  `rtw_sdio_suspend()` (`sdio.c`) only asks the host to keep the card powered. The device tree has
  `keep-power-in-suspend` on `&mmc1` now (`sun50i-a64-pinephone.dtsi`), and s2idle suspends
  rtw88 fine (below). Deep sleep is not used anyway: it resets the phone.
- `log-wifi`, `log-wifi-core`: they stop after their start lines, with no `rc=`, where the other logs
  have one. Possibly the same hang, possibly the test was interrupted; nothing says which.

## Tests (2026-11-06), none froze

All with the image's rtw88 (after the forced power-off), the logging and the watchdog below.

| Run | Wake | Result |
|---|---|---|
| Script, Wi-Fi stopped (`S15wifi stop`) | RTC, 5 s | suspended 341.0 s, back 349.1 s |
| Script, Wi-Fi connected | RTC, 5 s | suspended 559.9 s, back 568.0 s, still `COMPLETED` |
| phone-ui, Power, several times (3 kept in dmesg) | Power, ~3–5 s | all woke, reassociated within 2 s |

Timings with Wi-Fi connected: `wiphy_suspend` 145–487 ms (deauthenticates with
`DEAUTH_LEAVING`, stops the chip), rtw88's own `pm_generic_suspend`/`resume` ~1 µs (it only
sets `MMC_PM_KEEP_POWER`), `wiphy_resume` 218–460 ms (powers the chip on, firmware again).
Device resume takes 2.6–2.9 s in all, most of it the modem's USB (`usb 1-1` ~350 ms, its
`ttyUSB*` re-created) and the display (~260 ms).

Seen on the way, separate issues:

- After the first test's RTC sleep, the PC saw the phone as **charger only**, with no gadget,
  until it was replugged. The Power-woken sleeps later kept USB. Not looked into.
- `rtw88_8723cs: failed to get tx report from firmware` at 463 s, between tests: the usual drop
  pattern ([wifi-drops.md](wifi-drops.md)).

## Testing technique

The aim: a hang should leave a log that survives the reboot, with the device that hung named.

1. **Every device's callback timed**: `pm_print_times` logs "calling …" and "returned … after N
   usecs" for each device, so the last "calling" without a "returned" is the stuck one.
   `pm_debug_messages` adds the phases and the wakeup IRQ (`CONFIG_PM_DEBUG`).
2. **The console kept printing during suspend**: `console_suspend` N (printk module parameter,
   writable at runtime) and `dmesg -n 8`, so these lines reach the consoles, ramoops among them.
3. **The watchdog as the way out**: `watchdog -T 16 -t 2 /dev/watchdog` (BusyBox; sunxi's maximum
   is 16 s), fed while user space runs. If resume hangs, user space never comes back and the
   watchdog resets the SoC. That reset keeps the RAM, so the console is in **`/sys/fs/pstore`**
   on the next boot (`CONFIG_PSTORE_RAM`, `PSTORE_CONSOLE`; `linux.fragment`). A long Power press,
   or a kernel restart through Crust, restarts the PMIC and loses it. Catch: nothing feeds it
   while asleep, so a sleep longer than about 14 s may be reset in its sleep, which looks
   like a hang. Wake within 10 s, or use the RTC.
4. **Waking without hands**: the RTC alarm (`/sys/class/rtc/rtc0/wakealarm`, `+5`; wakes s2idle,
   IRQ 15), so the script runs detached and is read back after.
5. Logs on the data partition (`/var/lib/sleephang/`), which survives the reset; the script runs
   with `setsid` so SSH dropping with the USB gadget does not kill it.

Script (`/tmp/sleeptest.sh` on the phone, `wifi` or `nowifi`):

```sh
#!/bin/sh
L=/var/lib/sleephang
mkdir -p $L
N=$(date +%s)
exec >>$L/run-$N-$1.log 2>&1
echo "=== $1 $(date)"
if [ "$1" = nowifi ]; then
	/etc/init.d/S15wifi stop
	sleep 3
fi
wpa_cli -i wlan0 status | grep -E 'wpa_state|ssid'
dmesg -n 8
echo N > /sys/module/printk/parameters/console_suspend
echo 1 > /sys/power/pm_print_times
echo 1 > /sys/power/pm_debug_messages
watchdog -T 16 -t 2 /dev/watchdog
sleep 1
sync
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo +5 > /sys/class/rtc/rtc0/wakealarm
echo "suspending at $(cut -d' ' -f1 /proc/uptime)"
sync
echo freeze > /sys/power/state
echo "write status $?, resumed at $(cut -d' ' -f1 /proc/uptime), wakeup irq $(cat /sys/power/pm_wakeup_irq)"
killall watchdog
echo 0 > /sys/power/pm_print_times
echo 0 > /sys/power/pm_debug_messages
dmesg -n 5
sleep 10
wpa_cli -i wlan0 status | grep wpa_state
dmesg > $L/dmesg-$N-$1.txt
[ "$1" = nowifi ] && /etc/init.d/S15wifi start
echo done
```

For phone-ui's own path (Power to sleep and to wake), run the same settings and the
`watchdog` line by hand, without the sleep, then press Power. `killall watchdog` stops it
cleanly (BusyBox writes the magic 'V'); afterwards set the two `pm_*` files back to 0 and run `dmesg -n 5`.
`pm_print_times` fills the ring buffer fast: copy `dmesg` after each sleep.

Gotchas:

- Uploading over `wsl.exe -e ssh … < file` gives an empty file: stdin does not reach it.
  base64 the file into the command line instead (`echo $B | base64 -d > /tmp/x`).
- `/var/lib/sleeptest` is the earlier session's file: hence `sleephang`.
- No watchdog sysfs (`CONFIG_WATCHDOG_SYSFS` off): its timeout cannot be read back.
- After a sleep, USB may come back as charger only (above): Wi-Fi SSH (192.168.0.18, the key from
  WSL) still reaches the phone.

## Later findings (2026-11-06, 10:30–12:10)

### Deep sleep works (plugged in)

`/sys/power/mem_sleep` reads `s2idle [deep]`: Tow-Boot's Crust is there. `echo mem >
/sys/power/state` with an RTC alarm (+10 s): CPUs 1–3 killed through PSCI, timekeeping
suspended (dmesg's clock stops), woken by IRQ 15 after 12 s of wall clock, no device errors,
Wi-Fi reassociated. Suspend of devices 214 ms, resume of the display 273 ms, `wiphy_resume` 237 ms.
`CLOCK_BOOTTIME` counts the sleep (from the RTC), so phone-ui's "asleep" time still works. Only
tested on USB power: the old "deep sleep resets the phone" (power.c) may have been on battery.
phone-ui now writes `mem` (the board's kernel picks deep or s2idle through `mem_sleep`;
`mem_sleep_default=s2idle` on the kernel command line goes back to s2idle).

Woken by Power from deep sleep (`just ui` build), it froze once, lit; then the image's phone-ui
(s2idle) froze the same way: the hang is not deep sleep's.

### pstore was never mounted

`/sys/fs/pstore` is only a mount point: nothing mounted it, so it always looked empty, and the
records of every watchdog reset were there all along. `mount -t pstore pstore /sys/fs/pstore`
showed the previous boot's console (1049 lines, "ECC: No errors detected"). Fixed in the image:
inittab mounts it, S99bootlog appends it to the early snapshot of `boot-N.txt`. Each boot replaces
the previous boot's records: read them before restarting again.

### The watchdog runs during s2idle

A 30 s RTC sleep with the watchdog armed reset the phone about 16 s in: pstore ended at
`PM: suspend-to-idle`. So with the watchdog armed, sleeps longer than ~14 s end in a reset, a
"hang" that is not one. But once the phone stayed black for an hour (asleep after an injected
press without an RTC alarm, or stuck) and the watchdog did not reset it: unexplained.

### USB does not always come back after a sleep

After a wake, the PC may see the phone as **charging only** (no gadget) until replugged. Then SSH
over USB times out while the phone is fine, and Wi-Fi (192.168.0.18) drops on its own
([wifi-drops.md](wifi-drops.md)): several "unreachable, so frozen" readings this afternoon were
probably this. Check the screen before concluding anything. A problem of its own (MUSB resume,
the ANX7688 after sleep), not looked into.

### Boot-time wrapper (used, then retired by the flash)

To arm the logging from boot (the freezes came after reboots): `/var/lib/phone-ui/dev/phone-ui`
as a shell script that mounts pstore, copies its records to `/var/lib/sleephang/pstore-of-bootN-*`,
sets the logging, starts `watchdog -T 16 -t 2`, then `exec /usr/bin/phone-ui "$@"`, with
`dev/base` holding `sha256sum /usr/bin/phone-ui`: S90phone-ui runs it as a development build
until a flash changes the binary.

### Injecting Power presses

`/var/lib/sleephang/pressloop.sh ROUNDS SECONDS`: writes `struct input_event`s (24 bytes on
arm64: timeval, type 1, code 116, value; then a SYN) to `/dev/input/event0` (axp20x-pek), sets an
RTC alarm, waits for phone-ui's "awake" line. A release while the key is up is dropped by the
input core (no state change). The loop's presses did not make phone-ui sleep (twice, not
understood); a one-off press and release from a script seemed to. Unfinished.

## Next, if it happens again

- Look at the screen first: lit and stuck is the hang; black may be asleep; a phone that answers
  on Wi-Fi but not USB is the USB problem.
- Leave it untouched for 20 s if the watchdog was armed, then read `/sys/fs/pstore` (mounted since
  the inittab change; `boot-N.txt` has it too) after the reset. Without the watchdog, only the headphone UART (115200) shows the console: worth
  keeping the cable on during Wi-Fi debugging.
- **Arm it whenever experimenting**: while debug modules or loggers are loaded, keep the
  settings above and the watchdog running, so the next occurrence leaves a log.
- Repeat the Power test with the debug modules from `/tmp/wifimods` loaded and `thermlog.sh`
  running, as at the time. Then many cycles rather than long sleeps: the watchdog caps a
  sleep at about 14 s, so loop the script's RTC sleep (5 s) a few hundred times. A long sleep
  needs the watchdog off and the UART cable on.
- Candidates for the hang, from the code, untested: `wiphy_resume` → mac80211's reconfig →
  `rtw_core_start()`, powering the 8703b on again (rtw-next has fixes for this chip's power-on
  after a card disable, commits 43f6347e8bbe and 73e3b1c94c7d, [wifi-drops.md](wifi-drops.md));
  debugfs readers (the loggers) racing the chip's power-off; SDIO IRQ during resume.

## Catching it (2026-11-06, 12:15–12:45)

Reproducible on the first sleep after every boot (deep or s2idle), but not with the per-device
logging (`pm_print_times`, `dmesg -n 8`), which slows resume down.

- **Kernel console on the USB gadget** (`CONFIG_U_SERIAL_CONSOLE`, `console=ttyGS0` before
  `console=ttyS0`, `no_console_suspend`; a PowerShell logger on the host reopening the COM port):
  nothing after the suspend. The gadget only prints once MUSB has resumed, so it cannot see what
  comes before that, and nothing came after either.
- **Hung task, soft and hard lockup panics, then the watchdog** (`hung_task_timeout_secs` 5,
  `hung_task_panic`, `watchdog_thresh` 4, `softlockup_panic`, `hardlockup_panic`, all CPUs'
  backtraces; `kernel.panic` 0; `watchdog -T 16`): the freeze restarted the phone and pstore had
  the console, ending with MUSB's message on resume (`ULPI bus control register`, 0.42 s after the
  one on suspend), and no panic: every CPU stuck with interrupts off (all detectors run off timer
  interrupts), or the like. The backlight resumes about 70 ms after MUSB and the screen was lit:
  the hang is in the devices resumed in parallel after it (rtw88 among them: mmc1, wiphy_resume).
- **No cpuidle driver** (CPUs only WFI, the broadcast timer unused): not lost timer wakeups.
- **Bisection by unloading**: `S15wifi stop`, `rmmod rtw88_8723cs rtw88_8703b rtw88_8723x
  rtw88_sdio rtw88_core`, then the first sleep after boot woke fine. Wi-Fi.

The watchdog counts while the phone sleeps, deep sleep included: a 30 s sleep was reset at
~16 s in s2idle, and again in deep sleep (14:53, pstore kept: a watchdog reset). The black phone
that stayed black for an hour was something else. So a watchdog armed at every boot would
restart every sleep longer than ~14 s: it would need sunxi_wdt to stop on suspend and start
again early in resume (a kernel patch, judged not worth it on 2026-11-06).

Kept from this: pstore mounted at boot and kept in `boot-N.txt`. The USB console and the 30 s hung
task timeout were reverted after the fix (to bring back from the list above when needed).

Next: the mechanism in rtw88 (mac80211's suspend stops the driver, its resume restarts it:
`rtw_core_stop()`/`rtw_core_start()` inside dpm callbacks, vs. the same through `ip link` which is
fine); try rtw-next's 8703b power fixes; report to linux-wireless.
