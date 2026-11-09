# phone-ui in the browser (`just wasm`): WebAssembly with Emscripten, in the flake's `wasm`
# shell (LVGL_SRC, LITEHTML_SRC, LIBJPEG_SRC, LIBWEBP_SRC, LIBMPG123_SRC: their sources;
# LIB_HEADERS: ALSA's and libcurl's headers; EMOJI: the emoji images). The display is LVGL's
# OpenGL ES draw unit on WebGL 1 (GLES 2, as on the phone); main.c replaces ../src/main.c (the
# page's canvas and pointer), stubs.c the hardware phone-ui finds missing in a browser (no sound
# card, no external apps, no camera), include/ the Linux headers Emscripten does not have.
# curl.c is libcurl over the browser's fetch(), emoji.c fetches the emoji images (build/emoji/)
# as they are drawn; modem.c (instead of ../src/modem/modem_at.c), wifi.c and player.c play a modem,
# wpa_supplicant and `mpg123 --remote` (sockets.c); demo.c fetches the repository's demo
# files; probe.c answers smoke.mjs (`make smoke`: the page tested in Chromium). libpng and
# zlib are Emscripten's ports, libjpeg-turbo (phone-ui decodes to BGRA, which only it does),
# libwebp (Web Lite's and Books' pictures) and libmpg123 (Music's tags and lengths) are built here.
#
# build/phone-ui.{js,wasm}, index.html, emoji/ and demo/: `make serve` shows them on localhost:8080.

ifeq ($(LVGL_SRC)$(filter clean,$(MAKECMDGOALS)),)
$(error LVGL_SRC is not set: build with `just wasm`)
endif
SRC := ../src
BUILD := build

CC := emcc
CXX := em++
PORTS := -sUSE_LIBPNG=1 -sUSE_ZLIB=1
OPT := -O2
# C++ exceptions, which litehtml throws and catches (Emscripten aborts on a throw without
# them): through JavaScript, in every browser (-fwasm-exceptions fails in Emscripten 6.0.9)
EXCEPTIONS := -fexceptions
FLAGS := $(OPT) -Wall -ffunction-sections -fdata-sections -DLV_CONF_INCLUDE_SIMPLE \
	-I$(BUILD)/lvgl $(PORTS)
LVGL_FLAGS := $(FLAGS) -I$(LVGL_SRC) -w -Wno-error=int-conversion
UI_FLAGS := $(FLAGS) -include include/wasm.h -I$(BUILD)/lvgl/include -I$(SRC) -I$(BUILD) -Iinclude \
	-I$(BUILD)/jpeg/include -I$(BUILD)/webp/include -I$(BUILD)/mpg123/include -I$(BUILD)/litehtml/include \
	-isystem $(LIB_HEADERS)/include \
	-Wno-deprecated-declarations
# C: glibc's extensions (strcasestr), as C++ has them already
UI_CFLAGS := $(UI_FLAGS) -std=gnu17 -D_GNU_SOURCE=
LITEHTML_FLAGS := $(OPT) -w -I$(LITEHTML_SRC)/include -I$(LITEHTML_SRC)/include/litehtml \
	-I$(LITEHTML_SRC)/src/gumbo/src

# LVGL as in the image (lv_conf.h), without what a browser does not have: DRM, evdev,
# threads, NEON. LV_USE_EGL, which DRM's EGL backend sets on the phone: GLES 2 through
# glad (else desktop GL's)
LV_CONF := ../../lvgl/lv_conf.h
$(BUILD)/lvgl/lv_conf.h: $(LV_CONF)
	@mkdir -p $(dir $@)
	sed -e 's/^#define LV_USE_LINUX_DRM 1$$/#define LV_USE_LINUX_DRM 0/' \
		-e 's/^#define LV_USE_EVDEV 1$$/#define LV_USE_EVDEV 0/' \
		-e 's/^#define LV_USE_OS LV_OS_PTHREAD$$/#define LV_USE_OS LV_OS_NONE/' \
		-e 's/^#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NEON$$/#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE/' \
		-e 's/^#define PP_GPU 1$$/&\n#define LV_USE_EGL 1/' \
		$< > $@.tmp
	for d in 'LV_USE_LINUX_DRM 0' 'LV_USE_EVDEV 0' 'LV_USE_OS LV_OS_NONE' 'LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE' \
		'LV_USE_EGL 1'; do \
		grep -q "^#define $$d$$" $@.tmp || { echo "lv_conf.h: no $$d"; exit 1; }; done
	mv $@.tmp $@

# LVGL's headers where phone-ui includes them from (lvgl.h, src/...), as installed
$(BUILD)/lvgl/include/lvgl.h:
	@mkdir -p $(BUILD)/lvgl/include
	cd $(LVGL_SRC) && find lvgl.h lvgl_private.h lv_version.h src include -name '*.h' \
		-exec install -D -m 0644 {} $(abspath $(BUILD))/lvgl/include/{} \;
	chmod -R u+w $(BUILD)/lvgl/include

