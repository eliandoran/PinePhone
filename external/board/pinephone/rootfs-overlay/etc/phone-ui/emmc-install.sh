#!/bin/sh
#
# Installs the system running from a microSD card on the eMMC, with its data (Settings ›
# Storage, phone-ui's emmc_install.c):
#   status      "card" when the system runs from a card and the phone has an eMMC
#               ("emmc" from the eMMC; exit 1 when neither)
#   install     the eMMC made as pinephone.img would make it, without the card's
#               mark on it: Tow-Boot in boot0 (when it holds no bootloader yet: the
#               boot ROM starts the eMMC's from there), the partition table (disk
#               signature 50480001, genimage.cfg's), the running slot as slot A pointed
#               at its new partition, the data partition (S00data's) with /data's files.
#               The card is only read: a failure leaves it as it was.
#   card-status "system" when the system runs from the eMMC and the card in the phone
#               holds a copy of it (the card's disk signature, 50480002; exit 1 otherwise)
#   erase-card  that card no longer starts: its partition table and Tow-Boot's header
#               cleared, from the eMMC only (Settings › Storage once installed)
# install prints each step as it starts ("step bootloader", "step system", "step data"),
# "kept bootloader" when boot0 had one, "done" at the end; a failure "error: <why>" (with
# the last line the tool that failed printed) and exit 1.
#
# Writes the first 4 MiB of the eMMC's user area, which nothing else writes but sector 0
# (the flasher, pp-update): an earlier system's bootloader at 8 or 128 KiB there would
# start before boot0's (a sector each, once boot0 holds one). Its boot0 only when empty.
#
# Others for the tests (phone-ui's test_emmc_install.c: files for the disks): CMDLINE,
# CARD (the card's disk), ROOT (the running slot), EMMC, BOOT0, DATA (the data partition
# mounted), TOW_BOOT (its build for boot0), PROC (for drop_caches).

CMDLINE=${CMDLINE:-/proc/cmdline}
DATA=${DATA:-/data}
TOW_BOOT=${TOW_BOOT:-/usr/share/tow-boot/Tow-Boot.mmcboot.bin}
PROC=${PROC:-/proc}

# genimage.cfg's layout, in sectors: slot A at 4 MiB, slots of 512 MiB, data after them
SLOT_A=8192
SLOT_SIZE=1048576
SLOT_B=$((SLOT_A + SLOT_SIZE))
DATA_START=$((SLOT_B + SLOT_SIZE))
DATA_MIN=$((64 * 1024 * 1024)) # bytes the data partition needs besides the data

fail() {
	echo "error: $*"
	exit 1
}

# A tool run with what it says on errors kept, for failed() to tell
ERR=/tmp/emmc-install.err
quiet() {
	"$@" >/dev/null 2>"$ERR"
}

# Fails with what the last tool run by quiet() said
failed() {
	said=$(tail -n 1 "$ERR" 2>/dev/null)
	fail "$1${said:+: $said}"
}

# The running disk's signature and slot, from the root= extlinux.conf gave
signature() {
	sed -n 's/.*root=PARTUUID=\(5048000[12]\)-0[12].*/\1/p' "$CMDLINE"
}

slot() {
	sed -n 's/.*root=PARTUUID=5048000[12]-0\([12]\).*/\1/p' "$CMDLINE"
}

# The disk the root file system is mounted from (pp-update's)
root_disk() {
	majmin=$(awk '$5 == "/" { print $3; exit }' /proc/self/mountinfo)
	rootpart=$(basename "$(readlink -f "/sys/dev/block/$majmin")")
	echo "${rootpart%p[0-9]*}"
}

# The microSD card: an MMC disk without boot partitions
card_disk() {
	for b in /sys/block/mmcblk*; do
		d=${b##*/}
		case $d in *boot[0-9] | *rpmb) continue ;; esac
		[ -e "/sys/block/${d}boot0" ] && continue
		echo "$d"
		return
	done
}

# A disk's signature (its MBR's, little-endian), as its partitions' IDs start
disk_id() {
	dd if="$1" bs=1 skip=440 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n'
}

