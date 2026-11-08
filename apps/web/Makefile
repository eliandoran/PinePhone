# `just app web`: the Browser plugin app, web.so, built against the headers phone-ui
# installed in STAGING and WebKit's, with the web engine in root/: Buildroot builds
# it but leaves it out of the image (external/board/pinephone/web-engine.sh, which
# also links the folders WebKit reads at compiled-in paths to /var/lib/apps/web/root).
# web.so finds the engine's libraries through its rpath, and they and WebKit's
# processes each other through runpaths web-engine.sh sets.

CC ?= cc
OUT ?= out
BUILD ?= $(OUT)
STAGING ?= /
# Buildroot's output folder, and its tools next to CC
BR_OUTPUT = $(STAGING:%/staging=%)
PKG_CONFIG = $(dir $(CC))pkg-config
PATCHELF = $(dir $(CC))patchelf
# The cross strip (the build environment exports the host's in STRIP)
STRIP = $(patsubst %gcc,%strip,$(CC))
ENGINE = $(BR_OUTPUT)/../external/board/pinephone/web-engine.sh

PKGS = wpe-webkit-2.0 wpebackend-fdo-1.0
# WebKit's headers as the system's: libwpe's declarations without prototypes not warned about
CFLAGS = -O2 -std=gnu17 -D_FORTIFY_SOURCE=3 -Wall -Wextra -Wno-missing-field-initializers -Wshadow -Wvla -Wimplicit-fallthrough -Wstrict-prototypes -Wmissing-prototypes -fPIC -DLV_CONF_INCLUDE_SIMPLE -DUI_APP_CATALOG \
	-I$(STAGING)/usr/include/phone-ui -I$(STAGING)/usr/include/lvgl \
	$(patsubst -I%,-isystem %,$(shell $(PKG_CONFIG) --cflags $(PKGS)))
LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS)) -lEGL -lGLESv2

all:
	mkdir -p $(OUT) $(BUILD)
	python3 $(STAGING)/usr/share/phone-ui/gen-translations.py --app po $(BUILD)/i18n.c
	$(CC) $(CFLAGS) -shared -Wl,-rpath,'$$ORIGIN/root/usr/lib' -o $(OUT)/web.so web.c $(BUILD)/i18n.c $(LIBS)
	$(STRIP) $(OUT)/web.so
	$(ENGINE) bundle $(BR_OUTPUT)/target $(BR_OUTPUT)/build $(OUT)/root $(PATCHELF)
	cp app.json $(OUT)/
