################################################################################
#
# lvgl-bench
#
################################################################################

LVGL_BENCH_VERSION = 1.0
LVGL_BENCH_SITE = $(LVGL_BENCH_PKGDIR)/src
LVGL_BENCH_SITE_METHOD = local
LVGL_BENCH_LICENSE = MIT
LVGL_BENCH_DEPENDENCIES = lvgl libdrm

ifeq ($(BR2_PACKAGE_HAS_LIBGBM),y)
LVGL_BENCH_DEPENDENCIES += libgbm
LVGL_BENCH_LIBS = -lgbm
endif

define LVGL_BENCH_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -O2 -fno-omit-frame-pointer \
		-DLV_CONF_INCLUDE_SIMPLE -I$(STAGING_DIR)/usr/include/lvgl \
		-Wl,--gc-sections -o $(@D)/lvgl-bench $(@D)/main.c \
		-llvgl $(LVGL_BENCH_LIBS) -ldrm -lpthread -lm
endef

define LVGL_BENCH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/lvgl-bench $(TARGET_DIR)/usr/bin/lvgl-bench
	$(INSTALL) -D -m 0755 $(LVGL_BENCH_PKGDIR)/S90lvgl-bench \
		$(TARGET_DIR)/etc/init.d/S90lvgl-bench
endef

$(eval $(generic-package))
