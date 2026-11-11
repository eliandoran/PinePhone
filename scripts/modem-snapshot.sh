#!/bin/sh
# The EG25-G's state, for `just modem-record`: run on the phone (BusyBox), it asks the
# modem's second AT port (interface 3, which phone-ui leaves alone: it holds interface 2's)
# and prints each command with its answer, as "# " lines around them in a recording.
# Nothing is changed. Left out: what identifies the SIM or its owner (IMSI, ICCID, number).
set -u

# Interface 3's tty (ttyUSB3 usually; another number after a reset with a port open)
port=
for tty in /sys/class/tty/ttyUSB*; do
	[ "$(cat "$tty/device/../bInterfaceNumber" 2>/dev/null)" = 03 ] &&
		[ "$(cat "$tty/device/../../idVendor" 2>/dev/null)" = 2c7c ] &&
		port=/dev/${tty##*/} && break
done
if [ -z "$port" ]; then
	echo "# no modem on USB"
	exit 0
fi

stty -F "$port" raw -echo
exec 3<"$port"
cr=$(printf '\r')

# A command and its answer, up to OK or an error (or 3 s without a line)
ask() {
	printf '%s\r' "$1" > "$port"
	echo "# $1"
	# shellcheck disable=SC3045 # BusyBox ash has read -t
	while read -t 3 -r line <&3; do
		line=${line%"$cr"}
		[ -z "$line" ] && continue
		case "$line" in AT*) continue ;; esac # its echo
		echo "#   $line"
		case "$line" in OK | ERROR | "+CME ERROR"* | "+CMS ERROR"*) break ;; esac
	done
}

echo "# Modem state $(date '+%Y-%m-%d %H:%M:%S %Z') on $port"
ask "ATE0"
# Firmware and carrier profile (selected by the SIM: AutoSel)
ask "AT+QGMR"
ask 'AT+QMBNCFG="autosel"'
ask 'AT+QMBNCFG="list"'
# Network: registration (CS, PS, EPS), operator, technology, the serving cell
ask "AT+COPS?"
ask "AT+CREG?"
ask "AT+CGREG?"
ask "AT+CEREG?"
ask "AT+QNWINFO"
ask 'AT+QENG="servingcell"'
ask "AT+CSQ"
ask "AT+CESQ"
ask 'AT+QCFG="nwscanmode"'
ask 'AT+QCFG="servicedomain"'
ask 'AT+QCFG="band"'
# IMS and VoLTE: setting and registration (<value>,<registered>), IMS registration (27.007)
ask 'AT+QCFG="ims"'
ask "AT+CIREG?"
ask "AT+CEVDP?"
ask "AT+CAVIMS?"
# SMS: domain (NV: 00 circuit switched, 01 IMS), service, centre, reports, over GPRS or CS
ask 'AT+QNVFR="/nv/item_files/modem/mmode/sms_domain_pref"'
ask "AT+CSMS?"
ask "AT+CSCA?"
ask "AT+CNMI?"
ask "AT+CGSMS?"
ask 'AT+QURCCFG="urcport"'
# Data: contexts (APNs, the carrier profile's IMS one) and those active
ask "AT+CGDCONT?"
ask "AT+CGACT?"
# Calls now, and why the last one ended
ask "AT+CLCC"
ask "AT+CEER"
