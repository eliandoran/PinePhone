# `just app github`: a plugin app, github.so, that phone-ui loads and runs in its
# own process with its UI (external.c), built against the headers phone-ui installed
# in STAGING (ui.h, json.h, abi.h, LVGL's). Requests go through phone-ui's
# ui_fetch_request() (its libcurl).

CC ?= cc
OUT ?= out
STAGING ?= /
# The cross strip next to CC (the build environment exports the host's in STRIP)
STRIP = $(patsubst %gcc,%strip,$(CC))

CFLAGS = -O2 -Wall -fPIC -DLV_CONF_INCLUDE_SIMPLE \
	-I$(STAGING)/usr/include/phone-ui -I$(STAGING)/usr/include/lvgl

all:
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -o $(OUT)/github.so *.c
	$(STRIP) $(OUT)/github.so
	cp app.json $(OUT)/
