# `just app email`: a plugin app, email.so, that phone-ui loads and runs in its own
# process with its UI (external.c), built against the headers phone-ui installed in
# STAGING (ui.h, json.h, abi.h, LVGL's). Mail goes through libetpan (IMAP, MIME),
# built for staging only: it goes in lib/, found through email.so's runpath; OpenSSL
# is the image's.

CC ?= cc
OUT ?= out
BUILD ?= $(OUT)
STAGING ?= /
# The cross strip next to CC (the build environment exports the host's in STRIP)
STRIP = $(patsubst %gcc,%strip,$(CC))

CFLAGS = -O2 -std=gnu17 -D_FORTIFY_SOURCE=3 -Wall -Wextra -Wno-missing-field-initializers -Wshadow -Wvla -Wimplicit-fallthrough -Wstrict-prototypes -Wmissing-prototypes -fPIC -DLV_CONF_INCLUDE_SIMPLE -DUI_APP_CATALOG \
	-I$(STAGING)/usr/include/phone-ui -I$(STAGING)/usr/include/lvgl -I$(STAGING)/usr/include

all:
	mkdir -p $(OUT)/lib $(BUILD)
	python3 $(STAGING)/usr/share/phone-ui/gen-translations.py --app po $(BUILD)/i18n.c
	$(CC) $(CFLAGS) -shared -Wl,-rpath,'$$ORIGIN/lib' -o $(OUT)/email.so *.c $(BUILD)/i18n.c \
		-L$(STAGING)/usr/lib -letpan -lssl -lcrypto -lpthread
	$(STRIP) $(OUT)/email.so
	cp -a $(STAGING)/usr/lib/libetpan.so* $(OUT)/lib/
	$(STRIP) $(OUT)/lib/libetpan.so.*.*
	cp app.json $(OUT)/
