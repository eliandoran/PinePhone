# Flash the PinePhone over USB, no SD card required.
#
# Usage: pinephone-flash [--usb] [--expose] [IMAGES_DIR]
#   IMAGES_DIR   Buildroot images directory (default: output/images)
#   --usb        skip the over-the-air update, flash via USB mass storage
#   --expose     only expose the eMMC as a USB disk, write nothing
#
# If our firmware is running, rootfs.ext4 is sent over the USB network and
# installed into the inactive A/B slot by pp-update on the phone, which reboots.
# Otherwise pinephone.img is written via USB mass storage: hold Volume Up while
# powering on (bootloader UMS mode), or enter FEL mode.
# Expects UBOOT, SCRIPT and PP_UPDATE to be set (done by the Nix wrapper).

FEL_ID="1f3a:efe8"   # Allwinner BROM in FEL mode
UMS_ID="1f3a:1010"   # U-Boot USB mass storage gadget
PHONE="root@172.16.42.1"
SSH_KEY="${PINEPHONE_SSH_KEY:-keys/pinephone}"

mode=auto
IMAGES_DIR=output/images
for arg in "$@"; do
  case "$arg" in
    --usb) mode=usb ;;
    --expose) mode=expose ;;
    -*) echo "Unknown option: $arg" >&2; exit 1 ;;
    *) IMAGES_DIR=$arg ;;
  esac
done

ssh_phone() {
  ssh -i "$SSH_KEY" -o ConnectTimeout=3 -o BatchMode=yes \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
    "$PHONE" "$@"
}

if [[ "$mode" == auto ]] && [[ -f "$SSH_KEY" ]] && ssh_phone true 2>/dev/null; then
  ROOTFS="$IMAGES_DIR/rootfs.ext4"
  [[ -f "$ROOTFS" ]] || { echo "Image not found: $ROOTFS" >&2; exit 1; }
  echo "Phone is running, updating over the air with $ROOTFS"
  size=$(stat -L -c %s "$ROOTFS")  # rootfs.ext4 is a symlink to rootfs.ext2
  sha=$(sha256sum "$ROOTFS" | cut -d' ' -f1)
  old_slot=$(ssh_phone "sed -n 's/.*root=PARTUUID=50480001-0\([12]\).*/\1/p' /proc/cmdline")

  # Run the updater from this tree, not the possibly older copy on the phone.
  ssh_phone "cat > /tmp/pp-update" < "$PP_UPDATE"
  pv "$ROOTFS" | ssh_phone "sh /tmp/pp-update $size $sha"

  # The new slot boots on trial (S98pp-trial): wait until the phone is back
  # and the trial is settled, confirmed or rolled back.
  echo "Waiting for the phone to come back (up to 5 min)..."
  sleep 10
  deadline=$((SECONDS + 300))
  until ssh_phone "test ! -e /var/lib/pp-update/trial" 2>/dev/null; do
    if (( SECONDS > deadline )); then
      echo "Phone did not come back. Check the screen; recover with Volume Up (just flash-usb)." >&2
      exit 1
    fi
    sleep 2
  done
  slot=$(ssh_phone "sed -n 's/.*root=PARTUUID=50480001-0\([12]\).*/\1/p' /proc/cmdline")
  if [[ "$slot" != "$old_slot" ]]; then
    echo "Update confirmed, running slot $slot."
  else
    echo "Update ROLLED BACK, still running slot $slot. See 'just logs' or the pp-trial lines in dmesg." >&2
    exit 1
  fi
  exit 0
fi

IMAGE=""
if [[ "$mode" != expose ]]; then
  IMAGE="$IMAGES_DIR/pinephone.img"
  [[ -f "$IMAGE" ]] || { echo "Image not found: $IMAGE" >&2; exit 1; }
  [[ "$mode" == usb ]] || echo "Phone not reachable over the USB network, flashing via USB mass storage."
fi

