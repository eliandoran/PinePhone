# `just app trilium`: a plugin app, trilium.so, that phone-ui loads and runs in its own
# process with its UI (external.c), built against the headers phone-ui installed in
# STAGING (ui.h, i18n.h, abi.h, LVGL's). Its database is SQLite, which Buildroot builds
# for WebKit but leaves out of the image (web-engine.sh): it goes in lib/, found through
# trilium.so's runpath. Trilium's schema.sql in C (gen-schema.sh), its texts in
# phone-ui's language: po/'s catalogs in C (staging's gen-translations.py --app).

CC ?= cc
OUT ?= out
BUILD ?= $(OUT)
STAGING ?= /
# The cross strip next to CC (the build environment exports the host's in STRIP)
STRIP = $(patsubst %gcc,%strip,$(CC))

CFLAGS = -O2 -std=gnu17 -D_FORTIFY_SOURCE=3 -Wall -Wextra -Wno-missing-field-initializers -Wshadow -Wvla -Wimplicit-fallthrough -Wstrict-prototypes -Wmissing-prototypes -fPIC -DLV_CONF_INCLUDE_SIMPLE -DUI_APP_CATALOG \
	-I. -I$(STAGING)/usr/include/phone-ui -I$(STAGING)/usr/include/lvgl -I$(STAGING)/usr/include

all:
	mkdir -p $(OUT)/lib $(BUILD)
	python3 $(STAGING)/usr/share/phone-ui/gen-translations.py --app po $(BUILD)/i18n.c
	sh gen-schema.sh schema.sql $(BUILD)/schema.c
	$(CC) $(CFLAGS) -shared -Wl,-rpath,'$$ORIGIN/lib' -o $(OUT)/trilium.so *.c $(BUILD)/i18n.c $(BUILD)/schema.c \
		-L$(STAGING)/usr/lib -lsqlite3
	$(STRIP) $(OUT)/trilium.so
	cp -a $(STAGING)/usr/lib/libsqlite3.so* $(OUT)/lib/
	$(STRIP) $(OUT)/lib/libsqlite3.so.*.*
	cp app.json $(OUT)/
