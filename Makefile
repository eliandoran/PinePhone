# Thin wrapper around Buildroot with our BR2_EXTERNAL tree.
# Run inside the build environment: nix run .#fhs  (then: make)
#
#   make                 build output/images/pinephone.img
#   make menuconfig      any other target is passed through to Buildroot
#   make savedefconfig   writes back to external/configs/pinephone_defconfig

BR_VERSION := 2026.08.1
BR_SHA256  := 34fb7bea8a213ffde39cc02e61703391560f26e0850f03138c722d0b09880f2b
BR_TARBALL := buildroot-$(BR_VERSION).tar.xz

TOP       := $(CURDIR)
BR_DIR    := $(TOP)/buildroot
O         := $(TOP)/output
EXTERNAL  := $(TOP)/external
DEFCONFIG := pinephone_defconfig

# Keep downloads outside the Buildroot tree so a Buildroot upgrade reuses them.
export BR2_DL_DIR := $(TOP)/dl

BR_MAKE = $(MAKE) -C $(BR_DIR) BR2_EXTERNAL=$(EXTERNAL) O=$(O)

all: $(O)/.config keys/pinephone
	$(BR_MAKE)

# Development SSH key for root on the phone, installed by post-build.sh and used
# by the flasher for over-the-air updates. Not committed.
keys/pinephone:
	mkdir -p keys
	ssh-keygen -q -t ed25519 -N "" -C pinephone-dev -f $@

$(O)/.config: $(EXTERNAL)/configs/$(DEFCONFIG) | $(BR_DIR)/.version-$(BR_VERSION)
	$(BR_MAKE) $(DEFCONFIG)

$(BR_DIR)/.version-$(BR_VERSION):
	mkdir -p $(BR2_DL_DIR)
	test -f $(BR2_DL_DIR)/$(BR_TARBALL) || \
		wget -O $(BR2_DL_DIR)/$(BR_TARBALL) https://buildroot.org/downloads/$(BR_TARBALL)
	echo "$(BR_SHA256)  $(BR2_DL_DIR)/$(BR_TARBALL)" | sha256sum -c
	rm -rf $(BR_DIR) && mkdir -p $(BR_DIR)
	tar -xf $(BR2_DL_DIR)/$(BR_TARBALL) -C $(BR_DIR) --strip-components=1
	touch $@

savedefconfig: $(O)/.config
	$(BR_MAKE) savedefconfig BR2_DEFCONFIG=$(EXTERNAL)/configs/$(DEFCONFIG)

%: $(O)/.config
	$(BR_MAKE) $@

.PHONY: all savedefconfig
