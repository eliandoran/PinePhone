################################################################################
#
# phone-ui
#
################################################################################

PHONE_UI_VERSION = 1.0
PHONE_UI_SITE = $(PHONE_UI_PKGDIR)/src
PHONE_UI_SITE_METHOD = local
PHONE_UI_LICENSE = MIT
PHONE_UI_DEPENDENCIES = host-python3 lvgl libdrm libpng libcurl jpeg litehtml mpg123 alsa-lib zlib

# The copy into the build directory mirrors src/: a file removed or moved there goes from
# the copy too (rsync only adds otherwise), keeping Buildroot's stamps and file lists
PHONE_UI_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --delete --exclude='/.stamp_*' --exclude='/.files-list*'

ifeq ($(BR2_PACKAGE_HAS_LIBGBM),y)
PHONE_UI_DEPENDENCIES += libgbm
PHONE_UI_LIBS = -lgbm -lGLESv2 -lEGL
endif

# Plugin apps (external.c) are built against ui.h, json.h and LVGL's headers, installed
# in staging with abi.h: UI_ABI, a hash of them, which a plugin must match. phone-ui
# exports its functions (-rdynamic) and links all of LVGL, which they call.
# The translations: po/'s catalogs in C (gen-translations.py), compiled with the rest.
# The browser's litehtml container is C++, the rest C
define PHONE_UI_BUILD_CMDS
	{ cat $(@D)/ui.h $(@D)/json.h; find $(STAGING_DIR)/usr/include/lvgl -name '*.h' | LC_ALL=C sort | xargs cat; } \
		| sha256sum | cut -c1-16 | sed 's/.*/#define UI_ABI "&"/' > $(@D)/abi.h
	$(HOST_DIR)/bin/python3 $(PHONE_UI_PKGDIR)/gen-translations.py $(PHONE_UI_PKGDIR)/po $(@D)/i18n_languages.c
	for f in $(@D)/browser/*.cpp; do \
		$(TARGET_CXX) $(TARGET_CXXFLAGS) -O2 -Wall -std=c++17 -ffunction-sections -fdata-sections \
			-DLV_CONF_INCLUDE_SIMPLE -I$(STAGING_DIR)/usr/include/lvgl \
			-c $$f -o $${f%.cpp}.o || exit 1; \
	done
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -O2 -Wall \
		-DLV_CONF_INCLUDE_SIMPLE -I$(STAGING_DIR)/usr/include/lvgl -I$(STAGING_DIR)/usr/include/libdrm \
		-rdynamic -Wl,--gc-sections -o $(@D)/phone-ui $(@D)/*.c $(@D)/clock/*.c $(@D)/settings/*.c $(@D)/assistant/*.c \
		$(@D)/browser/*.c $(@D)/browser/*.o $(@D)/books/*.c \
		-Wl,--whole-archive -llvgl -Wl,--no-whole-archive \
		$(PHONE_UI_LIBS) -lcurl -lpng -ljpeg -ldrm -llitehtml -lgumbo -lmpg123 -lasound -lz \
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

# Buildroot only builds a local package once: its steps check that the previous stamp
# exists, not its age. Copy again when a source (file or directory, so removals count),
# a translation, this file or the init script is newer than the copy, and build again
# after a copy; installing and the image follow from the build stamp
PHONE_UI_SOURCES := $(shell find $(PHONE_UI_SITE)) $(PHONE_UI_PKGDIR)/phone-ui.mk \
	$(PHONE_UI_PKGDIR)/S90phone-ui $(PHONE_UI_PKGDIR)/gen-translations.py \
	$(wildcard $(PHONE_UI_PKGDIR)/po/*.po)
$(PHONE_UI_TARGET_RSYNC): $(PHONE_UI_SOURCES)
$(PHONE_UI_TARGET_BUILD): $(PHONE_UI_TARGET_RSYNC)
