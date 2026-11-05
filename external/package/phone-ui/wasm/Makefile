# phone-ui in the browser (`just wasm`): WebAssembly with Emscripten, in the flake's `wasm`
# shell (LVGL_SRC, LITEHTML_SRC, LIBJPEG_SRC: their sources; LIB_HEADERS: ALSA's, mpg123's
# and libcurl's headers; EMOJI: the emoji images). The display is LVGL's OpenGL ES draw
# unit on WebGL 1 (GLES 2, as on the phone); main.c replaces ../src/main.c (the page's canvas
# and pointer), stubs.c the hardware phone-ui finds missing in a browser (no sound card, no
# external apps, no camera), include/ the Linux headers Emscripten does not have. curl.c is
# libcurl over the browser's fetch(), emoji.c fetches the emoji images (build/emoji/) as they
# are drawn. libpng and zlib are Emscripten's ports, libjpeg-turbo is built here (phone-ui
# decodes to BGRA, which only it does).
#
# build/phone-ui.{js,wasm}, index.html and emoji/: `make serve` shows them on localhost:8080.

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
	-I$(BUILD)/jpeg/include -I$(BUILD)/litehtml/include -isystem $(LIB_HEADERS)/include \
	-Wno-deprecated-declarations
# C: glibc's extensions (strcasestr), as C++ has them already
UI_CFLAGS := $(UI_FLAGS) -D_GNU_SOURCE=
LITEHTML_FLAGS := $(OPT) -w -I$(LITEHTML_SRC)/include -I$(LITEHTML_SRC)/include/litehtml \
	-I$(LITEHTML_SRC)/src/gumbo/include -I$(LITEHTML_SRC)/src/gumbo/include/gumbo

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
$(BUILD)/litehtml/%.cpp.o: $(LITEHTML_SRC)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(LITEHTML_FLAGS) $(EXCEPTIONS) -std=c++17 -c $< -o $@
$(BUILD)/litehtml/%.c.o: $(LITEHTML_SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LITEHTML_FLAGS) -c $< -o $@
# Its headers as installed: <litehtml/litehtml.h>
$(BUILD)/litehtml/include/litehtml/litehtml.h:
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

# phone-ui but its hardware: main.c is this directory's, stubs.c stands in for the rest
NOT_HERE := main.c camera.c screenshot.c external.c
UI_SRCS := $(filter-out $(NOT_HERE),$(patsubst $(SRC)/%,%,$(wildcard $(SRC)/*.c $(SRC)/*/*.c)))
UI_CXX_SRCS := $(patsubst $(SRC)/%,%,$(wildcard $(SRC)/*/*.cpp))
UI_OBJS := $(UI_SRCS:%.c=$(BUILD)/ui/%.o) $(UI_CXX_SRCS:%.cpp=$(BUILD)/ui/%.o) \
	$(BUILD)/main.o $(BUILD)/stubs.o $(BUILD)/curl.o $(BUILD)/emoji.o $(BUILD)/emoji_names.o
HEADERS := $(BUILD)/lvgl/lv_conf.h $(BUILD)/lvgl/include/lvgl.h $(BUILD)/litehtml/include/litehtml/litehtml.h $(JPEG)

$(BUILD)/ui/%.o: $(SRC)/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(UI_CFLAGS) -c $< -o $@
$(BUILD)/ui/%.o: $(SRC)/%.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(UI_FLAGS) $(EXCEPTIONS) -std=c++17 -c $< -o $@
$(BUILD)/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
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

# WebGL 1 with its extensions, GLES 2 as on the Mali-400 (WebGL 2 has vertex arrays without
# OES_vertex_array_object, which LVGL takes them from); glad's loader through
# emscripten_webgl_get_proc_address; memory that grows (images, pages); emoji.c's look at the
# emoji files first
LDFLAGS := $(OPT) $(EXCEPTIONS) $(PORTS) -sMIN_WEBGL_VERSION=1 -sMAX_WEBGL_VERSION=1 -sGL_ENABLE_GET_PROC_ADDRESS=1 \
	-sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=128MB -sSTACK_SIZE=1MB \
	-sENVIRONMENT=web -sEXPORTED_RUNTIME_METHODS=FS -Wl,--gc-sections \
	-Wl,--wrap=png_image_begin_read_from_file
$(BUILD)/phone-ui.js: $(UI_OBJS) $(LVGL_OBJS) $(LITEHTML_OBJS) $(JPEG)
	$(CXX) $(LDFLAGS) -o $@ $^

# The page asks for phone-ui.js and .wasm by their hash (the browser's cache)
$(BUILD)/index.html: index.html $(BUILD)/phone-ui.js
	sed "s/@VERSION@/$$(cat $(BUILD)/phone-ui.js $(BUILD)/phone-ui.wasm | sha256sum | cut -c1-12)/g" $< > $@

all: $(BUILD)/phone-ui.js $(BUILD)/index.html $(BUILD)/emoji
.DEFAULT_GOAL := all

serve: all
	cd $(BUILD) && python3 -m http.server 8080

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d $(BUILD)/ui/*.d $(BUILD)/ui/*/*.d)

.PHONY: all serve clean $(BUILD)/emoji FORCE
