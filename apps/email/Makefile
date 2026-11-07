# `just app email`: a plugin app, email.so, that phone-ui loads and runs in its own
# process with its UI (external.c), built against the headers phone-ui installed in
# STAGING (ui.h, json.h, abi.h, LVGL's). The mail servers are reached through
# phone-ui's libcurl (ui_fetch_mail): nothing of its own in lib/.

CC ?= cc
OUT ?= out
BUILD ?= $(OUT)
STAGING ?= /
# The cross strip next to CC (the build environment exports the host's in STRIP)
STRIP = $(patsubst %gcc,%strip,$(CC))

CFLAGS = -O2 -Wall -fPIC -DLV_CONF_INCLUDE_SIMPLE -DUI_APP_CATALOG \
	-I$(STAGING)/usr/include/phone-ui -I$(STAGING)/usr/include/lvgl -I$(STAGING)/usr/include

all:
	mkdir -p $(OUT) $(BUILD)
	python3 $(STAGING)/usr/share/phone-ui/gen-translations.py --app po $(BUILD)/i18n.c
	$(CC) $(CFLAGS) -shared -o $(OUT)/email.so *.c $(BUILD)/i18n.c
	$(STRIP) $(OUT)/email.so
	cp app.json $(OUT)/
