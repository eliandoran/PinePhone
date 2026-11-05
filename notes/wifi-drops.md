# Wi-Fi drops (RTL8723CS, rtw88): investigation notes

Status: **worked around**: Wi-Fi power save is the trigger, off by default since 2026-11-06
(`# CONFIG_CFG80211_DEFAULT_PS is not set` in `linux.fragment`; see "Cause: power save" below).
Kernel 7.1.13, wpa_supplicant 2.12, rtw8703b firmware 11.0.0 from linux-firmware 20260810.

## Symptom

In **episodes** lasting minutes, Wi-Fi is unusable: it reconnects every 5–15 s, pings to the
router lose 60–80 %, and reconnecting often times out. Between episodes there are long periods
(15+ min seen) with 0 % loss, where 100 MB each way over SSH goes through without a hiccup
(~10 Mbit/s, probably SSH crypto bound). Signal stays strong throughout (−28 to −40 dBm).

The night of 2026-11-04 was "completely unusable"; on 2026-11-05 it happened right after boot
(boots 159–161, 164) and in several episodes during the morning.

Network: SSID `AAA`, BSSID `72:7f:f0:3b:48:33`, channel 10 (2457 MHz), **40 MHz with the
secondary channel below** (center 2447 MHz, i.e. channels 6–10), WPA2-PSK + SAE transition mode
(H2E advertised), PMF optional. A strong neighbor AP is on channel 13 (`ZoeAlexandra`, −36 dBm).
The PC is on the same router's 5 GHz band (fine); the RTL8723CS is 2.4 GHz only.

## Mechanism (established)

From wpa_supplicant's debug log (`-d -t`) during an episode:

```
nl80211: Beacon loss event                      (NL80211_CMD_NOTIFY_CQM)
wlan0: CTRL-EVENT-BEACON-LOSS
nl80211: Delete station 72:7f:f0:3b:48:33
wlan0: Deauthentication notification
wlan0:  * reason 4 (DISASSOC_DUE_TO_INACTIVITY) locally_generated=1
wlan0: CTRL-EVENT-DISCONNECTED bssid=72:7f:f0:3b:48:33 reason=4 locally_generated=1
```

and in the kernel log, just before: `rtw88_8723cs mmc1:0001:1: failed to get tx report from firmware`.

1. mac80211 stops receiving the AP's beacons (beacon loss).
2. It probes the AP with a frame that requests TX status; the firmware does not report it.
3. mac80211 drops the connection itself (reason 4, locally generated). **The kernel logs no
   disconnect line** for this: dmesg only shows a new `authenticate with` out of nowhere.
4. Reconnecting: auth frames need 2–3 tries or time out (`authentication with … timed out`):
   the AP's answers are not received.

The firmware beacon filter is **not** involved: rtw8703b firmware has the legacy header
(`b2 03 …`), so `fw->feature` is 0, `FW_FEATURE_BCN_FILTER` is off and mac80211 monitors
beacons in software. So during an episode the AP's frames really do not reach the host
(air, radio, firmware or the SDIO RX path).

Other driver messages seen around episodes: `[IQK] TX timed out after ~110000us!`,
`firmware failed to leave lps state` (once, boot 141), `rx drop misc` increasing in
`iw station dump`.

Note: two `authenticate with` lines ~40 ms apart at every connect are normal: SAE commit and
confirm.

## History from the boot logs

`/var/lib/bootlog/boot-N.txt` (only the first ~45 s of each boot):

- Boots ≤ 127: no Wi-Fi firmware (`rtw8703b_fw.bin` missing).
- Boot 133 (2026-11-04 21:45 local), the **first boot that ever associated**, already shows
  reconnects within 45 s; so do nearly all later boots. The problem was there from the start.
- Bluetooth firmware (`rtl8723cs_xx_fw.bin`, commit 4c7bd23) arrived at boot 154. Reconnects
  happen both before and after it.

## Phone, not air (2026-11-05 evening)

During an episode that had lasted 3+ minutes (every auth attempt timing out, `TEMP-DISABLED`):

