################################################################################
#
# phone-ui
#
################################################################################

PHONE_UI_VERSION = 1.0
PHONE_UI_SITE = $(PHONE_UI_PKGDIR)/src
PHONE_UI_SITE_METHOD = local
PHONE_UI_LICENSE = MIT
PHONE_UI_DEPENDENCIES = lvgl libdrm libpng libcurl jpeg

ifeq ($(BR2_PACKAGE_HAS_LIBGBM),y)
PHONE_UI_DEPENDENCIES += libgbm
PHONE_UI_LIBS = -lgbm -lGLESv2 -lEGL
endif

define PHONE_UI_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -O2 -Wall \
		-DLV_CONF_INCLUDE_SIMPLE -I$(STAGING_DIR)/usr/include/lvgl \
		-Wl,--gc-sections -o $(@D)/phone-ui $(@D)/*.c $(@D)/clock/*.c $(@D)/telegram/*.c \
		-llvgl $(PHONE_UI_LIBS) -lcurl -lpng -ljpeg -ldrm -lpthread -lm
endef

define PHONE_UI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/phone-ui $(TARGET_DIR)/usr/bin/phone-ui
	$(INSTALL) -D -m 0755 $(PHONE_UI_PKGDIR)/S90phone-ui \
		$(TARGET_DIR)/etc/init.d/S90phone-ui
endef

$(eval $(generic-package))
