#!/bin/sh
#
# Mobile data's network interface (modem.conf's data_script=): phone-ui runs
# `mobile-data.sh up|down|reset <interface>` as the modem's data session starts, ends or
# is refused; udhcpc runs it for the lease (deconfig, bound, renew, ...).
#   up     raw IP (qmi_wwan: the EG25-G sends IP packets, not Ethernet frames), the
#          link up, udhcpc in the background
#   down   udhcpc stopped, the address, route and DNS servers gone, the link down
#   reset  the interface's USB driver bound again: after the modem's USB reset (in
#          suspend) it refuses a session until then
# The default route has metric 5: a Wi-Fi network's (0) goes first, the USB host's (10)
# after. The DNS servers are tagged with the interface, as udhcpc's default script does.
#

RESOLV=/etc/resolv.conf

resolv_drop() {
	grep -v "# $1\$" "$RESOLV" > "/tmp/resolv.$$"
	cat "/tmp/resolv.$$" > "$RESOLV"
	rm -f "/tmp/resolv.$$"
}

# udhcpc: the interface in $interface, the lease in the environment
case "$1" in
	deconfig)
		ip -4 addr flush dev "$interface"
		resolv_drop "$interface"
		exit 0
		;;
	bound|renew)
		ip -4 addr flush dev "$interface"
		ip addr add "$ip/${mask:-32}" dev "$interface"
		set -- $router
		[ -n "$1" ] && ip route replace default via "$1" dev "$interface" metric 5
		resolv_drop "$interface"
		for server in $dns; do
			echo "nameserver $server # $interface" >> "$RESOLV"
		done
		exit 0
		;;
	leasefail|nak)
		exit 0
		;;
esac

IF=$2
PID=/run/udhcpc.$IF.pid
[ -n "$IF" ] && [ -d "/sys/class/net/$IF" ] || { echo "mobile-data: no interface $IF" >&2; exit 1; }

dhcp_stop() {
	[ -f "$PID" ] && kill "$(cat "$PID")" 2>/dev/null
	rm -f "$PID"
}

case "$1" in
	up)
		dhcp_stop
		# Only while down: qmi_wwan refuses the change on a running link
		if [ -f "/sys/class/net/$IF/qmi/raw_ip" ] && [ "$(cat "/sys/class/net/$IF/qmi/raw_ip")" != Y ]; then
			ip link set "$IF" down
			echo Y > "/sys/class/net/$IF/qmi/raw_ip"
		fi
		ip link set "$IF" up
		udhcpc -i "$IF" -b -p "$PID" -s "$0" >/dev/null 2>&1
		;;
	down)
		dhcp_stop
		ip route del default dev "$IF" 2>/dev/null
		interface=$IF "$0" deconfig
		ip link set "$IF" down
		;;
	reset)
		dhcp_stop
		dev=$(readlink -f "/sys/class/net/$IF/device")
		driver=$(readlink -f "$dev/driver")
		[ -d "$driver" ] || exit 1
		echo "${dev##*/}" > "$driver/unbind"
		sleep 1
		echo "${dev##*/}" > "$driver/bind"
		;;
	*)
		echo "Usage: $0 up|down|reset <interface>" >&2
		exit 1
		;;
esac
