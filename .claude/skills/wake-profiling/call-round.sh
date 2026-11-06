#!/usr/bin/env bash
# One unattended round: the PinePhone put to sleep (a Power press injected into its
# evdev node), called from an Android phone over adb once asleep, hung up once phone-ui
# shows the call screen; phone-ui's log for the round and dmesg saved in <out-dir>.
#
#   PINEPHONE_NUMBER=+40... call-round.sh <out-dir> [sleep-seconds]
#
# ADB_SERIAL picks the Android phone when several are connected. Run from anywhere in
# the repository (keys/pinephone). The modem must be up (phone-ui logged "network:"):
# right after a boot its start-up wakes the phone before the call does.
# shellcheck disable=SC2016 # the single-quoted commands expand on the PinePhone
set -u
OUT=${1:?usage: call-round.sh <out-dir> [sleep-seconds]}
ASLEEP=${2:-12}
NUMBER=${PINEPHONE_NUMBER:?set to the number of the PinePhone, to call it}
mkdir -p "$OUT"
cd "$(git rev-parse --show-toplevel)" || exit 1
PP() {
	timeout 20 ssh -o ConnectTimeout=10 -i keys/pinephone -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@172.16.42.1 "$@"
}
ADB() { adb ${ADB_SERIAL:+-s "$ADB_SERIAL"} "$@"; }

PP 'grep -q "network:" /tmp/phone-ui.log' || echo "warning: the modem is not registered yet"

# struct input_event on arm64: timeval (16 bytes), type and code (u16), value (s32),
# little endian; EV_KEY (1) KEY_POWER (116 = \164) pressed then released, each followed
# by EV_SYN. event0 is axp20x-pek, the Power key.
PP 'ev() { printf "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0$1$2$3"; }
key() { ev "\001\0" "\164\0" "$1\0\0\0"; ev "\0\0" "\0\0" "\0\0\0\0"; }
wc -l < /tmp/phone-ui.log > /tmp/round-start
{ key "\001"; sleep 0.1; key "\0"; } > /dev/input/event0' || { echo "no PinePhone"; exit 1; }
echo "Power pressed, asleep for ${ASLEEP} s"
sleep "$ASLEEP"

ADB shell am start -a android.intent.action.CALL -d "tel:$NUMBER" > /dev/null
echo "calling"
shown=""
for _ in $(seq 1 30); do
	sleep 1
	if PP 'tail -n +$(cat /tmp/round-start) /tmp/phone-ui.log | grep -q "call screen over the lock screen"' 2>/dev/null; then
		shown=1
		break
	fi
done
sleep 2
ADB shell input keyevent KEYCODE_ENDCALL
echo "hung up, call screen $([ -n "$shown" ] && echo shown || echo 'not seen')"
sleep 3
PP 'tail -n +$(cat /tmp/round-start) /tmp/phone-ui.log' > "$OUT/phone-ui.log"
PP dmesg > "$OUT/dmesg.txt"
grep -E "\[[0-9.]+\]" "$OUT/phone-ui.log"
