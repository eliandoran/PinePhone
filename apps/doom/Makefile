# `just app doom`: doomgeneric (pinned) with the PinePhone platform layer
# (pinephone.c, sound.c), shareware Doom 1.9 (episode 1) as the game. Music is
# Chocolate Doom's OPL (its emulator and player, which doomgeneric lacks) on
# sdl/, a stand-in for the bits of SDL they use. CC is the firmware's toolchain,
# OUT the folder installed on the phone, BUILD a scratch folder, DL the downloads.

CC ?= cc
OUT ?= out
BUILD ?= build
DL ?= ../../dl/apps

DG_COMMIT = e257a09527062bfb986b6e5e0780839b6cbd5b71
DG_SHA256 = 26f8f60daa6c6e4d3c01e5eac9fb700d592dede803d13ac848154ff356b2a2e7
DG_TAR = $(DL)/doomgeneric-$(DG_COMMIT).tar.gz
CHOCO_VERSION = 2.3.0
CHOCO_SHA256 = aac52a0b26cd43d6723874feca4bb7c5ea9fd42a04e1be1f07b2c3557bd5a744
CHOCO_TAR = $(DL)/chocolate-doom-$(CHOCO_VERSION).tar.gz
# id's shareware release, free to share unmodified but with no stable download:
# copied into DL by hand (md5 f0cefca49926d00903cf57551d901abe)
WAD_SHA256 = 1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771
WAD = $(DL)/doom1.wad

DG = $(BUILD)/doomgeneric-$(DG_COMMIT)/doomgeneric
CHOCO = $(BUILD)/chocolate-doom-chocolate-doom-$(CHOCO_VERSION)
# Chocolate Doom's files added to doomgeneric's
CHOCO_FILES = opl/opl.c opl/opl.h opl/opl_internal.h opl/opl_queue.c opl/opl_queue.h \
	opl/opl_sdl.c opl/opl3.c opl/opl3.h src/i_oplmusic.c src/midifile.c src/midifile.h
# doomgeneric's sources without the other platforms, SDL sound and tools
DG_EXCLUDE = (doomgeneric_|i_allegro|i_sdl|gusconf|icon)[^/]*$$
CFLAGS = -O2 -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE -DFEATURE_SOUND \
	-DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -I$(CURDIR)/sdl -I$(DG)

all: $(DG_TAR) $(CHOCO_TAR) $(WAD)
	echo "$(WAD_SHA256)  $(WAD)" | sha256sum -c --quiet
	rm -rf $(BUILD) && mkdir -p $(BUILD)/obj $(OUT)
	tar -xzf $(DG_TAR) -C $(BUILD)
	tar -xzf $(CHOCO_TAR) -C $(BUILD)
	cd $(CHOCO) && cp $(CHOCO_FILES) $(DG)/
	# Its music module is the one doomgeneric's i_sound.c uses
	sed -i 's/^music_module_t music_opl_module =/music_module_t DG_music_module =/' $(DG)/i_oplmusic.c
	sed -i '1i #include "choco.h"' $(DG)/i_oplmusic.c $(DG)/midifile.c $(DG)/opl.c
	ls $(DG)/*.c | grep -Ev '/$(DG_EXCLUDE)' | \
		xargs -P $$(nproc) -I{} sh -c '$(CC) $(CFLAGS) -w -c {} -o $(BUILD)/obj/$$(basename {} .c).o'
	$(CC) $(CFLAGS) -Wall -c pinephone.c -o $(BUILD)/obj/pinephone.o
	$(CC) $(CFLAGS) -Wall -c sound.c -o $(BUILD)/obj/sound.o
	$(CC) -static -Wl,--gc-sections -o $(OUT)/doom $(BUILD)/obj/*.o -lpthread -lm
	cp $(WAD) app.json $(OUT)/

$(DG_TAR):
	mkdir -p $(DL)
	wget -q -O $@.tmp https://github.com/ozkl/doomgeneric/archive/$(DG_COMMIT).tar.gz
	echo "$(DG_SHA256)  $@.tmp" | sha256sum -c --quiet && mv $@.tmp $@

$(CHOCO_TAR):
	mkdir -p $(DL)
	wget -q -O $@.tmp https://github.com/chocolate-doom/chocolate-doom/archive/refs/tags/chocolate-doom-$(CHOCO_VERSION).tar.gz
	echo "$(CHOCO_SHA256)  $@.tmp" | sha256sum -c --quiet && mv $@.tmp $@

$(WAD):
	@echo "Copy shareware Doom 1.9's DOOM1.WAD to $(WAD)" >&2; exit 1
