#!/bin/sh
#
# wpa_cli's action script (S15wifi): `wpa_action.sh wlan0 CONNECTED|DISCONNECTED`.
# Connected, udhcpc gets an address (renewed when it already runs: another access
# point of the network); the default script adds the network's default route (metric
# 0, before the USB one) and DNS servers. Disconnected, udhcpc releases the lease and
# leaves.
#

PID=/run/udhcpc.$1.pid

case "$2" in
	CONNECTED)
		if [ -f "$PID" ] && kill -0 "$(cat "$PID")" 2>/dev/null; then
			kill -USR1 "$(cat "$PID")"
		else
			udhcpc -i "$1" -b -R -p "$PID" -x hostname:pinephone >/dev/null 2>&1
		fi
		;;
	DISCONNECTED)
		[ -f "$PID" ] && kill "$(cat "$PID")" 2>/dev/null
		rm -f "$PID"
		interface="$1" /usr/share/udhcpc/default.script deconfig >/dev/null
		;;
esac
