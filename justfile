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

# Serial console over the USB gadget
console:
    picocom -b 115200 /dev/ttyACM0