LVGL_SRCS := $(shell cd $(LVGL_SRC) 2>/dev/null && find src -name '*.c')
LVGL_OBJS := $(LVGL_SRCS:%.c=$(BUILD)/lvgl/%.o)
$(BUILD)/lvgl/%.o: $(LVGL_SRC)/%.c $(BUILD)/lvgl/lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(LVGL_FLAGS) -c $< -o $@

LITEHTML_SRCS := $(shell cd $(LITEHTML_SRC) 2>/dev/null && find src -name '*.cpp' -o -name '*.c')
LITEHTML_OBJS := $(patsubst %,$(BUILD)/litehtml/%.o,$(LITEHTML_SRCS))
# The sources' store path, rewritten when it changes (a patch added): their files all date
# from 1970, so the objects would look newer than the new sources
LITEHTML_PATH := $(BUILD)/litehtml/src-path
$(shell mkdir -p $(BUILD)/litehtml && [ "$$(cat $(LITEHTML_PATH) 2>/dev/null)" = "$(LITEHTML_SRC)" ] \
	|| echo "$(LITEHTML_SRC)" > $(LITEHTML_PATH))
$(BUILD)/litehtml/%.cpp.o: $(LITEHTML_SRC)/%.cpp $(LITEHTML_PATH)
	@mkdir -p $(dir $@)
	$(CXX) $(LITEHTML_FLAGS) $(EXCEPTIONS) -std=c++17 -c $< -o $@
$(BUILD)/litehtml/%.c.o: $(LITEHTML_SRC)/%.c $(LITEHTML_PATH)
	@mkdir -p $(dir $@)
	$(CC) $(LITEHTML_FLAGS) -c $< -o $@
# Its headers as installed: <litehtml/litehtml.h>
$(BUILD)/litehtml/include/litehtml/litehtml.h: $(LITEHTML_PATH)
	@mkdir -p $(dir $@)
	cp -r $(LITEHTML_SRC)/include/litehtml/. $(LITEHTML_SRC)/include/litehtml.h $(dir $@)
	chmod -R u+w $(BUILD)/litehtml/include

# libjpeg-turbo, static, without SIMD (none in WebAssembly)
JPEG := $(BUILD)/jpeg/lib/libjpeg.a
$(JPEG):
	emcmake cmake -S $(LIBJPEG_SRC) -B $(BUILD)/jpeg/build -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=$(abspath $(BUILD))/jpeg -DCMAKE_INSTALL_LIBDIR=lib \
		-DENABLE_SHARED=OFF -DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
	cmake --build $(BUILD)/jpeg/build -j$$(nproc) --target install

# libwebp, static, its decoder: without SIMD, its programs or its other libraries
WEBP := $(BUILD)/webp/lib/libwebp.a
$(WEBP):
	emcmake cmake -S $(LIBWEBP_SRC) -B $(BUILD)/webp/build -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=$(abspath $(BUILD))/webp -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF \
		-DWEBP_ENABLE_SIMD=OFF -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
		-DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF \
		-DWEBP_BUILD_LIBWEBPMUX=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF
	cmake --build $(BUILD)/webp/build -j$$(nproc) --target install

# libmpg123, static, its generic decoder (no SIMD), without its programs and output library
MPG123 := $(BUILD)/mpg123/lib/libmpg123.a
$(MPG123):
	emcmake cmake -S $(LIBMPG123_SRC)/ports/cmake -B $(BUILD)/mpg123/build -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=$(abspath $(BUILD))/mpg123 -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF \
		-DBUILD_LIBOUT123=OFF -DBUILD_PROGRAMS=OFF -DNETWORK=OFF
	cmake --build $(BUILD)/mpg123/build -j$$(nproc) --target install

# phone-ui but its hardware: main.c is this directory's, stubs.c stands in for the rest
NOT_HERE := main.c system/cpu.c apps/camera.c system/screenshot.c system/external.c modem/modem_at.c \
	net/pairing.c net/bluetooth.c system/touch.c system/gamepad.c system/mouse.c
UI_SRCS := $(filter-out $(NOT_HERE),$(shell cd $(SRC) && find . -name '*.c' | sed 's|^\./||'))
UI_CXX_SRCS := $(shell cd $(SRC) && find . -name '*.cpp' | sed 's|^\./||')
UI_OBJS := $(UI_SRCS:%.c=$(BUILD)/ui/%.o) $(UI_CXX_SRCS:%.cpp=$(BUILD)/ui/%.o) \
	$(BUILD)/main.o $(BUILD)/stubs.o $(BUILD)/curl.o $(BUILD)/emoji.o $(BUILD)/emoji_names.o \
	$(BUILD)/modem.o $(BUILD)/wifi.o $(BUILD)/sockets.o $(BUILD)/player.o $(BUILD)/demo.o \
	$(BUILD)/demo_files.o $(BUILD)/probe.o $(BUILD)/i18n_languages.o $(BUILD)/words.o
HEADERS := $(BUILD)/lvgl/lv_conf.h $(BUILD)/lvgl/include/lvgl.h $(BUILD)/litehtml/include/litehtml/litehtml.h $(JPEG) $(WEBP) $(MPG123)

