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
# the copy too (rsync only adds otherwise), keeping Buildroot's stamps and file lists, and
# the build's objects and generated sources (Makefile.build compiles only what changed)
PHONE_UI_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --delete --exclude='/.stamp_*' --exclude='/.files-list*' \
	--exclude=/obj --exclude=/abi.h --exclude=/i18n_languages.c

ifeq ($(BR2_PACKAGE_HAS_LIBGBM),y)
PHONE_UI_DEPENDENCIES += libgbm
PHONE_UI_LIBS = -lgbm -lGLESv2 -lEGL
endif

# Plugin apps (external.c) are built against ui.h, json.h, i18n.h and LVGL's headers,
# installed in staging with abi.h: UI_ABI, a hash of them, which a plugin must match;
# ui_symbols.h too (the fonts' icons), not hashed: a symbol added breaks no plugin.
# phone-ui exports its functions (-rdynamic) and links all of LVGL, which they call.
# gen-translations.py goes there too: a plugin's catalogs (--app).
# The translations: po/'s catalogs in C (gen-translations.py), compiled with the rest.
# Both generated files are rewritten only when they change, or what includes them would
# compile again. Makefile.build compiles the sources that changed, in parallel, and links
define PHONE_UI_BUILD_CMDS
	{ cat $(@D)/ui.h $(@D)/json.h $(@D)/i18n.h; find $(STAGING_DIR)/usr/include/lvgl -name '*.h' | LC_ALL=C sort | xargs cat; } \
		| sha256sum | cut -c1-16 | sed 's/.*/#define UI_ABI "&"/' > $(@D)/abi.h.new
	if cmp -s $(@D)/abi.h.new $(@D)/abi.h; then rm $(@D)/abi.h.new; else mv $(@D)/abi.h.new $(@D)/abi.h; fi
	$(HOST_DIR)/bin/python3 $(PHONE_UI_PKGDIR)/gen-translations.py $(PHONE_UI_PKGDIR)/po $(@D)/i18n_languages.c
	$(TARGET_MAKE_ENV) $(MAKE) --output-sync=target -C $(@D) -f $(PHONE_UI_PKGDIR)/Makefile.build \
		CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" CFLAGS="$(TARGET_CFLAGS)" CXXFLAGS="$(TARGET_CXXFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" STAGING="$(STAGING_DIR)" \
		LIBS="$(PHONE_UI_LIBS) -lcurl -lpng -ljpeg -ldrm -llitehtml -lgumbo -lmpg123 -lasound -lz -lstdc++ -lpthread -lm"
endef

PHONE_UI_INSTALL_STAGING = YES
define PHONE_UI_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 -t $(STAGING_DIR)/usr/include/phone-ui $(@D)/ui.h $(@D)/json.h $(@D)/i18n.h \
		$(@D)/abi.h $(@D)/ui_symbols.h
	$(INSTALL) -D -m 0644 $(PHONE_UI_PKGDIR)/gen-translations.py \
		$(STAGING_DIR)/usr/share/phone-ui/gen-translations.py
endef

define PHONE_UI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/phone-ui $(TARGET_DIR)/usr/bin/phone-ui
	$(INSTALL) -D -m 0755 $(PHONE_UI_PKGDIR)/S90phone-ui \
		$(TARGET_DIR)/etc/init.d/S90phone-ui
endef

$(eval $(generic-package))

# Buildroot only builds a local package once: its steps check that the previous stamp
# exists, not its age. Copy again when a source (file or directory, so removals count),
# a translation, this file, Makefile.build or the init script is newer than the copy, and build again
# after a copy; installing and the image follow from the build stamp
PHONE_UI_SOURCES := $(shell find $(PHONE_UI_SITE)) $(PHONE_UI_PKGDIR)/phone-ui.mk \
	$(PHONE_UI_PKGDIR)/Makefile.build $(PHONE_UI_PKGDIR)/S90phone-ui $(PHONE_UI_PKGDIR)/gen-translations.py \
	$(wildcard $(PHONE_UI_PKGDIR)/po/*.po)
$(PHONE_UI_TARGET_RSYNC): $(PHONE_UI_SOURCES)
$(PHONE_UI_TARGET_BUILD): $(PHONE_UI_TARGET_RSYNC)
