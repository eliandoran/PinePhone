# Flash an image to the PinePhone eMMC over USB, no SD card required.
#
# Usage: pinephone-flash [IMAGE]
#   Without IMAGE, the eMMC is only exposed as a USB disk and left for you to use.
#
# Put the phone in FEL mode first: power off, hold Volume Up, press Power,
# keep holding Volume Up for a few seconds. The screen stays black.
# Expects UBOOT and SCRIPT to be set (done by the Nix wrapper).

IMAGE="${1:-}"
FEL_ID="1f3a:efe8"   # Allwinner BROM in FEL mode
UMS_ID="1f3a:1010"   # U-Boot USB mass storage gadget

if [[ -n "$IMAGE" && ! -f "$IMAGE" ]]; then
  echo "Image not found: $IMAGE" >&2
  exit 1
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
until disk=$(find_ums_disk); do
  if lsusb -d "$FEL_ID" >/dev/null 2>&1; then
    echo "FEL mode detected, loading U-Boot into RAM..."
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
  echo "eMMC available at $disk. Power off the phone (hold Power) when done."
  exit 0
fi

echo
read -r -p "Overwrite $disk with $(basename "$IMAGE")? Type 'yes' to continue: " answer
[[ "$answer" == "yes" ]] || { echo "Aborted."; exit 1; }

sudo dd if="$IMAGE" of="$disk" bs=4M conv=fsync status=progress
sync
echo "Done. Hold Power to turn the phone off, then boot normally."