$(BUILD)/ui/%.o: $(SRC)/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(UI_CFLAGS) -c $< -o $@
$(BUILD)/ui/%.o: $(SRC)/%.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(UI_FLAGS) $(EXCEPTIONS) -std=c++17 -c $< -o $@
$(BUILD)/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(UI_CFLAGS) -c $< -o $@

# The translations: ../po's catalogs in C, as for the image (gen-translations.py)
$(BUILD)/i18n_languages.c: ../gen-translations.py $(wildcard ../po/*.po)
	@python3 ../gen-translations.py ../po $@
$(BUILD)/i18n_languages.o: $(BUILD)/i18n_languages.c $(HEADERS)
	$(CC) $(UI_CFLAGS) -c $< -o $@

# The keyboard's dictionaries: ../words' lists in C, as for the image (gen-words.py)
$(BUILD)/words.c: ../gen-words.py $(wildcard ../words/*.txt)
	@python3 ../gen-words.py ../words $@
$(BUILD)/words.o: $(BUILD)/words.c $(HEADERS)
	$(CC) $(UI_CFLAGS) -c $< -o $@

# The names of the emoji images, for emoji.c, and their version: their store path's hash (the
# browser's cache); rewritten when it changes. The images beside the page
$(BUILD)/emoji_names.c: FORCE
	@mkdir -p $(dir $@)
	{ printf 'const char emoji_version[] = "%s";\n' $$(basename $(EMOJI) | cut -c1-12); \
	  printf 'const char emoji_names[] =\n'; ls $(EMOJI) | sed -n 's/\.png$$/\\n/p' | sed 's/.*/\t"&"/'; echo ';'; } > $@.tmp
	cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@
$(BUILD)/emoji_names.o: $(BUILD)/emoji_names.c
	$(CC) $(OPT) -c $< -o $@
$(BUILD)/emoji:
	ln -sfn $(EMOJI) $@

# The repository's demo files for demo.c, smallest first, and their version: their contents'
# hash. Beside the page
DEMO := ../../../../demo
DEMO_DIRS := books photos music
$(BUILD)/demo_files.c: FORCE
	@mkdir -p $(dir $@)
	cd $(DEMO) && { printf 'const char demo_version[] = "%s";\n' \
			"$$(find $(DEMO_DIRS) -type f | LC_ALL=C sort | xargs -d '\n' cat | sha256sum | cut -c1-12)"; \
		printf 'const char demo_files[] =\n'; \
		find $(DEMO_DIRS) -type f -printf '%s %p\n' | sort -n | cut -d' ' -f2- | sed 's/.*/\t"&\\n"/'; echo ';'; } \
		> $(abspath $@).tmp
	cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@
$(BUILD)/demo_files.o: $(BUILD)/demo_files.c
	$(CC) $(OPT) -c $< -o $@
$(BUILD)/demo:
	ln -sfn $(abspath $(DEMO)) $@

# WebGL 1 with its extensions, GLES 2 as on the Mali-400 (WebGL 2 has vertex arrays without
# OES_vertex_array_object, which LVGL takes them from); glad's loader through
# emscripten_webgl_get_proc_address; memory that grows (images, pages); emoji.c's look at the
# emoji files first; sockets.c's wpa_supplicant and mpg123 behind the socket calls (and fork),
# wifi.c's interfaces
SOCKET_CALLS := socket socketpair bind connect send recv read poll close ioctl fork waitpid \
	getifaddrs freeifaddrs
LDFLAGS := $(OPT) $(EXCEPTIONS) $(PORTS) -sMIN_WEBGL_VERSION=1 -sMAX_WEBGL_VERSION=1 -sGL_ENABLE_GET_PROC_ADDRESS=1 \
	-sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=128MB -sSTACK_SIZE=1MB \
	-sENVIRONMENT=web -sEXPORTED_RUNTIME_METHODS=FS,ccall -Wl,--gc-sections \
	-Wl,--wrap=png_image_begin_read_from_file $(SOCKET_CALLS:%=-Wl,--wrap=%)
$(BUILD)/phone-ui.js: $(UI_OBJS) $(LVGL_OBJS) $(LITEHTML_OBJS) $(JPEG) $(WEBP) $(MPG123)
	$(CXX) $(LDFLAGS) -o $@ $^

# The page asks for phone-ui.js and .wasm by their hash (the browser's cache)
$(BUILD)/index.html: index.html $(BUILD)/phone-ui.js
	sed "s/@VERSION@/$$(cat $(BUILD)/phone-ui.js $(BUILD)/phone-ui.wasm | sha256sum | cut -c1-12)/g" $< > $@

all: $(BUILD)/phone-ui.js $(BUILD)/index.html $(BUILD)/emoji $(BUILD)/demo
.DEFAULT_GOAL := all

serve: all
	cd $(BUILD) && python3 -m http.server 8080

# smoke.mjs: the page in headless Chromium, used as someone would
smoke: all
	node smoke.mjs $(BUILD)

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d $(BUILD)/ui/*.d $(BUILD)/ui/*/*.d $(BUILD)/ui/*/*/*.d)

.PHONY: all serve smoke clean $(BUILD)/emoji $(BUILD)/demo FORCE
