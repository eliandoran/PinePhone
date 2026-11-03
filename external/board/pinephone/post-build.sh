#!/bin/sh
# Install the development SSH key (created by the top-level Makefile) for root,
# keep SSH host keys on the data partition,
# and create mount points needed on the read-only root filesystem.

set -e

TARGET_DIR=$1
KEY="$(dirname "$0")/../../../keys/pinephone.pub"

if [ -f "$KEY" ]; then
	install -d -m 0700 "$TARGET_DIR/root/.ssh"
	install -m 0600 "$KEY" "$TARGET_DIR/root/.ssh/authorized_keys"
else
	echo "post-build: $KEY not found, SSH login will not work" >&2
fi

# Data partition (S00data)
install -d -m 0755 "$TARGET_DIR/data"

# SSH host keys on the data partition (/var/lib), so they survive reboots and
# updates. Buildroot points /etc/dropbear to /var/run, which is RAM here.
install -d -m 0700 "$TARGET_DIR/var/lib/dropbear"
ln -snf /var/lib/dropbear "$TARGET_DIR/etc/dropbear"

# fontconfig: only the rendering settings of the stock configuration, the aliases
# being 50-pinephone.conf (overlay). The stock ones list over 100 families for
# fonts that are not installed, and WebKit's Skia checks each of them with a match
# of its own on every font lookup: a cold Wikipedia page drew after 10 s, not 2 s.
for f in 20-unhint-small-vera 30-metric-aliases 40-nonlatin 45-generic 45-latin \
	48-spacing 60-generic 60-latin 65-fonts-persian 65-nonlatin 69-unifont 80-delicious; do
	rm -f "$TARGET_DIR/etc/fonts/conf.d/$f.conf"
done
