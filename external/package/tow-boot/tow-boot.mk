################################################################################
#
# tow-boot
#
################################################################################

# The release's binary, as Tow-Boot builds it (with Nix): Tow-Boot.noenv.bin is the
# one for a card shared with a system (its own shared.disk-image.img puts it at 8 KiB),
# its environment not saved on the disk. Its source: the release's tag.
TOW_BOOT_VERSION = 2023.07-007
TOW_BOOT_SOURCE = pine64-pinephoneA64-$(TOW_BOOT_VERSION).tar.xz
TOW_BOOT_SITE = https://github.com/Tow-Boot/Tow-Boot/releases/download/release-$(TOW_BOOT_VERSION)
TOW_BOOT_LICENSE = GPL-2.0+
TOW_BOOT_INSTALL_TARGET = NO
TOW_BOOT_INSTALL_IMAGES = YES

define TOW_BOOT_INSTALL_IMAGES_CMDS
	$(INSTALL) -D -m 0644 $(@D)/binaries/Tow-Boot.noenv.bin \
		$(BINARIES_DIR)/Tow-Boot.noenv.bin
endef

$(eval $(generic-package))
