# Flash the PinePhone over USB, no SD card required.
#
# Usage: pinephone-flash [--usb] [--expose] [IMAGES_DIR]
#   IMAGES_DIR   Buildroot images directory (default: output/images)
#   --usb        flash pinephone.img via USB mass storage
#   --expose     only expose the eMMC as a USB disk, write nothing
#
# Without an option, our firmware must be running: rootfs.ext4 is sent over the
# USB network and installed into the inactive A/B slot by pp-update on the
# phone, which reboots. If the phone does not answer it fails, rather than wait
# for the bootloader. --usb writes the whole image via USB mass storage: hold
# Volume Up while powering on (bootloader UMS mode), or enter FEL mode.
# Expects UBOOT, SCRIPT and PP_UPDATE to be set (done by the Nix wrapper).

FEL_ID="1f3a:efe8"   # Allwinner BROM in FEL mode
UMS_ID="1f3a:1010"   # U-Boot USB mass storage gadget
PHONE="root@172.16.42.1"
SSH_KEY="${PINEPHONE_SSH_KEY:-keys/pinephone}"

mode=ota
IMAGES_DIR=output/images
for arg in "$@"; do
  case "$arg" in
    --usb) mode=usb ;;
    --expose) mode=expose ;;
    -*) echo "Unknown option: $arg" >&2; exit 1 ;;
    *) IMAGES_DIR=$arg ;;
  esac
done

CONSOLE="${PINEPHONE_CONSOLE:-/dev/ttyACM0}"  # or COMn under WSL

ssh_phone() {
  ssh -i "$SSH_KEY" -o ConnectTimeout=3 -o BatchMode=yes \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
    "$PHONE" "$@"
}

# A phone running an image built with another keys/ (another checkout) refuses ours.
# Root has no password on the USB serial console: authorize our key there until the
# phone's next reboot. The root filesystem is read-only, so a copy of authorized_keys
# in /tmp is bind-mounted over it.
authorize_key() {
  local key cmd out logged_in=""
  key=$(cat "$SSH_KEY.pub") || return 1
  [[ "$key" != *"'"* ]] || return 1
  cmd="if ! grep -qxF '$key' /root/.ssh/authorized_keys; then { cat /root/.ssh/authorized_keys; echo '$key'; } > /tmp/pp-authorized_keys && chmod 600 /tmp/pp-authorized_keys && mount --bind /tmp/pp-authorized_keys /root/.ssh/authorized_keys; fi"
  # Under WSL the phone is usually attached to Windows (its network reaches WSL through
  # Windows): its console is a COM port there, not /dev/ttyACM0.
  if [[ "$CONSOLE" == COM* || ( ! -e "$CONSOLE" && -z "${PINEPHONE_CONSOLE:-}" ) ]] \
      && command -v powershell.exe >/dev/null; then
    authorize_key_windows "$cmd"
    return
  fi
  if [[ ! -e "$CONSOLE" ]]; then
    echo "No $CONSOLE to authorize the key there (PINEPHONE_CONSOLE to use another)." >&2
    return 1
  fi
  if [[ ! -w "$CONSOLE" ]]; then
    echo "Cannot write $CONSOLE to authorize the key there (dialout group?)." >&2
    return 1
  fi
  echo "The phone refuses $SSH_KEY (image built with other keys?), authorizing it over $CONSOLE until its next reboot."
  stty -F "$CONSOLE" raw -echo clocal 2>/dev/null
  exec 3<>"$CONSOLE" || return 1
  printf '\n' >&3
  out=$(timeout 1 cat <&3)
  if [[ "$out" == *"login:"* ]]; then
    printf 'root\n' >&3
    logged_in=1
    sleep 1
  fi
  printf "%s\n" "$cmd" >&3
  sleep 1
  [[ -z "$logged_in" ]] || printf 'exit\n' >&3
  exec 3>&-
}

