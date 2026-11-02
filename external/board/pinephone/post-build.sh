#!/bin/sh
# Install the development SSH key (created by the top-level Makefile) for root,
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
