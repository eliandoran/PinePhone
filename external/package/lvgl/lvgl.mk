################################################################################
#
# lvgl
#
################################################################################

LVGL_VERSION = 9.6.0
LVGL_SITE = $(call github,lvgl,lvgl,v$(LVGL_VERSION))
LVGL_LICENSE = MIT
LVGL_LICENSE_FILES = LICENCE.txt
LVGL_INSTALL_STAGING = YES
# Static library only: nothing to install on the target
LVGL_INSTALL_TARGET = NO
LVGL_DEPENDENCIES = libdrm

LVGL_MAKE_OPTS = \
	CC="$(TARGET_CC)" AR="$(TARGET_AR)" \
	CFLAGS="$(TARGET_CFLAGS) -I$(STAGING_DIR)/usr/include/libdrm" \
	LV_CONF=$(LVGL_PKGDIR)/lv_conf.h

define LVGL_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f $(LVGL_PKGDIR)/Makefile.lib $(LVGL_MAKE_OPTS)
endef

define LVGL_INSTALL_STAGING_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f $(LVGL_PKGDIR)/Makefile.lib $(LVGL_MAKE_OPTS) \
		DESTDIR=$(STAGING_DIR) install
endef

$(eval $(generic-package))
