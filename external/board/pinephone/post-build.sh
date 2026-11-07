#!/bin/sh
# Keep root's authorized keys and the SSH host keys on the data partition,
# and create mount points needed on the read-only root filesystem.

set -e

TARGET_DIR=$1

# root's authorized_keys on the data partition, where phone-ui adds the computers
# allowed on the phone (pairing.c): /root/.ssh links to /var/lib/ssh. The image
# trusts no key, not even the one of the computer that built it (keys/pinephone):
# each computer is allowed on the phone's screen once. Earlier builds installed
# that key and S49ssh-keys, which output/target keeps.
rm -rf "$TARGET_DIR/root/.ssh"
ln -snf /var/lib/ssh "$TARGET_DIR/root/.ssh"
install -d -m 0700 "$TARGET_DIR/var/lib/ssh"
rm -f "$TARGET_DIR/var/lib/ssh/authorized_keys" "$TARGET_DIR/etc/init.d/S49ssh-keys"
rm -rf "$TARGET_DIR/usr/share/ssh"

# Data partition (S00data)
install -d -m 0755 "$TARGET_DIR/data"

# SSH host keys on the data partition (/var/lib), so they survive reboots and
# updates. Buildroot points /etc/dropbear to /var/run, which is RAM here.
install -d -m 0700 "$TARGET_DIR/var/lib/dropbear"
ln -snf /var/lib/dropbear "$TARGET_DIR/etc/dropbear"

# The USB host's name, so lookups of its address stay local. Dropbear resolves
# the client's address before its banner: without internet sharing the queries
# to the public DNS servers are dropped, and SSH waited 20 s (2 servers x 2 x 5 s).
grep -q '^172\.16\.42\.2[[:space:]]' "$TARGET_DIR/etc/hosts" ||
	printf '172.16.42.2\thost\n' >> "$TARGET_DIR/etc/hosts"

# fontconfig: only the rendering settings of the stock configuration, the aliases
# being 50-pinephone.conf (overlay). The stock ones list over 100 families for
# fonts that are not installed, and WebKit's Skia checks each of them with a match
# of its own on every font lookup: a cold Wikipedia page drew after 10 s, not 2 s.
for f in 20-unhint-small-vera 30-metric-aliases 40-nonlatin 45-generic 45-latin \
	48-spacing 60-generic 60-latin 65-fonts-persian 65-nonlatin 69-unifont 80-delicious; do
	rm -f "$TARGET_DIR/etc/fonts/conf.d/$f.conf"
done

# Wi-Fi firmware: linux-firmware installs every rtw88 chip's, the RTL8723CS
# (rtw88_8723cs) only loads rtw8703b_fw.bin and rtw8703b_wow_fw.bin.
if [ -d "$TARGET_DIR/lib/firmware/rtw88" ]; then
	find "$TARGET_DIR/lib/firmware/rtw88" ! -type d ! -name 'rtw8703b_*' -delete
fi

# v4l-utils: only media-ctl and v4l2-ctl (and libv4l2 with libv4lconvert, which
# v4l2-ctl links) are used, to set up and test the camera pipeline. Buildroot builds
# all its tools: CEC, DVB, infrared (their keymaps and udev rule), compliance tests.
for f in cec-compliance cec-ctl cec-follower cx18-ctl decode_tm6000 edid-decode ir-ctl \
	ir-keytable ivtv-ctl rds-ctl v4l2-compliance v4l2-sysfs-path; do
	rm -f "$TARGET_DIR/usr/bin/$f"
done
rm -f "$TARGET_DIR/usr/sbin/v4l2-dbg" "$TARGET_DIR/etc/rc_maps.cfg" \
	"$TARGET_DIR/lib/udev/rules.d/70-infrared.rules" "$TARGET_DIR"/usr/lib/libv4l1.so* \
	"$TARGET_DIR"/usr/lib/libv4l2rds.so* "$TARGET_DIR/usr/lib/libv4l/ov511-decomp" \
	"$TARGET_DIR/usr/lib/libv4l/ov518-decomp" "$TARGET_DIR/usr/lib/libv4l/v4l1compat.so" \
	"$TARGET_DIR/usr/lib/libv4l/v4l2convert.so"
rm -rf "$TARGET_DIR/lib/udev/rc_keymaps"
# DVB's character sets for glibc's iconv(), the only modules in gconv/
rm -f "$TARGET_DIR/usr/lib/gconv/ARIB-STD-B24.so" "$TARGET_DIR/usr/lib/gconv/EN300-468-TAB00.so" \
	"$TARGET_DIR/usr/lib/gconv/gconv-modules"
rmdir "$TARGET_DIR/usr/lib/gconv" 2>/dev/null || true
