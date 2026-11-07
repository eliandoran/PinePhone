#!/bin/sh
#
# The USB gadget's mode (S05usbgadget sets it up in the default one):
#   default  serial console (ACM) + network (NCM)
#   mtp      network (NCM) + file transfer (MTP: umtprd on FunctionFS, umtprd.conf's folders)
# `usb-mode.sh default|mtp` switches, `usb-mode.sh status` prints the mode and whether a
# computer configured the gadget ("mtp configured", "default not attached", ...).
#
# The A64's MUSB has 5 IN endpoints besides ep0: ACM and NCM take two each (interrupt +
# bulk), MTP two too, so MTP takes the serial console's place. The network stays: usb0
# keeps its address, the host sees the gadget leave and come back (about a second).
# A switch is refused while a trial boot waits for the host (S98pp-trial).
#

GADGET=/sys/kernel/config/usb_gadget/pinephone
FFS=/dev/ffs-mtp
PIDFILE=/run/umtprd.pid
TRIAL=/var/lib/pp-update/trial
# Others to try one out of the image
UMTPRD=${UMTPRD:-/usr/sbin/umtprd}
UMTPRD_CONF=${UMTPRD_CONF:-/etc/umtprd/umtprd.conf}

log() {
	echo "usb-mode: $*" > /dev/kmsg
	echo "usb-mode: $*" >&2
}

udc_state() {
	cat /sys/class/udc/*/state 2>/dev/null | head -n1
}

mode() {
	[ -L "$GADGET/configs/c.1/ffs.mtp" ] && echo mtp || echo default
}

# The configuration's functions, in this order (the host numbers interfaces by it)
link_functions() {
	for f in "$GADGET"/configs/c.1/*.*; do
		[ -L "$f" ] && rm "$f"
	done
	for f in "$@"; do
		ln -s "$GADGET/functions/$f" "$GADGET/configs/c.1/" || return 1
	done
}

umtprd_stop() {
	start-stop-daemon -K -q -p "$PIDFILE" -s TERM
	i=0
	while [ $i -lt 30 ] && [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; do
		sleep 0.1
		i=$((i + 1))
	done
	[ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null && kill -KILL "$(cat "$PIDFILE")"
	rm -f "$PIDFILE"
	grep -q " $FFS " /proc/mounts && umount "$FFS"
	[ -d "$GADGET/functions/ffs.mtp" ] && rmdir "$GADGET/functions/ffs.mtp"
}

# umtprd writes the function's descriptors to ep0; FunctionFS then makes its
# endpoints, and only then may the gadget be bound
umtprd_start() {
	mkdir -p "$GADGET/functions/ffs.mtp" "$FFS" || return 1
	grep -q " $FFS " /proc/mounts || mount -t functionfs mtp "$FFS" || return 1
	# Without the lock (fd 9): the daemon would hold it, the next switch wait forever
	start-stop-daemon -S -q -b -m -p "$PIDFILE" -x /bin/sh -- \
		-c 'exec "$0" -conf "$1" > /tmp/umtprd.log 2>&1 9>&-' "$UMTPRD" "$UMTPRD_CONF"
	i=0
	while [ ! -e "$FFS/ep3" ]; do
		[ $i -ge 50 ] && { log "umtprd did not set up its endpoints (/tmp/umtprd.log)"; return 1; }
		sleep 0.1
		i=$((i + 1))
	done
}

switch() {
	[ "$(mode)" = "$1" ] && return 0
	if [ -f "$TRIAL" ]; then
		log "trial boot not confirmed yet, staying in $(mode) mode"
		return 1
	fi
	# S05usbgadget's first-connection retries would rebind the gadget meanwhile
	[ -f /run/usbgadget-reconnect.pid ] && kill "$(cat /run/usbgadget-reconnect.pid)" 2>/dev/null
	rm -f /run/usbgadget-reconnect.pid

	udc=$(ls /sys/class/udc | head -n1)
	# Unbound already when umtprd went away (its function lost its descriptors)
	echo "" > "$GADGET/UDC" 2>/dev/null
	if [ "$1" = mtp ]; then
		if umtprd_start && link_functions ncm.usb0 ffs.mtp; then
			echo "NCM + MTP" > "$GADGET/configs/c.1/strings/0x409/configuration"
		else
			umtprd_stop
			link_functions acm.usb0 ncm.usb0
			echo "$udc" > "$GADGET/UDC"
			return 1
		fi
	else
		link_functions acm.usb0 ncm.usb0
		umtprd_stop
		echo "ACM + NCM" > "$GADGET/configs/c.1/strings/0x409/configuration"
	fi
	echo "$udc" > "$GADGET/UDC"
	log "$1 mode"
}

case "$1" in
	status)
		echo "$(mode) $(udc_state)"
		;;
	default|mtp)
		exec 9> /run/usb-mode.lock
		flock 9
		switch "$1"
		;;
	*)
		echo "Usage: $0 default|mtp|status" >&2
		exit 1
		;;
esac
