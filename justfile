# Run `nix develop` first to get just and the tools used below.

set positional-arguments

phone := "root@172.16.42.1"
ssh_opts := "-i keys/pinephone -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

default:
    @just --list

# Build the firmware into output/images/
build:
    nix run .#fhs -- -c make

# Run a Buildroot target in the build environment, e.g. `just make linux-rebuild`
make *args:
    nix run .#fhs -- -c 'make "$@"' make "$@"

# Configure Buildroot, then `just savedefconfig` to keep the changes
menuconfig:
    just make menuconfig

# Configure the kernel (save changes into board/pinephone/linux.fragment by hand)
linux-menuconfig:
    just make linux-menuconfig

# Write the Buildroot configuration back to external/configs/pinephone_defconfig
savedefconfig:
    just make savedefconfig

# Interactive shell in the build environment
shell:
    nix run .#fhs

# Flash: over the air if the phone is running our firmware, otherwise via USB
flash *args:
    nix run . -- "$@"

# Flash the full image via USB mass storage (Volume Up at power-on, or FEL)
flash-usb:
    nix run . -- --usb

# Only expose the eMMC as a USB disk, write nothing
expose:
    nix run . -- --expose

# Build and flash
deploy: build flash

# Push phone-ui from the working tree to the running phone, no flash; args are
# passed to it (e.g. `just ui --open Camera`). Kept across reboots until the next
# flash (see S90phone-ui); `just ui-reset` goes back to the installed one.
ui *args:
    #!/usr/bin/env bash
    set -euo pipefail
    # One push at a time (several agents), and never next to a full build
    exec 9>output/.ui.lock
    flock 9
    if pgrep -f 'make.*O=.*/output' >/dev/null; then
        echo "A Buildroot build is running in output/, wait for it." >&2; exit 1
    fi
    nix run .#fhs -- -c 'make phone-ui-rebuild' >output/.ui-build.log 2>&1 || {
        grep -E 'error|warning' output/.ui-build.log || tail -20 output/.ui-build.log; exit 1; }
    grep -E 'warning:' output/.ui-build.log | grep -v 'Git tree' || true
    info="$(git describe --always --dirty) $(date '+%F %T') $(whoami)"
    ssh {{ssh_opts}} {{phone}} "mkdir -p /var/lib/phone-ui/dev && cd /var/lib/phone-ui/dev &&
        gunzip > phone-ui.new && chmod 755 phone-ui.new && mv phone-ui.new phone-ui &&
        sha256sum /usr/bin/phone-ui | cut -d' ' -f1 > base &&
        echo '$info' > info && echo '$*' > args && sync &&
        /etc/init.d/S90phone-ui restart && sleep 2 &&
        grep -v 'shader\|glsl\|GLSL\|^ *lv_opengl\|^$' /tmp/phone-ui.log" \
        < <(gzip -1 < output/build/phone-ui-1.0/phone-ui)

# Build the external app in apps/<name> (its Makefile, with the firmware's toolchain
# in CC, installs into OUT; BUILD and DL for scratch and downloads) and install it in
# /var/lib/apps/<name>; restarts phone-ui
app name:
    #!/usr/bin/env bash
    set -euo pipefail
    out="$PWD/output/apps/{{name}}"
    rm -rf "$out"
    nix run .#fhs -- -c 'make -C "$1" CC="$2" OUT="$3" BUILD="$4" DL="$5"' make "apps/{{name}}" \
        "$PWD/output/host/bin/aarch64-linux-gcc" "$out" "$PWD/output/apps-build/{{name}}" "$PWD/dl/apps"
    # Unpacked next to it (hidden: not listed), then swapped
    tar -C "$out" -c . | gzip -1 | ssh {{ssh_opts}} {{phone}} "set -e; cd /var/lib && mkdir -p apps &&
        rm -rf apps/.{{name}}.new && mkdir apps/.{{name}}.new && gunzip | tar -xo -C apps/.{{name}}.new &&
        rm -rf apps/{{name}} && mv apps/.{{name}}.new apps/{{name}} && sync &&
        /etc/init.d/S90phone-ui restart && sleep 2 && { grep 'external app' /tmp/phone-ui.log || true; }"

# Remove an external app from the phone; restarts phone-ui
app-rm name:
    ssh {{ssh_opts}} {{phone}} 'rm -rf /var/lib/apps/{{name}} && sync && /etc/init.d/S90phone-ui restart'

# Drop the development build pushed by `just ui`, run the installed phone-ui
ui-reset:
    ssh {{ssh_opts}} {{phone}} 'rm -rf /var/lib/phone-ui/dev && /etc/init.d/S90phone-ui restart'

# iptables, not nft: Docker's FORWARD policy is DROP, the rules go before it.
# Lasts until `just unshare-internet` or the host reboots.
# Share the host's internet connection with the phone over USB (asks for sudo)
share-internet:
    #!/usr/bin/env bash
    set -euo pipefail
    iface=$(ip -o link | awk -F': ' '/02:50:48:00:00:02/ { print $2 }')
    [ -n "$iface" ] || { echo "The phone's USB network is not up." >&2; exit 1; }
    sudo sysctl -q -w net.ipv4.ip_forward=1
    rule() { sudo iptables -t "$1" -C "${@:2}" 2>/dev/null || sudo iptables -t "$1" -I "${@:2}"; }
    rule nat POSTROUTING -s 172.16.42.0/24 ! -o "$iface" -j MASQUERADE
    rule filter FORWARD -i "$iface" -j ACCEPT
    rule filter FORWARD -o "$iface" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
    echo "Sharing the internet connection with the phone ($iface)."

# Stop sharing the internet connection (IP forwarding stays on)
unshare-internet:
    #!/usr/bin/env bash
    set -euo pipefail
    iface=$(ip -o link | awk -F': ' '/02:50:48:00:00:02/ { print $2 }')
    [ -n "$iface" ] || { echo "The phone's USB network is not up." >&2; exit 1; }
    sudo iptables -t nat -D POSTROUTING -s 172.16.42.0/24 ! -o "$iface" -j MASQUERADE || true
    sudo iptables -t filter -D FORWARD -i "$iface" -j ACCEPT || true
    sudo iptables -t filter -D FORWARD -o "$iface" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT || true

# Follow phone-ui's output (quit: Ctrl-c)
ui-log:
    ssh {{ssh_opts}} {{phone}} tail -n 50 -f /tmp/phone-ui.log

# SSH into the phone over the USB network
ssh *args:
    ssh {{ssh_opts}} {{phone}} "$@"

# Serial console over the USB gadget, reconnects across reboots (quit: Ctrl-t q)
console dev="/dev/ttyACM0":
    tio {{dev}}

# Follow the kernel log over SSH, from the start of each boot, across reboots (quit: Ctrl-c)
logs:
    #!/usr/bin/env bash
    while true; do
        # /dev/kmsg lines are "prio,seq,usec,flags;message", continuation lines start with a space
        ssh {{ssh_opts}} -o ConnectTimeout=2 -o ServerAliveInterval=2 {{phone}} cat /dev/kmsg 2>/dev/null \
            | grep --line-buffered -v '^ ' | sed -u 's/^[^;]*;//'
        echo "--- phone not reachable, waiting ---"
        until ssh {{ssh_opts}} -o ConnectTimeout=2 -o BatchMode=yes {{phone}} true 2>/dev/null; do sleep 1; done
    done
