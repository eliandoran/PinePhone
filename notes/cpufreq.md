# CPU frequency: pinned, not schedutil (2026-11-07)

phone-ui pins the A64's frequency (`src/cpu.c`): 648 MHz (its lowest, 1.04 V) for the
interface, 1152 MHz (its highest, 1.3 V) while an app asks for it with `ui_cpu_boost()`:
Browser while open, Camera from the shutter to the photo saved, external apps with
`"cpu": "full"` in `app.json`. Doom and Genesis play at 648 MHz and go to 1152 for the rest
of the game once their engine thread is busy 85% of a second (`ui_cpu_boost_when_busy()`,
its CPU time against real time each second). `S90phone-ui stop` gives the whole
range back; `pp-update` runs at the highest and restores what it found.

## Why: frequency changes cost more than they save

schedutil may change the frequency every 366 µs here (`rate_limit_us`, 1.5 × the
`cpuinfo_transition_latency` of 244 µs). Each change runs the clock driver's PLL search
and a PMIC voltage write in `sugov`, a deadline-priority thread; the CPU runs on the
24 MHz oscillator while the PLL locks. Work that waits on hardware in small steps
(each SDIO command of the Wi-Fi driver, touchscreen interrupts) looks idle, then busy,
and the frequency changes all the time:

- **Wi-Fi** (RTL8723CS, rtw88 over SDIO): receiving, ~250 changes a second. 30 MB of
  random data over SSH to the phone, −55 dBm, 40 MHz channel, link at 135–150 Mbit/s:

  | Frequency | Upload to the phone | Download | Changes |
  |---|---|---|---|
  | schedutil, rate limit 366 µs | 1.25 MB/s | 2.5 MB/s | ~250/s |
  | schedutil, rate limit 10 ms | 2.6 MB/s | | ~30/s |
  | schedutil, rate limit 50 ms | 2.95 MB/s | | ~15/s |
  | pinned 648 MHz | 2.4–2.6 MB/s | 3.1–4.0 MB/s | 0 |
  | pinned 1152 MHz | 3.1–3.2 MB/s | 6.1–6.3 MB/s | 0 |

  Download is partly the phone's CPU (`head -c … /dev/urandom`). Under schedutil the CPU
  was mostly at 1008–1152 MHz anyway: the changes are the cost, not the speed.
  The SDIO bus (4 bits at 50 MHz, ~25 MB/s) moves the data in a third of the receive
  handler's time; the rest is small commands at ~120 µs each, 23 per interrupt.
- **Dragging** (earlier, `main.c`'s pin while touching): 15% of the CPU in sugov, about
  40 FPS instead of 60; replaying the touches without the touchscreen's interrupts ran at
  60. The menu scrolls at 66 FPS at 648 MHz, about half a core.

## What needs the highest

- Camera's preview keeps up at 648 MHz: 66 FPS, 34% CPU (30% at 1152). A rear photo
  (2592x1944 raw, debayered and encoded on the CPU) takes 720 ms at 648, 430 at 1152,
  after the raw frame's 275 ms either way.
- Doom, Genesis, 5 s each at full frame rate, the busiest thread (the engine) at 1152 / 648
  MHz: Doom II's demo 25% / 45% of a core, Streets of Rage standing in a level 28% / 55%,
  Dune (Sega CD) in a dialogue 39% / 68%. Calm scenes: a crowded one may need more, hence
  the boost on demand.
- Browser: WebKit's processes, not measured at 648.

## Not done, and dead ends

- Battery: not measured. 648 MHz is also where schedutil idled; this A64 has no
  cpuidle states, so the voltage is what idles.
- rtw88's RX aggregation for the 8703b (`size 0x6, timeout 0x6` in `sdio.c`): 0x20
  changed nothing. Reading 16-bit registers with one CMD53 (`sdio_readw`) instead of two
  CMD52s wedged the chip (`Failed to read … from SDIO port: -12`, then "mac power on
  failed" until a reboot).
- Published PinePhone figures (linux-wireless, January 2025, weak signal): rtw88 14 Mbit/s
  after "rtw88: 8703b: Fix RX/TX issues" (in our kernel), the vendor driver 23.6; a
  Pinebook with the same chip 30–50 Mbit/s.
