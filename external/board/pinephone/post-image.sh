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
# The kernel keeps 2% of the blocks (at most 4096) out of reach for delayed
# allocation, reserved blocks or not: add them, else the slack cannot be written
resv=$(((min + SLACK_BLOCKS) / 49 + 1))
[ "$resv" -le 4096 ] || resv=4096
blocks=$((min + SLACK_BLOCKS + resv))
resize2fs "$IMG" "$blocks" >/dev/null
bs=$(dumpe2fs -h "$IMG" 2>/dev/null | sed -n 's/^Block size: *//p')
truncate -s $((blocks * bs)) "$IMG"
echo "post-image: rootfs.ext2 shrunk to $((blocks * bs / 1024 / 1024)) MiB"

# The microSD card's image (pinephone-sd.img) has a disk signature of its own, so a
# card and the eMMC both holding the firmware do not share partition IDs: the same
# root filesystem, its boot entry pointed at the card's slot A
SD="$BINARIES_DIR/rootfs-sd.ext4"
cp "$IMG" "$SD"
conf=/boot/extlinux/extlinux.conf
tmp=$(mktemp -d)
debugfs -R "dump $conf $tmp/extlinux.conf" "$SD" 2>/dev/null
grep -q 'root=PARTUUID=50480001-01' "$tmp/extlinux.conf" ||
	{ echo "post-image: no root=PARTUUID=50480001-01 in $conf" >&2; exit 1; }
sed -i 's/root=PARTUUID=50480001-01/root=PARTUUID=50480002-01/' "$tmp/extlinux.conf"
# written anew: owned by root, as the original (debugfs gives it the builder's)
debugfs -w -f - "$SD" >/dev/null 2>&1 <<EOF
rm $conf
write $tmp/extlinux.conf $conf
sif $conf uid 0
sif $conf gid 0
sif $conf mode 0100644
EOF
debugfs -R "cat $conf" "$SD" 2>/dev/null | grep -q 'root=PARTUUID=50480002-01' ||
	{ echo "post-image: $conf not rewritten in rootfs-sd.ext4" >&2; exit 1; }
rm -r "$tmp"
e2fsck -fn "$SD" >/dev/null

exec support/scripts/genimage.sh "$@"
