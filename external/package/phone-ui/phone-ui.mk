################################################################################
#
# phone-ui
#
################################################################################

PHONE_UI_VERSION = 1.0
PHONE_UI_SITE = $(PHONE_UI_PKGDIR)/src
PHONE_UI_SITE_METHOD = local
PHONE_UI_LICENSE = MIT
PHONE_UI_DEPENDENCIES = lvgl libdrm libpng libcurl jpeg litehtml

ifeq ($(BR2_PACKAGE_HAS_LIBGBM),y)
PHONE_UI_DEPENDENCIES += libgbm
PHONE_UI_LIBS = -lgbm -lGLESv2 -lEGL
endif

# Web: WPE WebKit, with the flags of its pkg-config files (read when building)
ifeq ($(BR2_PACKAGE_WPEWEBKIT),y)
PHONE_UI_DEPENDENCIES += wpewebkit
PHONE_UI_WPE_PKGS = wpe-webkit-2.0 wpebackend-fdo-1.0
PHONE_UI_WPE_CFLAGS = -DPHONE_UI_WPE=1 `$(PKG_CONFIG_HOST_BINARY) --cflags $(PHONE_UI_WPE_PKGS)`
PHONE_UI_WPE_LIBS = `$(PKG_CONFIG_HOST_BINARY) --libs $(PHONE_UI_WPE_PKGS)`
endif

# Plugin apps (external.c) are built against ui.h, json.h and LVGL's headers, installed
# in staging with abi.h: UI_ABI, a hash of them, which a plugin must match. phone-ui
# exports its functions (-rdynamic) and links all of LVGL, which they call.
# The browser's litehtml container is C++, the rest C; web/ alone sees WebKit's headers
define PHONE_UI_BUILD_CMDS
	{ cat $(@D)/ui.h $(@D)/json.h; find $(STAGING_DIR)/usr/include/lvgl -name '*.h' | LC_ALL=C sort | xargs cat; } \
		| sha256sum | cut -c1-16 | sed 's/.*/#define UI_ABI "&"/' > $(@D)/abi.h
	for f in $(@D)/browser/*.cpp; do \
		$(TARGET_CXX) $(TARGET_CXXFLAGS) -O2 -Wall -std=c++17 -ffunction-sections -fdata-sections \
			-DLV_CONF_INCLUDE_SIMPLE -I$(STAGING_DIR)/usr/include/lvgl \
			-c $$f -o $${f%.cpp}.o || exit 1; \
	done
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -Wall -DLV_CONF_INCLUDE_SIMPLE -I$(STAGING_DIR)/usr/include/lvgl \
		$(PHONE_UI_WPE_CFLAGS) -c $(@D)/web/web.c -o $(@D)/web/web.o
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -O2 -Wall \
		-DLV_CONF_INCLUDE_SIMPLE -I$(STAGING_DIR)/usr/include/lvgl -I$(STAGING_DIR)/usr/include/libdrm \
		-rdynamic -Wl,--gc-sections -o $(@D)/phone-ui $(@D)/*.c $(@D)/clock/*.c \
		$(@D)/browser/*.c $(@D)/browser/*.o $(@D)/web/web.o \
		-Wl,--whole-archive -llvgl -Wl,--no-whole-archive \
		$(PHONE_UI_LIBS) $(PHONE_UI_WPE_LIBS) -lcurl -lpng -ljpeg -ldrm -llitehtml -lgumbo \
		-lstdc++ -lpthread -lm
endef

PHONE_UI_INSTALL_STAGING = YES
define PHONE_UI_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 -t $(STAGING_DIR)/usr/include/phone-ui $(@D)/ui.h $(@D)/json.h $(@D)/abi.h
endef

define PHONE_UI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/phone-ui $(TARGET_DIR)/usr/bin/phone-ui
	$(INSTALL) -D -m 0755 $(PHONE_UI_PKGDIR)/S90phone-ui \
		$(TARGET_DIR)/etc/init.d/S90phone-ui
endef

$(eval $(generic-package))
