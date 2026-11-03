################################################################################
#
# ov5640-firmware
#
################################################################################

# The copy postmarketOS packages (firmware-pine64-ov5640)
OV5640_FIRMWARE_VERSION = 61beaa4eb1ad87ad067cfbe123fbcd0a0cf01246
OV5640_FIRMWARE_SITE = $(call github,pmsourcedump,ov5640,$(OV5640_FIRMWARE_VERSION))
OV5640_FIRMWARE_LICENSE = PROPRIETARY

define OV5640_FIRMWARE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/ov5640_af.bin $(TARGET_DIR)/lib/firmware/ov5640_af.bin
endef

$(eval $(generic-package))
