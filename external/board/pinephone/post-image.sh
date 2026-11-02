#!/bin/sh
# Shrink the root filesystem to its content plus a little slack, then build
# pinephone.img. The root filesystem is mounted read-only (built without a
# journal or reserved blocks, see the defconfig); the slack only lets
# pp-update rewrite extlinux.conf in a freshly written slot.
# Arguments are passed on to genimage.sh (-c genimage.cfg).

set -e

IMG="$BINARIES_DIR/rootfs.ext2"
SLACK_BLOCKS=256

e2fsck -fy "$IMG" >/dev/null || [ $? -le 1 ]
min=$(resize2fs -P "$IMG" 2>/dev/null | sed -n 's/.*minimum size of the filesystem: //p')
blocks=$((min + SLACK_BLOCKS))
resize2fs "$IMG" "$blocks" >/dev/null
bs=$(dumpe2fs -h "$IMG" 2>/dev/null | sed -n 's/^Block size: *//p')
truncate -s $((blocks * bs)) "$IMG"
echo "post-image: rootfs.ext2 shrunk to $((blocks * bs / 1024 / 1024)) MiB"

exec support/scripts/genimage.sh "$@"
