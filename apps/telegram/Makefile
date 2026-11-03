# `just app telegram`: a plugin app, telegram.so, that phone-ui loads and runs in
# its own process with its UI (external.c), built against the headers phone-ui
# installed in STAGING (ui.h, json.h, abi.h, LVGL's). TDLib's libtdjson goes in
# lib/, found through telegram.so's runpath when the app opens.

CC ?= cc
OUT ?= out
STAGING ?= /
# The cross strip next to CC (the build environment exports the host's in STRIP)
STRIP = $(patsubst %gcc,%strip,$(CC))

CFLAGS = -O2 -Wall -fPIC -DLV_CONF_INCLUDE_SIMPLE \
	-I$(STAGING)/usr/include/phone-ui -I$(STAGING)/usr/include/lvgl

all:
	mkdir -p $(OUT)/lib
	$(CC) $(CFLAGS) -shared -Wl,-rpath,'$$ORIGIN/lib' -o $(OUT)/telegram.so *.c -lm
	$(STRIP) $(OUT)/telegram.so
	cp -a $(STAGING)/usr/lib/libtdjson.so* $(OUT)/lib/
	$(STRIP) $(OUT)/lib/libtdjson.so.*.*
	cp app.json $(OUT)/
