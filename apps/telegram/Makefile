# `just app telegram`: a plugin app, telegram.so, that phone-ui loads and runs in
# its own process with its UI (external.c), built against the headers phone-ui
# installed in STAGING (ui.h, json.h, abi.h, LVGL's). TDLib's libtdjson goes in
# lib/, found through telegram.so's runpath when the app opens, with libwebp
# (stickers; built for WebKit, which is not in the image) and its libsharpyuv,
# linked here so that it is loaded from lib/ before libwebp needs it.

CC ?= cc
OUT ?= out
BUILD ?= $(OUT)
STAGING ?= /
# The cross strip next to CC (the build environment exports the host's in STRIP)
STRIP = $(patsubst %gcc,%strip,$(CC))

CFLAGS = -O2 -D_FORTIFY_SOURCE=3 -Wall -Wextra -Wno-missing-field-initializers -Wshadow -Wvla -Wimplicit-fallthrough -Wstrict-prototypes -Wmissing-prototypes -fPIC -DLV_CONF_INCLUDE_SIMPLE -DUI_APP_CATALOG \
	-I$(STAGING)/usr/include/phone-ui -I$(STAGING)/usr/include/lvgl -I$(STAGING)/usr/include

all:
	mkdir -p $(OUT)/lib $(BUILD)
	python3 $(STAGING)/usr/share/phone-ui/gen-translations.py --app po $(BUILD)/i18n.c
	$(CC) $(CFLAGS) -shared -Wl,-rpath,'$$ORIGIN/lib' -o $(OUT)/telegram.so *.c $(BUILD)/i18n.c -lm \
		-L$(STAGING)/usr/lib -lwebp -Wl,--no-as-needed -lsharpyuv
	$(STRIP) $(OUT)/telegram.so
	cp -a $(STAGING)/usr/lib/libtdjson.so* $(OUT)/lib/
	cp -a $(STAGING)/usr/lib/libwebp.so.7* $(STAGING)/usr/lib/libsharpyuv.so.0* $(OUT)/lib/
	$(STRIP) $(OUT)/lib/libtdjson.so.*.* $(OUT)/lib/libwebp.so.7.* $(OUT)/lib/libsharpyuv.so.0.*
	cp app.json $(OUT)/
