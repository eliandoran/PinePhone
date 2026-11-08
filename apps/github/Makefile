# `just app github`: a plugin app, github.so, that phone-ui loads and runs in its
# own process with its UI (external.c), built against the headers phone-ui installed
# in STAGING (ui.h, json.h, i18n.h, ui_symbols.h, abi.h, LVGL's). Requests go through
# phone-ui's ui_fetch_request() (its libcurl). Its texts in phone-ui's language: po/'s
# catalogs in C (staging's gen-translations.py --app).

CC ?= cc
OUT ?= out
BUILD ?= $(OUT)
STAGING ?= /
# The cross strip next to CC (the build environment exports the host's in STRIP)
STRIP = $(patsubst %gcc,%strip,$(CC))

CFLAGS = -O2 -D_FORTIFY_SOURCE=3 -Wall -Wextra -Wno-missing-field-initializers -Wshadow -Wvla -Wimplicit-fallthrough -Wstrict-prototypes -Wmissing-prototypes -fPIC -DLV_CONF_INCLUDE_SIMPLE -DUI_APP_CATALOG \
	-I$(STAGING)/usr/include/phone-ui -I$(STAGING)/usr/include/lvgl

all:
	mkdir -p $(OUT) $(BUILD)
	python3 $(STAGING)/usr/share/phone-ui/gen-translations.py --app po $(BUILD)/i18n.c
	$(CC) $(CFLAGS) -shared -o $(OUT)/github.so *.c $(BUILD)/i18n.c \
		-L$(STAGING)/usr/lib -lpng16 -lm
	$(STRIP) $(OUT)/github.so
	cp app.json $(OUT)/