find_ums_disk() {
  for dev in /sys/block/sd*; do
    [[ -e "$dev" ]] || continue
    props=$(udevadm info --query=property --path="$dev")
    if grep -qx "ID_VENDOR_ID=1f3a" <<<"$props" && grep -qx "ID_MODEL_ID=1010" <<<"$props"; then
      echo "/dev/$(basename "$dev")"
      return 0
    fi
  done
  return 1
}

# Either the bootloader on the eMMC (e.g. Tow-Boot, Volume Up at power-on)
# already exposes the eMMC, or the phone is in FEL mode and we load our own U-Boot.
echo "Waiting for PinePhone: USB mass storage ($UMS_ID) or FEL mode ($FEL_ID)..."
disk=""
via_fel=""
until disk=$(find_ums_disk); do
  if lsusb -d "$FEL_ID" >/dev/null 2>&1; then
    echo "FEL mode detected, loading U-Boot into RAM..."
    via_fel=1
    sunxi-fel -v -p uboot "$UBOOT" write 0x4FC00000 "$SCRIPT"
    for _ in $(seq 60); do
      disk=$(find_ums_disk) && break
      sleep 1
    done
    break
  fi
  sleep 0.5
done

if [[ -z "$disk" ]]; then
  echo "eMMC did not appear. Check 'lsusb' and 'dmesg'." >&2
  exit 1
fi

lsblk -o NAME,SIZE,MODEL,VENDOR "$disk"

if [[ -z "$IMAGE" ]]; then
  echo "eMMC available at $disk. When done: hold Power until the blue LED goes off, release right away, then press Power briefly to boot."
  exit 0
fi

# The first 4 MiB are never overwritten, so a bootloader there survives flashing.
# The A64 boot ROM looks for an eGON header at 8 KiB, then at 128 KiB.
# It can also boot from the eMMC boot0 hardware partition (where Tow-Boot installs
# itself on the PinePhone), which USB mass storage does not expose: not finding
# a header here does not mean there is no bootloader.
has_bootloader() {
  local off
  for off in 8 128; do
    if [[ "$(sudo dd if="$disk" bs=1024 skip="$off" count=1 status=none | dd bs=1 skip=4 count=8 status=none)" == "eGON.BT0" ]]; then
      return 0
    fi
  done
  return 1
}

had_bootloader=""
if has_bootloader; then
  had_bootloader=1
  echo "Bootloader found in the first 4 MiB of $disk, it will be kept."
elif [[ -n "$via_fel" ]]; then
  echo "WARNING: no bootloader found in the first 4 MiB of $disk." >&2
  echo "If there is none in boot0 either, the phone will only boot via FEL." >&2
else
  echo "Bootloader is outside the flashed area (eMMC boot0), it will be kept."
fi

echo
read -r -p "Flash $(basename "$IMAGE") to $disk (bootloader is kept)? Type 'yes' to continue: " answer
[[ "$answer" == "yes" ]] || { echo "Aborted."; exit 1; }

# Our image uses an MBR. Drop a leftover GPT (e.g. from Mobile NixOS) so tools
# don't see two partition tables: primary header in sector 1, backup in the last sector.
if [[ "$(sudo dd if="$disk" bs=512 skip=1 count=1 status=none | head -c 8)" == "EFI PART" ]]; then
  echo "Removing old GPT headers..."
  last=$(( $(sudo blockdev --getsz "$disk") - 1 ))
  sudo dd if=/dev/zero of="$disk" bs=512 seek=1 count=1 conv=fsync status=none
  sudo dd if=/dev/zero of="$disk" bs=512 seek="$last" count=1 conv=fsync status=none
fi

sudo dd if="$IMAGE" of="$disk" bs=512 count=1 conv=fsync status=none
sudo dd if="$IMAGE" of="$disk" bs=1M skip=4 seek=4 conv=fsync status=progress
sync

if [[ -n "$had_bootloader" ]] && ! has_bootloader; then
  echo "WARNING: bootloader signature no longer found!" >&2
fi
echo "Done. Hold Power until the blue LED goes off, release right away, then press Power briefly to boot."
