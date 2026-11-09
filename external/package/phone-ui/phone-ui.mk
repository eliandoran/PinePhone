################################################################################
#
# phone-ui
#
################################################################################

PHONE_UI_VERSION = 1.0
PHONE_UI_SITE = $(PHONE_UI_PKGDIR)/src
PHONE_UI_SITE_METHOD = local
PHONE_UI_LICENSE = MIT
PHONE_UI_DEPENDENCIES = host-python3 lvgl libdrm libpng libcurl jpeg webp litehtml mpg123 alsa-lib zlib dbus

# The copy into the build directory mirrors src/: a file removed or moved there goes from
# the copy too (rsync only adds otherwise), keeping Buildroot's stamps and file lists, and
# the build's objects and generated sources (Makefile.build compiles only what changed).
# The justfile's `ui` copies as this does
PHONE_UI_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --delete --exclude='/.stamp_*' --exclude='/.files-list*' \
	--exclude=/obj --exclude=/abi.h --exclude=/exports.list --exclude=/i18n_languages.c --exclude=/words.c

ifeq ($(BR2_PACKAGE_HAS_LIBGBM),y)
PHONE_UI_DEPENDENCIES += libgbm
PHONE_UI_LIBS = -lgbm -lGLESv2 -lEGL
endif

# Plugin apps (external.c) are built against ui.h, json.h, i18n.h and LVGL's headers,
# installed in staging with abi.h: UI_ABI, a fingerprint of each of their declarations
# and one of LVGL's (gen-abi.py), each of which a plugin's must still match (a
# declaration added breaks no plugin); ui_symbols.h too (the fonts' icons), not in it.
# phone-ui exports what those headers declare and all of LVGL (exports.list), which they call.
# gen-translations.py goes there too: a plugin's catalogs (--app).
# The translations: po/'s catalogs in C (gen-translations.py), compiled with the rest.
# Makefile.build generates both, compiles the sources that changed, in parallel, and links;
# it saves these variables for `just ui`, which runs it without Buildroot
define PHONE_UI_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) --output-sync=target -C $(@D) -f $(PHONE_UI_PKGDIR)/Makefile.build \
		CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" CFLAGS="$(TARGET_CFLAGS)" CXXFLAGS="$(TARGET_CXXFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" STAGING="$(STAGING_DIR)" PYTHON="$(HOST_DIR)/bin/python3" \
		PKGDIR="$(PHONE_UI_PKGDIR)" \
		LIBS="$(PHONE_UI_LIBS) -lcurl -lpng -ljpeg -lwebp -ldrm -llitehtml -lgumbo -lmpg123 -lasound -ldbus-1 -lz -lstdc++ -lpthread -lm"
endef

PHONE_UI_INSTALL_STAGING = YES
define PHONE_UI_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 -t $(STAGING_DIR)/usr/include/phone-ui $(@D)/ui.h $(@D)/json.h $(@D)/i18n.h \
		$(@D)/abi.h $(@D)/ui_symbols.h
	$(INSTALL) -D -m 0644 -t $(STAGING_DIR)/usr/include/phone-ui/ui $(@D)/ui/rich_text.h $(@D)/ui/rich_edit.h
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
# a translation, a word list, this file, Makefile.build or the init script is newer than the copy, and build again
# after a copy; installing and the image follow from the build stamp
PHONE_UI_SOURCES := $(shell find $(PHONE_UI_SITE)) $(PHONE_UI_PKGDIR)/phone-ui.mk \
	$(PHONE_UI_PKGDIR)/Makefile.build $(PHONE_UI_PKGDIR)/S90phone-ui $(PHONE_UI_PKGDIR)/gen-translations.py \
	$(PHONE_UI_PKGDIR)/gen-abi.py $(wildcard $(PHONE_UI_PKGDIR)/po/*.po) \
	$(PHONE_UI_PKGDIR)/gen-words.py $(wildcard $(PHONE_UI_PKGDIR)/words/*.txt)
$(PHONE_UI_TARGET_RSYNC): $(PHONE_UI_SOURCES)
$(PHONE_UI_TARGET_BUILD): $(PHONE_UI_TARGET_RSYNC)
