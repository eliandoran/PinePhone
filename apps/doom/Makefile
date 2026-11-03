# `just app doom`: doomgeneric (pinned) with the PinePhone platform layer
# (pinephone.c), Freedoom's first phase as the game. CC is the firmware's toolchain,
# OUT the folder installed on the phone, BUILD a scratch folder, DL the downloads.

CC ?= cc
OUT ?= out
BUILD ?= build
DL ?= ../../dl/apps

DG_COMMIT = e257a09527062bfb986b6e5e0780839b6cbd5b71
DG_SHA256 = 26f8f60daa6c6e4d3c01e5eac9fb700d592dede803d13ac848154ff356b2a2e7
DG_TAR = $(DL)/doomgeneric-$(DG_COMMIT).tar.gz
FREEDOOM_VERSION = 0.13.0
FREEDOOM_SHA256 = 3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59
FREEDOOM_ZIP = $(DL)/freedoom-$(FREEDOOM_VERSION).zip

DG = $(BUILD)/doomgeneric-$(DG_COMMIT)/doomgeneric
# doomgeneric's sources without the other platforms, SDL sound and tools
DG_EXCLUDE = (doomgeneric_|i_allegro|i_sdl|gusconf|icon|mus2mid)[^/]*$$
CFLAGS = -O2 -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200

all: $(DG_TAR) $(FREEDOOM_ZIP)
	rm -rf $(BUILD) && mkdir -p $(BUILD)/obj $(OUT)
	tar -xzf $(DG_TAR) -C $(BUILD)
	ls $(DG)/*.c | grep -Ev '/$(DG_EXCLUDE)' | \
		xargs -P $$(nproc) -I{} sh -c '$(CC) $(CFLAGS) -w -c {} -o $(BUILD)/obj/$$(basename {} .c).o'
	$(CC) $(CFLAGS) -Wall -I$(DG) -c pinephone.c -o $(BUILD)/obj/pinephone.o
	$(CC) -static -Wl,--gc-sections -o $(OUT)/doom $(BUILD)/obj/*.o -lm
	unzip -p $(FREEDOOM_ZIP) freedoom-$(FREEDOOM_VERSION)/freedoom1.wad > $(OUT)/freedoom1.wad
	cp app.json $(OUT)/

$(DG_TAR):
	mkdir -p $(DL)
	wget -q -O $@.tmp https://github.com/ozkl/doomgeneric/archive/$(DG_COMMIT).tar.gz
	echo "$(DG_SHA256)  $@.tmp" | sha256sum -c --quiet && mv $@.tmp $@

$(FREEDOOM_ZIP):
	mkdir -p $(DL)
	wget -q -O $@.tmp https://github.com/freedoom/freedoom/releases/download/v$(FREEDOOM_VERSION)/freedoom-$(FREEDOOM_VERSION).zip
	echo "$(FREEDOOM_SHA256)  $@.tmp" | sha256sum -c --quiet && mv $@.tmp $@