- A scan still listed the AP at −28 dBm: the radio receives beacons.
- Another MAC address (`ip link set wlan0 address …`) timed out the same way: the router
  is not ignoring this client.
- Reloading the driver (`modprobe -r rtw88_8723cs; modprobe rtw88_8723cs`, then wpa_supplicant)
  connected in 3 s with 0 % ping loss.

So it looked like a stuck state that only a reset clears, but the drops came back 100 s after
that reload: the reload may only have coincided with the end of an episode. The scan and the
other MAC still point at the phone rather than the router.

Candidate mechanism, from the code: in `sdio.c`, `rtw_sdio_write_port()` returns `-EBUSY` when
`REG_SDIO_FREE_TXPG` reports too few free pages; `rtw_sdio_process_tx_queue()` puts the frame back
at the head, the TX work gives up after 1000 tries and nothing re-kicks it or stops mac80211's
queues. If the chip stops freeing pages (firmware stuck, e.g. in LPS), every frame (auth, probes,
nullfunc) waits there and never gets a TX report, while RX goes on. A similar wedge on 8821ce
(PCI, TX ring read index frozen, queue never woken after a ring reset) was reported on
linux-wireless in 2026-08 ("[BUG] rtw88 8821ce: connection wedges (100% loss until reboot)",
https://ratatoskr.run/linux-wireless/2026/08/17465622/t); there power save was ruled out.

To check it: `CONFIG_RTW88_DEBUG`, `RTW88_DEBUGFS`, `CFG80211_DEBUGFS` and `MAC80211_DEBUGFS`
(added to `linux.fragment` for the test, not kept; modules swapped in from `/tmp`), and `/var/lib/wifilog/wifiwatch.sh`: `debug_mask` 0x40000 (RTW_DBG_SDIO, logs
"Not enough free pages"), then on each auth timeout or missing TX report a snapshot into
`snap.log` of `REG_SDIO_FREE_TXPG` (0x10250020: high, normal, low, public pages, one byte each),
`REG_TXPAUSE` (0x522), mac80211's `queues` (stop reasons), the station flags and `dmesg`.
Baseline while connected: FREE_TXPG `0xe702020c` (public 231, low 2, normal 2, high 12),
TXPAUSE 0, queues not stopped.

Result (snapshot of an auth timeout at 19:53): FREE_TXPG `0xe702020c`, all pages free, and
queues stopped only for OFFCHANNEL (0x40, the scan). **Page starvation is ruled out.**

## Cause: power save (2026-11-06)

A day after patch 0032 (below) was in the image, the drops were back: from boot it associated
and dropped every 5–8 s, with `failed to get tx report from firmware`. Toggling power save at
runtime, one minute of pings each:

| Power save | Result |
|---|---|
| off | 60/60 pings, no reconnect |
| on | most lost, reconnecting |
| off | one reconnect (~12 s to recover), then clean |
| on, `disable_lps_deep=Y` (rtw88_core, writable at runtime) | still dropping, 8 auth lines |

So plain LPS is enough to break it; deep LPS is not the culprit. Turning it off does not unstick a
connection that is already stuck: it takes a reconnect, sometimes 20–30 s. That is why the test
in "Ruled out" below looked like no change.

Fix: `# CONFIG_CFG80211_DEFAULT_PS is not set` (cfg80211's default for every interface), flashed
2026-11-06: power save off at boot, connected on the first try, 40/40 pings.

Others: no rtw88 report for the RTL8723CS (postmarketOS still uses the vendor `8723cs` driver),
but the same family has it. UBports turned power saving off in the vendor driver on the PinePhone
in 2020; the RTL8723BS rtw88 series reports `firmware failed to leave lps state` on SDIO; 8821ce
and 8822ce users work around drops with `power_save off` (and found `disable_lps_deep` useless);
a 2022 patch turned power save off for rtw88's USB devices, but 7.1.13 offers it to every rtw88
device again (`SUPPORTS_PS` in `main.c`). Worth reporting to linux-wireless (Ping-Ke Shih, Fiona
Klute).

Cost: not measured. The only PinePhone figure found is the whole Wi-Fi module at ~100 mW (1.45 W
with the vendor driver unloaded, 1.55 W connected, at the wall, forum.pine64.org tid 14250); what
power save would save is a part of that. To measure: on battery (USB unplugged, SSH over Wi-Fi),
`/sys/class/power_supply/axp20x-battery/current_now`, idle with power save off, then on.

## rtw8703b's MAC left at 20 MHz on a 40 MHz channel (patch 0032, not the cause)

This was taken for the cause on 2026-11-05 after a 5-minute test; the drops came back the next
day with the patch in the image (see above). The registers it sets do match the vendor driver,
rtlwifi and rtl8xxxu.

`phy_info` (rtw88 debugfs) while connected: `OFDM cnt (ok, err) = (0, 16)`, no HT frame
received, only CCK beacons. Auth retries came ~1 s apart: the auth frame's TX status said
"not acked" or came late.

The baseband is in 40 MHz with the right sideband (0x800/0x900 RFMOD 1, 0xa00 CCK sideband,
0xd00 bits 11:10, 0x818 bits 27:26, all as the vendor driver sets them for primary upper). The MAC
is not: `rtw_set_channel_mac()` writes the newer chips' registers, and for this generation the
vendor driver, rtlwifi (`rtl8723be/phy.c`) and rtl8xxxu (`core.c`) also clear `BW_OPMODE_20MHZ` in
`REG_BWOPMODE` (0x603) and set the response sideband in `REG_RRSR` bits 22:21 (0x442 `<< 5`).
On the phone 0x603 was `0x04` (20 MHz) and the RSC bits 0. `rtw_phy_rrsr_update()` also rewrote
the whole of `REG_RRSR` every 2 s, so a fix there has to keep the RSC bits.

Patch `0032-…` sets both in `rtw8703b_set_channel()` and keeps RSC in `rtw_phy_rrsr_update()`.
Tested by loading the patched modules (2026-11-05 19:58): 0x603 `0x00`, `REG_RRSR` `0x0040015f`,
HT MCS7 frames received, then 596 of 600 pings (one every ~0.5 s, 5 min), 0 reconnects,
0 beacon loss. Too short: episodes come and go on their own. Whether the patch changes anything
at all is unknown (the "OFDM ok 0" reading may have been taken with the chip in power save).
rtw8723d (RTL8723DS) shares `rtw_set_channel_mac()` and probably has the same bug (not
checked on hardware). Worth reporting upstream (linux-wireless, Ping-Ke Shih, Fiona Klute).

## Ruled out (each tested during an active episode)

| Variable | Test | Result |
|---|---|---|
| Wi-Fi power save (LPS) | `iw dev wlan0 set power_save off`, watched 2 min | 27 reconnects in 2 min: no change. **Wrong conclusion**: off does not unstick an episode already under way, but power save is the trigger (see "Cause: power save") |
| Bluetooth coexistence | unbind `hci_uart_h5` (`echo serial0-0 > /sys/bus/serial/drivers/hci_uart_h5/unbind`), watched 90 s | still looping, 80 % ping loss |
| Load | 100 MB each way over SSH (PC → 192.168.0.18), Maps with the tile cache emptied (24 tiles fetched) | no drop, outside an episode |

An earlier Bluetooth unbind (this morning) was followed by an instant recovery: that was a
coincidence (the next episode came with Bluetooth bound or unbound alike).

## Not tested yet

- **20 MHz instead of 40 MHz.** No way found from the phone:
  - `wpa_cli set_network 0 disable_ht40 1` → `FAIL`: wpa_supplicant built without
    `CONFIG_HT_OVERRIDES`.
  - `cfg80211_disable_40mhz_24ghz=Y` (cfg80211 module parameter): no effect, even after
    reloading `rtw88_8723cs`. It only applies to drivers using cfg80211's connect API, not to
    mac80211 drivers with wpa_supplicant's SME.
- **WPA2 only (no SAE)**: tried once, but while the AP was not answering at all, so inconclusive.
- **Another 2.4 GHz device next to the phone during an episode**: separates "phone" from "air
  or router". Not done.

## Hypotheses

| | Hypothesis | For it |
|---|---|---|
| A | rtw88 / firmware: RX path stalls, TX reports missing | `failed to get tx report`, `[IQK] TX timed out`, `failed to leave lps state`; known SDIO issues on this chip family |
| B | 2.4 GHz interference made worse by 40 MHz | 40 MHz on 6–10 next to a strong neighbor on 13; episodic; worse at night |
| C | the router's 2.4 GHz side | indistinguishable from B without a second device |

## Upstream research (2026-11-05)

| Report | Fix | In our 7.1.13? |
|---|---|---|
| PinePhone RTL8723CS: random disconnects after beacon loss (Fiona Klute: "rare instead of a frequent nuisance" with the fix) | SDIO indicates TX status for `IEEE80211_TX_CTL_REQ_TX_STATUS` (`rtw_tx_report_enqueue`) | **yes**, `sdio.c:1213` |
| `failed to get tx report from firmware` on RTL8723D/8821A/8812A, "probably RTL8703B": firmware can't keep up with TX-status requests for QoS Null | commit 737e980e (`ieee80211_is_any_nullfunc` in `rtw_tx_pkt_info_update()`) | **yes**, `tx.c:429` |
| RTL8822CU 2022: spurious `BCN_FILTER_CONNECTION_LOSS` after associating (cd96e22bc1da) | follow-up patch | n/a: 8703b firmware has no beacon filter |
| `firmware failed to leave lps state` leaves the chip unresponsive | `rtw_fw_recovery()` in `ps.c` (Kalpesh Solanki, v2 2026-11-03) | no, not merged |
| TX report timeout 500 ms too short under load and scans | `RTW_TX_PROBE_TIMEOUT` → 2500 ms (Luka Gejak) | no; not merged, maintainers want it for RTL8723DU (USB) only |
| RTL8723CS: MAC power-on fails after a warm reboot | 8703b card-disable repair, commits 43f6347e8bbe, 73e3b1c94c7d (rtw-next, likely 7.3) | no, but different symptom |
| SDIO RX interrupt storm on 3081-based chips | v8 series (Alastair D'Silva, 2026-11) | n/a: RTL8723CS is an 8051-based chip |

postmarketOS still ships the vendor `8723cs` staging driver for the PinePhone; switching to
rtw88 is an open work item. No report found of exactly this pattern (episodic beacon loss with
the fixes above present).

Sources:
- https://lists.linaro.org/archives/list/linux-stable-mirror@lists.linaro.org/message/MKWWANIUJUJSD2JYYCHFT3XMLORHU4NO (SDIO beacon-loss fix; 404 when fetched)
- https://lists.linaro.org/archives/list/linux-stable-mirror@lists.linaro.org/message/OCS5U3EXTRF4KTWA6BKYMWNNA4WQ2K2Y (PinePhone regression from its backport; 404 when fetched)
- https://lkml.iu.edu/hypermail/linux/kernel/2504.0/10112.html (SDIO TX status for management frames)
- https://patchew.org/linux/20260420132314.1023554-274-sashal@kernel.org/mbox (737e980e)
- https://patchew.org/linux/20260501150402.227788-1-luka.gejak@linux.dev/ (TX report timeout)
- https://ratatoskr.run/linux-wireless/2026/11/17849249 (LPS leave recovery)
- https://ratatoskr.run/lkml/2026/09/17554385/t (8703b card-disable repair)
- https://ratatoskr.run/lkml/2026/11/17867549/t (SDIO RX interrupt storm)
- https://lore.kernel.org/linux-wireless/20221122145527.GA29978@pengutronix.de/ (2022 beacon filter regression)
- https://gitlab.postmarketos.org/postmarketOS/pmaports/-/work_items/3420 (pmOS: switch to rtw88)
- https://gitlab.com/postmarketOS/pmaports/-/issues/2512 (pmOS: WPA3/SAE with iwd)
- https://lkml.iu.edu/hypermail/linux/kernel/2501.0/01961.html (8703b RX/TX fix, PinePhone iperf)

## Plan (one variable at a time)

0. **Baseline recording that survives reboots.** Turn the ad-hoc loggers below into an init
   script (development only, like S99bootlog): wpa_supplicant with `-d -t -f`, kernel Wi-Fi
   lines, `iw station dump` every 10 s, and a ping check that logs each episode's start and
   end. Measure episodes per hour with nothing changed.
1. **Phone or air?** During an episode, check a second device pinned to the router's 2.4 GHz
   side. Affected too → B/C; only the PinePhone → A.
2. **If B/C:** router 2.4 GHz to 20 MHz, then another channel (1 or 6); compare episode rates
   with step 0.
3. **If A:**
   - rtw88 debug output during an episode (`/sys/module/rtw88_core/parameters/debug_mask`).
   - Cherry-pick the LPS-leave recovery patch.
   - wpa_supplicant with `CONFIG_HT_OVERRIDES` to test 20 MHz from the phone.
   - Compare with the vendor `8723cs` driver (as postmarketOS).
   - Report to linux-wireless (Ping-Ke Shih, Fiona Klute) with the logs above.

## Resuming: commands and gotchas

Live recording used on 2026-11-05 (until reboot, files kept in `/var/lib/wifilog/`):

```sh
# wpa_supplicant with a debug log; wait for the old one to exit first, or it fails with
# "Register frame command failed … -114" and exits
/etc/init.d/S15wifi stop; sleep 3
wpa_supplicant -B -d -t -f /var/lib/wifilog/wpa.log -i wlan0 -D nl80211 \
	-c /var/lib/wpa_supplicant/wpa_supplicant.conf
while [ ! -S /run/wpa_supplicant/wlan0 ]; do sleep 0.2; done
wpa_cli -B -i wlan0 -a /etc/wpa_action.sh
# station counters every 10 s (setsid: start-stop-daemon -x /bin/sh refuses a second sh)
setsid sh /var/lib/wifilog/stats.sh </dev/null >/dev/null 2>&1 &
```

Reading the log: `grep -vE "SIGNAL_POLL|recv command|hexdump" wpa.log` (phone-ui polls the
signal every second). Timestamps are Unix time.

wpa_supplicant's events without its debug log: `(echo "level 2"; sleep 60) | wpa_cli -i wlan0`.

Episode check: `for i in $(seq 60); do ping -c1 -W1 192.168.0.1 >/dev/null && printf . || printf X; sleep 1; done`.

### Driver debugging without flashing

rtw88's debug output and debugfs, and mac80211's, are off in the image. To get them, add to
`board/pinephone/linux.fragment`:

```
CONFIG_RTW88_DEBUG=y
CONFIG_RTW88_DEBUGFS=y
CONFIG_CFG80211_DEBUGFS=y
CONFIG_MAC80211_DEBUGFS=y
```

then `just make linux-reconfigure` (no `all`: only the kernel; they only change modules, so the
running kernel takes them). Load them from `/tmp` until the next reboot (the rootfs is read-only;
the phone's SSH has no SFTP, so `scp -O`):

```sh
M=output/target/lib/modules/7.1.13/kernel
scp -O -i keys/pinephone $M/net/wireless/cfg80211.ko $M/net/mac80211/mac80211.ko \
	$M/drivers/net/wireless/realtek/rtw88/*.ko root@172.16.42.1:/tmp/wifimods/
# on the phone
cd /tmp/wifimods; /etc/init.d/S15wifi stop; sleep 3
rmmod rtw88_8723cs rtw88_8703b rtw88_8723x rtw88_sdio rtw88_core mac80211 cfg80211
for m in cfg80211 mac80211 rtw88_core rtw88_sdio rtw88_8723x rtw88_8703b rtw88_8723cs; do insmod $m.ko; done
rm -f /run/udhcpc.wlan0.pid; /etc/init.d/S15wifi start
```

For a change to rtw88 only, swap the `rtw88_*` modules and leave cfg80211 and mac80211 loaded.
Each reload makes a new `phyN`: use `ls -d /sys/kernel/debug/ieee80211/phy*/rtw88`.

What it gives (`R=…/phyN/rtw88`, `P=…/phyN`):
- `cat $R/phy_info`: IGI, RSSI, received rates by modulation, CCA and false alarms, CCK/OFDM/HT
  ok and error counts (OFDM ok 0 is what showed the 40 MHz bug).
- `echo "0x603 4" > $R/read_reg; cat $R/read_reg`: any register; `0x1025xxxx` are SDIO local ones.
- `cat $P/queues`: mac80211's queue stop reasons (bit 6, 0x40, OFFCHANNEL is a scan).
- `echo 0x00040000 > /sys/module/rtw88_core/parameters/debug_mask`: `RTW_DBG_SDIO` (others in
  `rtw88/debug.h`), in `dmesg` at debug level.

Capture script used on 2026-11-05 (`/var/lib/wifilog/wifiwatch.sh`, start it with
`setsid sh /var/lib/wifilog/wifiwatch.sh </dev/null >/dev/null 2>&1 &`): a snapshot into
`snap.log` at start, then on each auth timeout, missing TX report or TX back-pressure, at most once a
minute.

```sh
#!/bin/sh
LOG=/var/lib/wifilog/snap.log
R=$(ls -d /sys/kernel/debug/ieee80211/phy*/rtw88 2>/dev/null | head -1)
P=${R%/rtw88}

echo 0x00040000 > /sys/module/rtw88_core/parameters/debug_mask

reg() { echo "$1 4" > "$R/read_reg"; cat "$R/read_reg"; }

snap() {
	{
		echo "=== $(date '+%F %T') $1"
		wpa_cli -i wlan0 status | grep wpa_state
		echo "-- FREE_TXPG (high normal low public)"; reg 0x10250020
		echo "-- FREE_TXPG_SEQ"; reg 0x10250028
		echo "-- TXPAUSE"; reg 0x0522
		echo "-- mac80211 queues"; cat "$P/queues" 2>&1
		for f in "$P"/netdev:wlan0/stations/*/; do
			echo "-- station $f"; cat "$f/flags" "$f/num_ps_buf_frames" 2>&1
		done
		echo "-- tx pending"; cat "$P/netdev:wlan0/aqm" 2>/dev/null | head -5
		echo "-- dmesg"; dmesg | tail -40
	} >> "$LOG" 2>&1
}

snap start
last=0
tail -n 0 -F /var/log/messages | while read -r line; do
	case "$line" in
	*"timed out"*|*"failed to get tx report"*|*"Not enough free pages"*)
		now=$(date +%s)
		if [ $((now - last)) -ge 60 ]; then
			last=$now
			snap "$line"
		fi
		;;
	esac
done
```

To stop it: `kill $(ps | awk '/wifiwatch|tail -n 0 -F/ && !/awk/ {print $1}')` (no `pgrep` on
the image).

Gotchas:
- No `rfkill` and no `timeout` on the image; BusyBox `wget` has no HTTPS and tries IPv6 first
  (hangs, the phone has no IPv6 route). Load tests: stream over SSH from the PC to the phone's
  Wi-Fi address (`dd if=/dev/zero bs=64k count=1600 | ssh root@<wlan ip> 'cat >/dev/null'`).
- Maps keeps tiles in `/var/lib/maps/tiles`: move it aside to force downloads, then put it back.
- `/var/log/messages` is in RAM; S99bootlog only keeps the first 45 s of each boot.
- From Windows/WSL: the key is at `keys/pinephone` inside WSL (`/home/nixos/pinephone-build/keys`);
  when WSL's network is down, Windows' ssh reaches the phone over Wi-Fi with a copy of the key.
