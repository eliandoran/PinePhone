#!/bin/sh
#
# udhcpc's script for Wi-Fi (wpa_action.sh): the default one, but deconfig only drops
# the address and the network's DNS servers. The default's brings the interface up,
# which would turn Wi-Fi on again when phone-ui took wlan0 down: udhcpc deconfigures
# as it leaves.
#

if [ "$1" = deconfig ]; then
	ip -4 addr flush dev "$interface"
	grep -v "# $interface\$" /etc/resolv.conf > "/tmp/resolv.$$"
	cat "/tmp/resolv.$$" > /etc/resolv.conf
	rm -f "/tmp/resolv.$$"
	exit 0
fi
exec /usr/share/udhcpc/default.script "$@"
