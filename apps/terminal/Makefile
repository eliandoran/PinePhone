# `just app terminal`: a plugin app, terminal.so, that phone-ui loads and runs in its
# own process with its UI (external.c), built against the headers phone-ui installed
# in STAGING (ui.h, abi.h, LVGL's). term_font.c comes from gen-font.sh.

CC ?= cc
OUT ?= out
STAGING ?= /
# The cross strip next to CC (the build environment exports the host's in STRIP)
STRIP = $(patsubst %gcc,%strip,$(CC))

CFLAGS = -O2 -Wall -fPIC -DLV_CONF_INCLUDE_SIMPLE \
	-I$(STAGING)/usr/include/phone-ui -I$(STAGING)/usr/include/lvgl

all:
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -o $(OUT)/terminal.so terminal.c term_font.c
	$(STRIP) $(OUT)/terminal.so
	cp app.json $(OUT)/