# The same dialogue on a Windows COM port, from WSL: PINEPHONE_CONSOLE=COMn, or the port
# of the phone's USB gadget (1d6b:0104, its ACM interface). The command goes in an
# environment variable (WSLENV), the script has no double quotes (Windows' argument parsing).
authorize_key_windows() {
  local ps
  # shellcheck disable=SC2016  # PowerShell's variables
  ps='$ErrorActionPreference = '\''Stop'\''
$port = $env:PP_CONSOLE
if (-not $port) {
  $port = (Get-CimInstance Win32_SerialPort | Where-Object { $_.PNPDeviceID -like '\''USB\VID_1D6B&PID_0104*'\'' } | Select-Object -First 1).DeviceID
}
if (-not $port) { [Console]::Error.WriteLine('\''No COM port of the phone in Windows.'\''); exit 1 }
[Console]::Out.WriteLine($port)
$nl = [string][char]10
$p = New-Object System.IO.Ports.SerialPort $port, 115200
$p.DtrEnable = $true
$p.Open()
$p.Write($nl); Start-Sleep -Milliseconds 1000
$in = $p.ReadExisting() -like '\''*login:*'\''
if ($in) { $p.Write('\''root'\'' + $nl); Start-Sleep -Milliseconds 1000 }
$p.Write($env:PP_CMD + $nl); Start-Sleep -Milliseconds 1000
if ($in) { $p.Write('\''exit'\'' + $nl) }
$p.Close()'
  local port="" exe line
  [[ "$CONSOLE" == COM* ]] && port=$CONSOLE
  exe=(powershell.exe)
  # NixOS-WSL's systemd-binfmt drops WSL's own entry unless wsl.interop.register is set:
  # /init is the interpreter that entry names.
  [[ -e /proc/sys/fs/binfmt_misc/WSLInterop ]] || exe=(/init "$(command -v powershell.exe)")
  echo "The phone refuses $SSH_KEY (image built with other keys?), authorizing it over its Windows COM port until its next reboot."
  PP_CMD="$1" PP_CONSOLE="$port" WSLENV="${WSLENV:+$WSLENV:}PP_CMD:PP_CONSOLE" \
    "${exe[@]}" -NoProfile -NonInteractive -Command "$ps" | tr -d '\r' \
    | while IFS= read -r line; do echo "Console: $line"; done
}

# Whether the phone answers over SSH, authorizing our key over the console if refused.
phone_up() {
  local err
  err=$(ssh_phone true 2>&1) && return 0
  [[ "$err" == *"Permission denied"* ]] && authorize_key || return 1
  for _ in 1 2 3 4 5; do
    ssh_phone true 2>/dev/null && return 0
    sleep 1
  done
  echo "The phone still refuses $SSH_KEY." >&2
  return 1
}

if [[ "$mode" == ota ]]; then
  [[ -f "$SSH_KEY" ]] || { echo "SSH key not found: $SSH_KEY" >&2; exit 1; }
  if ! phone_up; then
    echo "Phone not reachable over the USB network ($PHONE)." >&2
    echo "Is it up and plugged in? Without our firmware running: just flash-usb" >&2
    exit 1
  fi
  ROOTFS="$IMAGES_DIR/rootfs.ext4"
  [[ -f "$ROOTFS" ]] || { echo "Image not found: $ROOTFS" >&2; exit 1; }
  echo "Phone is running, updating over the air with $ROOTFS"
  size=$(stat -L -c %s "$ROOTFS")  # rootfs.ext4 is a symlink to rootfs.ext2
  sha=$(sha256sum "$ROOTFS" | cut -d' ' -f1)
  old_slot=$(ssh_phone "sed -n 's/.*root=PARTUUID=50480001-0\([12]\).*/\1/p' /proc/cmdline")

  # Run the updater from this tree, not the possibly older copy on the phone.
  ssh_phone "cat > /tmp/pp-update" < "$PP_UPDATE"
  # gzip: the image is mostly ext4 metadata and slack, it compresses well.
  pv "$ROOTFS" | gzip -1 | ssh_phone "gunzip | sh /tmp/pp-update $size $sha"

  # The new slot boots on trial (S98pp-trial): wait until the phone is back
  # and the trial is settled, confirmed or rolled back.
  echo "Waiting for the phone to come back (up to 5 min)..."
  sleep 10
  deadline=$((SECONDS + 300))
  # A rollback boots the previous image again, which may have other keys.
  until phone_up 2>/dev/null && ssh_phone "test ! -e /var/lib/pp-update/trial" 2>/dev/null; do
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
