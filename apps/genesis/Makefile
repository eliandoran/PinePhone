# `just app genesis`: a plugin app, genesis.so, that phone-ui loads and runs in its
# own process with its UI (external.c), built against the headers phone-ui installed
# in STAGING (ui.h, abi.h, LVGL's). The emulator is Genesis Plus GX's libretro core
# (pinned), built once into BUILD with its own Makefile, in lib/; genesis.so loads it
# when the app opens. Its sound comes at 48 kHz, the rate the codec plays.
# Games (ROMs) are not part of it: `just roms` copies them to the phone.

CC ?= cc
OUT ?= out
BUILD ?= build
DL ?= ../../dl/apps
STAGING ?= /
# The cross strip next to CC (the build environment exports the host's in STRIP)
STRIP = $(patsubst %gcc,%strip,$(CC))

GPGX_COMMIT = f27655b592ef43f317272d79883bcaa10d3ac8ad
GPGX_SHA256 = 4720dce8f219994951fe5d021e433f3b107ad7c3c64b6295a6a12928f31a73ea
GPGX_TAR = $(DL)/Genesis-Plus-GX-$(GPGX_COMMIT).tar.gz
GPGX = $(BUILD)/Genesis-Plus-GX-$(GPGX_COMMIT)
CORE = $(GPGX)/genesis_plus_gx_libretro.so

CFLAGS = -O2 -Wall -fPIC -DLV_CONF_INCLUDE_SIMPLE -DUI_APP_CATALOG \
	-I$(STAGING)/usr/include/phone-ui -I$(STAGING)/usr/include/lvgl \
	-I$(GPGX)/libretro/libretro-common/include

all: $(CORE)
	mkdir -p $(OUT)/lib $(BUILD)
	python3 $(STAGING)/usr/share/phone-ui/gen-translations.py --app po $(BUILD)/i18n.c
	$(CC) $(CFLAGS) -shared -o $(OUT)/genesis.so genesis.c $(BUILD)/i18n.c -lGLESv2 -lasound -lpthread
	$(STRIP) $(OUT)/genesis.so
	cp $(CORE) $(OUT)/lib/
	$(STRIP) $(OUT)/lib/genesis_plus_gx_libretro.so
	cp app.json $(OUT)/

# The core's own Makefile, without our variables (MAKEFLAGS) and CD-ROM drive access
$(CORE): $(GPGX_TAR)
	echo "$(GPGX_SHA256)  $(GPGX_TAR)" | sha256sum -c --quiet
	rm -rf $(GPGX) && mkdir -p $(BUILD)
	tar -xzf $(GPGX_TAR) -C $(BUILD)
	sed -i 's/^#define SOUND_FREQUENCY 44100/#define SOUND_FREQUENCY 48000/' $(GPGX)/libretro/libretro.c
	grep -q '^#define SOUND_FREQUENCY 48000' $(GPGX)/libretro/libretro.c
	env -u MAKEFLAGS -u MAKELEVEL $(MAKE) -C $(GPGX) -f Makefile.libretro -j$$(nproc) \
		platform=unix CC=$(CC) HAVE_CDROM=0

$(GPGX_TAR):
	mkdir -p $(DL)
	wget -q -O $@.tmp https://github.com/libretro/Genesis-Plus-GX/archive/$(GPGX_COMMIT).tar.gz
	echo "$(GPGX_SHA256)  $@.tmp" | sha256sum -c --quiet && mv $@.tmp $@