# The eMMC: the disk with boot partitions
emmc_disk() {
	for b in /sys/block/mmcblk*boot0; do
		[ -e "$b" ] || continue
		d=${b##*/}
		echo "${d%boot0}"
		return
	done
}

find_disks() {
	if [ -z "$CARD" ] || [ -z "$ROOT" ]; then
		d=$(root_disk)
		ROOT=${ROOT:-/dev/${d}p$(slot)}
		if [ -z "$CARD" ] && [ "$(signature)" = 50480002 ]; then
			CARD=/dev/$d
		elif [ -z "$CARD" ]; then
			d=$(card_disk)
			[ -n "$d" ] && CARD=/dev/$d
		fi
	fi
	if [ -z "$EMMC" ]; then
		d=$(emmc_disk)
		[ -n "$d" ] && EMMC=/dev/$d && BOOT0=${BOOT0:-/dev/${d}boot0}
	fi
}

# Bytes in a disk or file
size() {
	if [ -b "$1" ]; then blockdev --getsize64 "$1"; else wc -c < "$1"; fi
}

# An Allwinner boot image (eGON) at byte $2 of $1
egon() {
	[ "$(dd if="$1" bs=512 skip=$(($2 / 512)) count=1 2>/dev/null | dd bs=1 skip=4 count=8 2>/dev/null | tr -d '\000')" = "eGON.BT0" ]
}

# SHA-256 of the first $3 bytes from MiB $2 of $1
sum() {
	dd if="$1" bs=1M skip="$2" count=$((($3 + 1048575) / 1048576)) 2>/dev/null | head -c "$3" | sha256sum | cut -d' ' -f1
}

# Zero sector $2 of $1
clear_sector() {
	dd if=/dev/zero of="$1" bs=512 seek="$2" count=1 conv=notrunc,fsync 2>/dev/null
}

status() {
	find_disks
	sig=$(signature)
	if [ "$sig" = 50480002 ] && [ -n "$EMMC" ] && [ -e "$EMMC" ] && [ "$EMMC" != "$CARD" ]; then
		echo card
	elif [ "$sig" = 50480001 ]; then
		echo emmc
	else
		exit 1
	fi
}

bootloader() {
	echo "step bootloader"
	if egon "$BOOT0" 0; then
		echo "kept bootloader"
	else
		ro=/sys/class/block/${BOOT0##*/}/force_ro
		[ -e "$ro" ] && echo 0 > "$ro"
		# Everything after the first 8 KiB first: interrupted, boot0 holds no header
		# rather than half a bootloader (Tow-Boot's own installer's order)
		quiet dd if="$TOW_BOOT" of="$BOOT0" bs=8192 skip=1 seek=1 conv=notrunc,fsync &&
			quiet dd if="$TOW_BOOT" of="$BOOT0" bs=8192 count=1 conv=notrunc,fsync
		ok=$?
		[ -e "$ro" ] && echo 1 > "$ro"
		[ $ok -eq 0 ] || failed "cannot write the bootloader to $BOOT0"
		{ echo 3 > "$PROC/sys/vm/drop_caches"; } 2>/dev/null
		[ "$(sum "$BOOT0" 0 "$(size "$TOW_BOOT")")" = "$(sum "$TOW_BOOT" 0 "$(size "$TOW_BOOT")")" ] ||
			fail "the bootloader written to $BOOT0 reads back different"
	fi
	for kib in 8 128; do
		if egon "$EMMC" $((kib * 1024)); then
			clear_sector "$EMMC" $((kib * 2)) || fail "cannot clear the bootloader at $kib KiB of $EMMC"
		fi
	done
}

system() {
	echo "step system"
	# Whatever the eMMC held has no partition table until the end: Tow-Boot looks for a
	# system on the eMMC before the card, and would start a half-written one. A GPT's
	# headers (another system's) cleared too, its first and backup ones
	clear_sector "$EMMC" 0 && clear_sector "$EMMC" 1 && clear_sector "$EMMC" $(($(size "$EMMC") / 512 - 1)) ||
		fail "cannot write to $EMMC"

	quiet dd if="$ROOT" of="$EMMC" bs=1M seek=$((SLOT_A / 2048)) count=$(((root_bytes + 1048575) / 1048576)) \
		conv=notrunc,fsync || failed "cannot write the system to $EMMC"
	{ echo 3 > "$PROC/sys/vm/drop_caches"; } 2>/dev/null
	[ "$(sum "$EMMC" $((SLOT_A / 2048)) "$root_bytes")" = "$(sum "$ROOT" 0 "$root_bytes")" ] ||
		fail "the system written to $EMMC reads back different"

	# Its boot entry pointed at its new partition, as post-image.sh points the card's
	fs="$EMMC?offset=$((SLOT_A * 512))"
	conf=/boot/extlinux/extlinux.conf
	tmp=$(mktemp -d /tmp/emmc-install.XXXXXX) || fail "no room in /tmp"
	quiet debugfs -R "dump $conf $tmp/extlinux.conf" "$fs"
	grep -q 'root=PARTUUID=5048000[12]-0[12]' "$tmp/extlinux.conf" 2>/dev/null ||
		{ rm -r "$tmp"; failed "no root= in the system's $conf"; }
	sed -i 's/root=PARTUUID=5048000[12]-0[12]/root=PARTUUID=50480001-01/' "$tmp/extlinux.conf"
	quiet debugfs -w -f - "$fs" <<-EOF
		rm $conf
		write $tmp/extlinux.conf $conf
		sif $conf uid 0
		sif $conf gid 0
		sif $conf mode 0100644
	EOF
	rm -r "$tmp"
	debugfs -R "cat $conf" "$fs" 2>/dev/null | grep -q 'root=PARTUUID=50480001-01' ||
		fail "cannot point the system's $conf at the eMMC"
}

# pinephone.img's partition table, once everything it points to is written
table() {
	printf 'label: dos\nlabel-id: 0x50480001\nstart=%s, size=%s, type=83, bootable\nstart=%s, size=%s, type=83\nstart=%s, type=83\n' \
		$SLOT_A $SLOT_SIZE $SLOT_B $SLOT_SIZE $DATA_START |
		quiet sfdisk --quiet --wipe never --no-reread --no-tell-kernel "$EMMC" ||
		failed "cannot write the partition table to $EMMC"
}

data() {
	echo "step data"
	# Lazy initialization: the inode tables are written by the kernel once mounted
	quiet mke2fs -F -q -t ext4 -L PP_DATA -E "offset=$((DATA_START * 512)),nodiscard" -d "$DATA" \
		"$EMMC" "$((data_bytes / 1024))k" </dev/null || failed "cannot copy the data to $EMMC"
	quiet e2fsck -fn "$EMMC?offset=$((DATA_START * 512))" ||
		failed "the data partition on $EMMC does not check"
}

install() {
	find_disks
	[ "$(status)" = card ] || fail "not running from a microSD card, or no eMMC"
	# All the tools first: an image without one (a package option not rebuilt) writes nothing
	for t in sfdisk mke2fs e2fsck dumpe2fs debugfs blockdev; do
		command -v "$t" >/dev/null || fail "no $t in this system"
	done
	[ -f "$TOW_BOOT" ] || fail "no $TOW_BOOT"
	[ -r "$ROOT" ] || fail "cannot read the system from $ROOT"
	# A system on trial (pp-update) is not known to start yet
	[ -f "$DATA/var/lib/pp-update/trial" ] && fail "the system is on trial: restart first"

	bs=$(dumpe2fs -h "$ROOT" 2>/dev/null | sed -n 's/^Block size: *//p')
	blocks=$(dumpe2fs -h "$ROOT" 2>/dev/null | sed -n 's/^Block count: *//p')
	[ -n "$bs" ] && [ -n "$blocks" ] || fail "no file system on $ROOT"
	root_bytes=$((bs * blocks))
	[ "$root_bytes" -le $((SLOT_SIZE * 512)) ] || fail "the system does not fit in a slot"

	emmc_bytes=$(size "$EMMC")
	data_bytes=$((emmc_bytes - DATA_START * 512))
	[ "$data_bytes" -ge "$DATA_MIN" ] || fail "$EMMC is too small"
	used=$(du -sk "$DATA" | cut -f1)
	[ $((used * 1024 + DATA_MIN)) -le "$data_bytes" ] || fail "the data does not fit on $EMMC"
	[ "$(size "$TOW_BOOT")" -le "$(size "$BOOT0")" ] || fail "the bootloader does not fit in $BOOT0"

	bootloader
	system
	data
	table
	sync
	echo done
}

# Running from the eMMC with a card of this system in the phone
card_status() {
	find_disks
	[ "$(signature)" = 50480001 ] && [ -n "$CARD" ] && [ -e "$CARD" ] &&
		[ "$(disk_id "$CARD")" = 02004850 ] || exit 1
	echo system
}

erase_card() {
	[ "$(card_status)" = system ] || fail "not running from the eMMC with a card of this system"
	find_disks
	# The partition table, then Tow-Boot's header at 8 KiB
	clear_sector "$CARD" 0 && clear_sector "$CARD" 16 || fail "cannot write to $CARD"
	sync
}

case "$1" in
	status) status ;;
	install) install ;;
	card-status) card_status ;;
	erase-card) erase_card ;;
	*)
		echo "Usage: $0 status|install|card-status|erase-card" >&2
		exit 1
		;;
esac
