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
