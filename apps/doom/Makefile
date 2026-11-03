# `just app doom`: doomgeneric (pinned) with the PinePhone platform layer
# (pinephone.c), shareware Doom 1.9 (episode 1) as the game. CC is the firmware's
# toolchain, OUT the folder installed on the phone, BUILD a scratch folder, DL the
# downloads.

CC ?= cc
OUT ?= out
BUILD ?= build
DL ?= ../../dl/apps

DG_COMMIT = e257a09527062bfb986b6e5e0780839b6cbd5b71
DG_SHA256 = 26f8f60daa6c6e4d3c01e5eac9fb700d592dede803d13ac848154ff356b2a2e7
DG_TAR = $(DL)/doomgeneric-$(DG_COMMIT).tar.gz
# id's shareware release, free to share unmodified but with no stable download:
# copied into DL by hand (md5 f0cefca49926d00903cf57551d901abe)
WAD_SHA256 = 1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771
WAD = $(DL)/doom1.wad

DG = $(BUILD)/doomgeneric-$(DG_COMMIT)/doomgeneric
# doomgeneric's sources without the other platforms, SDL sound and tools
DG_EXCLUDE = (doomgeneric_|i_allegro|i_sdl|gusconf|icon|mus2mid)[^/]*$$
CFLAGS = -O2 -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200

all: $(DG_TAR) $(WAD)
	echo "$(WAD_SHA256)  $(WAD)" | sha256sum -c --quiet
	rm -rf $(BUILD) && mkdir -p $(BUILD)/obj $(OUT)
	tar -xzf $(DG_TAR) -C $(BUILD)
	ls $(DG)/*.c | grep -Ev '/$(DG_EXCLUDE)' | \
		xargs -P $$(nproc) -I{} sh -c '$(CC) $(CFLAGS) -w -c {} -o $(BUILD)/obj/$$(basename {} .c).o'
	$(CC) $(CFLAGS) -Wall -I$(DG) -c pinephone.c -o $(BUILD)/obj/pinephone.o
	$(CC) -static -Wl,--gc-sections -o $(OUT)/doom $(BUILD)/obj/*.o -lm
	cp $(WAD) app.json $(OUT)/

$(DG_TAR):
	mkdir -p $(DL)
	wget -q -O $@.tmp https://github.com/ozkl/doomgeneric/archive/$(DG_COMMIT).tar.gz
	echo "$(DG_SHA256)  $@.tmp" | sha256sum -c --quiet && mv $@.tmp $@

$(WAD):
	@echo "Copy shareware Doom 1.9's DOOM1.WAD to $(WAD)" >&2; exit 1
