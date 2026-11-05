# phone-ui's tests, on the host: `just test [filter]`, which runs make here in the
# flake's `tests` shell (LVGL: lvgl-host, EGL_VENDOR: Mesa's, EMOJI: the emoji images, the
# libraries). Built with ASan and UBSan. One program per test file, run in a sandbox
# (bubblewrap: the host read-only, /var/lib and /sys in build/run/test_<name>/, no devices,
# no network), all at once;
# build/test_<name> [filter] runs one, in `$(SANDBOX)`.
#
# Unit tests (TESTS) link the sources they list and stubs.c. UI tests (UI_TESTS) link
# all of phone-ui but main.c, ui_test.c that starts it (ui_test.h) and fake_curl.c, a
# libcurl answering from what the tests serve (no network in the sandbox), and
# fake_alsa.c, the PCM streams of a sound card on the tests' clock (wrapped), and
# fake_wpa.c, wpa_supplicant's control interface, and fake_modem.c, the modem's AT port,
# fake_camera.c, the cameras' V4L2 nodes, and fake_drm.c, the display's scanout (wrapped).
# test_web.c also links fake_webkit.c, WPE WebKit for the Browser plugin app (apps/web), which
# it builds for the host and loads as phone-ui does.
#
# `make coverage` (`just coverage`): the same tests built with --coverage in
# build/coverage (-Og, for exact lines) and run, then gcovr's report of ../src: per file
# here, line by line in build/coverage/html/index.html, and the sources no test reaches.

ifeq ($(LVGL)$(filter clean,$(MAKECMDGOALS)),)
$(error LVGL is not set: run the tests with `just test`)
endif
SRC := ../src
BUILD := build
.DEFAULT_GOAL := run

TESTS := json data calculator sms_pdu emoji i18n
UI_TESTS := home navigation notes phone messages contacts settings clock calendar files gallery snake lock shade volume weather maps browser books assistant recorder keyboard music compass wifi modem mobile_data ussd calls camera screenshot splash web terminal github

# What each unit test links besides itself and stubs.c (sources of ../src)
json_SRCS := json.c
data_SRCS := data.c json.c
calculator_SRCS := ui_font_48.c ui_font_32.c ui_font_26.c
sms_pdu_SRCS := sms_pdu.c
i18n_SRCS := i18n.c i18n_ro.c
emoji_LDLIBS := -lpng
# test_emoji.c copies the images from there into its own EMOJI_DIR
$(BUILD)/test_emoji.o: override CFLAGS += -DEMOJI_IMAGES='"$(EMOJI)"'
# test_docs.c's pictures, music and books: the repository's demo files
DEMO := ../../../../demo
$(BUILD)/test_docs.o: override CFLAGS += -DDEMO_DIR='"$(abspath $(DEMO))"'
# test_i18n.c reads the sources for the texts marked for translation
$(BUILD)/test_i18n.o: override CFLAGS += -DSRC_DIR='"$(abspath $(SRC))"'

SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=undefined
FLAGS := -O1 -g -Wall -fno-omit-frame-pointer $(SANITIZE) -MMD -MP \
	-DLV_CONF_INCLUDE_SIMPLE -I$(LVGL)/usr/include/lvgl -I$(SRC)
override CFLAGS += $(FLAGS)
override CXXFLAGS += $(FLAGS) -std=c++17
LDFLAGS := $(SANITIZE)
LDLIBS := $(LVGL)/usr/lib/liblvgl.a -lm
# As phone-ui.mk links it, but libcurl: fake_curl.c answers what the tests serve
UI_LDLIBS := $(LVGL)/usr/lib/liblvgl.a -lpng -ljpeg -ldrm -llitehtml -lgumbo -lmpg123 -lz \
	-lasound -lgbm -lGLESv2 -lEGL -lstdc++ -lpthread -lm

ifdef COVERAGE
# -Og, not -O0: nix's compiler wrapper sets _FORTIFY_SOURCE, which warns without optimization
override CFLAGS += --coverage -Og -DTEST_COVERAGE
override CXXFLAGS += --coverage -Og
LDFLAGS += --coverage
endif
# Generated sources (fonts, images), not in the report
GENERATED := ui_font_.*|icons|emoji_list|splash_image

# Unit tests: contacts and conversations in the build, the fonts' emoji fallbacks
# (emoji.c, with all of its images) LVGL's Montserrat
$(BUILD)/src/data.o $(BUILD)/test_data.o: override CFLAGS += -DCONTACTS='"$(abspath $(BUILD))/contacts"' \
	-DMESSAGES='"$(abspath $(BUILD))/messages"'
FONTS := 48 32 26
$(foreach s,$(FONTS),$(eval $(BUILD)/src/ui_font_$(s).o: override CFLAGS += -Dui_emoji_$(s)=lv_font_montserrat_$(s)))

# UI tests: phone-ui as in the image
UI_SRCS := $(filter-out main.c,$(patsubst $(SRC)/%,%,$(wildcard $(SRC)/*.c $(SRC)/*/*.c)))
UI_CXX_SRCS := $(patsubst $(SRC)/%,%,$(wildcard $(SRC)/*/*.cpp))
UI_OBJS := $(UI_SRCS:%.c=$(BUILD)/ui/%.o) $(UI_CXX_SRCS:%.cpp=$(BUILD)/ui/%.o) $(BUILD)/ui_test.o $(BUILD)/fake_curl.o $(BUILD)/fake_alsa.o \
	$(BUILD)/fake_wpa.o $(BUILD)/fake_modem.o $(BUILD)/fake_camera.o $(BUILD)/fake_drm.o
$(BUILD)/ui/%.o: override CFLAGS += $(shell pkg-config --cflags libdrm) -I$(BUILD)/ui
$(BUILD)/fake_drm.o: override CFLAGS += $(shell pkg-config --cflags libdrm)
# The emoji images of the image, from the tests shell (EMOJI)
$(BUILD)/ui/emoji.o: override CFLAGS += -DEMOJI_DIR='"$(EMOJI)"'
$(BUILD)/ui/external.o: $(BUILD)/ui/abi.h
# wpa_supplicant's control sockets: fake_wpa.c's, in the sandbox's /tmp
# and the routing table, test_wifi.c's (the sandbox has no network)
$(BUILD)/ui/settings/wifi.o: override CFLAGS += -DWPA_CTRL_DIR='"/tmp/wpa_supplicant"' \
	-DPROC_NET_ROUTE='"/tmp/route"'
# The board's modem.conf, ui_test.c's (its port fake_modem.c's pty)
$(BUILD)/ui/modem.o: override CFLAGS += -DMODEM_CONF='"/tmp/modem.conf"'
# The boot splash's once-per-boot mark, in the sandbox's /tmp (/run is the host's)
$(BUILD)/ui/splash.o: override CFLAGS += -DMARKER='"/tmp/phone-ui.splash"'

# The plugins' ABI, as phone-ui.mk makes it
$(BUILD)/ui/abi.h: $(SRC)/ui.h $(SRC)/json.h
	@mkdir -p $(dir $@)
	{ cat $^; find $(LVGL)/usr/include/lvgl -name '*.h' | LC_ALL=C sort | xargs cat; } \
		| sha256sum | cut -c1-16 | sed 's/.*/#define UI_ABI "&"/' > $@

# The sandbox: /var/lib and /sys are directories of the build that ui_test.c fills
# (a phone's sysfs), $(1)/var-lib and $(1)/sys; no network but loopback, the time zone
# UTC as on the phone (no zoneinfo there, the host's /etc/localtime here). Mesa's shader
# cache in the build: llvmpipe compiling LVGL's shaders again was half of a UI test.
VAR_LIB := $(abspath $(BUILD))/var-lib
SYS := $(abspath $(BUILD))/sys
sandbox = bwrap --ro-bind / / --dev /dev --proc /proc --tmpfs /tmp --unshare-net \
	--bind $(1)/var-lib /var/lib --bind $(1)/sys /sys --bind $(abspath $(BUILD)) $(abspath $(BUILD)) \
	--setenv __EGL_VENDOR_LIBRARY_FILENAMES "$(EGL_VENDOR)" \
	--setenv MESA_SHADER_CACHE_DIR $(abspath $(BUILD))/mesa-cache --setenv TZ UTC0 \
	--die-with-parent --
SANDBOX = $(call sandbox,$(abspath $(BUILD)))

# The translated parts of ../src first (../i18n-check.py): each text a user reads there
# marked for translation, or NON-NLS
TRANSLATED := settings
i18n-check:
	@python3 ../i18n-check.py --strict $(addprefix $(SRC)/,$(TRANSLATED))

# The programs JOBS at a time (a core each; more and the tests' real-time waits run out
# on CI's few cores), each with its /var/lib and /sys (build/run/test_<name>/), their
# output shown in order as each ends. One run at a time in a build (several agents, a
# run by hand): they would share those.
RUN := $(abspath $(BUILD))/run
JOBS ?= $(shell nproc)
run: $(TESTS:%=$(BUILD)/test_%) $(UI_TESTS:%=$(BUILD)/test_%) | i18n-check
	@flock $(BUILD)/.run sh -c ' \
		rm -rf $(RUN); started=0; \
		for t in "$$@"; do \
			while [ $$((started - $$(cat $(RUN)/*/status 2>/dev/null | wc -l))) -ge $(JOBS) ]; do \
				sleep 0.1; \
			done; \
			started=$$((started + 1)); \
			d=$(RUN)/$${t##*/}; mkdir -p $$d/var-lib $$d/sys; \
			touch $$d/var-lib/.phone-ui-tests $$d/sys/.phone-ui-tests; \
			{ $(call sandbox,$$d) ./$$t $(FILTER) > $$d/log 2>&1; echo $$? > $$d/status; } & \
		done; \
		failed=0; \
		for t in "$$@"; do \
			d=$(RUN)/$${t##*/}; while [ ! -s $$d/status ]; do sleep 0.1; done; \
			cat $$d/log; [ "$$(cat $$d/status)" = 0 ] || failed=1; \
		done; \
		exit $$failed' sh $^

# The report even when tests fail (then fails too); counts from earlier runs dropped;
# no limit on hit counts: loops over every pixel of the screen reach gcovr's 2^32 "suspicious" one
# Then the external apps' (../../../../apps, those with tests: their .so built in build/coverage/apps)
APPS := ../../../../apps
COV := $(BUILD)/coverage
coverage:
	@mkdir -p $(COV) && find $(COV) -name '*.gcda' -delete
	@$(MAKE) --no-print-directory run BUILD=$(COV) COVERAGE=1; status=$$?; \
	mkdir -p $(COV)/html && \
	gcovr --root $(SRC) --object-directory $(COV) --exclude '.*/($(GENERATED))\.c' \
		--gcov-suspicious-hits-threshold 0 \
		--txt --print-summary --html-details $(COV)/html/index.html \
		--json-summary $(COV)/summary.json $(COV) && \
	jq -r '.files[].filename' $(COV)/summary.json | sort > $(COV)/tested && \
	(cd $(SRC) && find . -name '*.c' | sed 's|^\./||' | grep -Ev '^($(GENERATED))\.c$$' | sort) \
		| comm -23 - $(COV)/tested | tr '\n' ' ' | sed 's/^./No tests: &/;s/ $$/\n/' && \
	echo "Line by line: $(abspath $(COV))/html/index.html" && \
	mkdir -p $(COV)/apps $(COV)/html/apps && \
	gcovr --root $(APPS) --object-directory $(COV)/apps --exclude '.*/term_font\.c' \
		--gcov-suspicious-hits-threshold 0 \
		--txt --print-summary --html-details $(COV)/html/apps/index.html $(COV)/apps && \
	ls $(APPS) | grep -vxF "$$(ls $(COV)/apps)" | tr '\n' ' ' | sed 's/^./External apps without tests: &/;s/ $$/\n/' && \
	echo "Line by line: $(abspath $(COV))/html/apps/index.html" && exit $$status

define test_rule
$(BUILD)/test_$(1): $(BUILD)/test_$(1).o $(BUILD)/stubs.o $($(1)_SRCS:%.c=$(BUILD)/src/%.o)
	$$(CC) $$(LDFLAGS) -o $$@ $$^ $$(LDLIBS) $$($(1)_LDLIBS)
endef
$(foreach t,$(TESTS),$(eval $(call test_rule,$(t))))

# UI tests: phone-ui's clocks are ui_test.c's (monotonic: the tests' time; the wall clock
# when a test sets it), its PCM streams
# fake_alsa.c's, wlan0's flags (ioctl) fake_wpa.c's, the cameras' nodes (ioctl) fake_camera.c's,
# the display's framebuffer (libdrm) fake_drm.c's
PCM_WRAPS := open set_params close start drop state recover avail_update delay readi writei
DRM_WRAPS := drmModeGetResources drmModeFreeResources drmModeGetCrtc drmModeFreeCrtc drmModeGetFB2 \
	drmModeFreeFB2 drmIoctl drmCloseBufferHandle
UI_WRAPS := -Wl,--wrap=clock_gettime -Wl,--wrap=time $(PCM_WRAPS:%=-Wl,--wrap=snd_pcm_%) -Wl,--wrap=ioctl $(DRM_WRAPS:%=-Wl,--wrap=%)
define ui_test_rule
$(BUILD)/test_$(1): $(BUILD)/test_$(1).o $(UI_OBJS)
	$$(CXX) $$(LDFLAGS) $(UI_WRAPS) -o $$@ $$^ $$(UI_LDLIBS)
endef
$(foreach t,$(filter-out web terminal github,$(UI_TESTS)),$(eval $(call ui_test_rule,$(t))))

# Browser, the WebKit plugin app (test_web.c): ../../../../apps/web/web.c built for the host as
# web.so (app.json beside it, the test installs both), against fake_webkit.c: libwpe's and
# WPEBackend-fdo's real headers (the flake's), WebKit's declarations in fake_webkit/. The
# program exports phone-ui's functions and all of LVGL to it, as phone-ui.mk links phone-ui.
WEB := ../../../../apps/web
WEB_APP := $(BUILD)/apps/web
WEB_CFLAGS = -Ifake_webkit $(shell pkg-config --cflags wpe-1.0 wpebackend-fdo-1.0 gio-2.0)
$(WEB_APP)/web.so: $(WEB)/web.c $(WEB)/app.json $(BUILD)/ui/abi.h fake_webkit/wpe/webkit.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WEB_CFLAGS) -I$(BUILD)/ui -fPIC -shared -o $@ $<
	cp $(WEB)/app.json $(WEB_APP)/
$(BUILD)/fake_webkit.o: override CFLAGS += $(WEB_CFLAGS)
$(BUILD)/test_web.o: override CFLAGS += $(WEB_CFLAGS) -DWEB_APP='"$(abspath $(WEB_APP))"'
$(BUILD)/test_web: $(BUILD)/test_web.o $(UI_OBJS) $(BUILD)/fake_webkit.o | $(WEB_APP)/web.so
	$(CXX) $(LDFLAGS) $(UI_WRAPS) -rdynamic -o $@ $^ -Wl,--whole-archive $(LVGL)/usr/lib/liblvgl.a \
		-Wl,--no-whole-archive $(UI_LDLIBS) $(shell pkg-config --libs gio-2.0)

# Terminal, a plugin app too (test_terminal.c): ../../../../apps/terminal built for the host as
# terminal.so, its shell the script each test writes (TERMINAL_SHELL: the host user's would be
# theirs), the program linked as test_web
TERMINAL := ../../../../apps/terminal
TERMINAL_APP := $(BUILD)/apps/terminal
$(TERMINAL_APP)/terminal.so: $(TERMINAL)/terminal.c $(TERMINAL)/term_font.c $(TERMINAL)/app.json $(BUILD)/ui/abi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(BUILD)/ui -DTERMINAL_SHELL='"/tmp/terminal-shell"' -fPIC -shared -o $@ $(filter %.c,$^)
	cp $(TERMINAL)/app.json $(TERMINAL_APP)/
$(BUILD)/test_terminal.o: override CFLAGS += -DTERMINAL_APP='"$(abspath $(TERMINAL_APP))"'
$(BUILD)/test_terminal: $(BUILD)/test_terminal.o $(UI_OBJS) | $(TERMINAL_APP)/terminal.so
	$(CXX) $(LDFLAGS) $(UI_WRAPS) -rdynamic -o $@ $^ -Wl,--whole-archive $(LVGL)/usr/lib/liblvgl.a \
		-Wl,--no-whole-archive $(UI_LDLIBS)

# GitHub, a plugin app too (test_github.c): ../../../../apps/github built for the host as
# github.so, its requests answered by fake_curl.c through phone-ui's ui_fetch, the program
# linked as test_web
GITHUB := ../../../../apps/github
GITHUB_APP := $(BUILD)/apps/github
$(GITHUB_APP)/github.so: $(wildcard $(GITHUB)/*.c $(GITHUB)/*.h) $(GITHUB)/app.json $(BUILD)/ui/abi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(BUILD)/ui -fPIC -shared -o $@ $(filter %.c,$^)
	cp $(GITHUB)/app.json $(GITHUB_APP)/
$(BUILD)/test_github.o: override CFLAGS += -DGITHUB_APP='"$(abspath $(GITHUB_APP))"'
$(BUILD)/test_github: $(BUILD)/test_github.o $(UI_OBJS) | $(GITHUB_APP)/github.so
	$(CXX) $(LDFLAGS) $(UI_WRAPS) -rdynamic -o $@ $^ -Wl,--whole-archive $(LVGL)/usr/lib/liblvgl.a \
		-Wl,--no-whole-archive $(UI_LDLIBS)

# The user documentation's screenshots (`just docs-shots`): test_docs.c, a UI test that
# `run` leaves out, saves them in the sandbox as PNG, then they replace DOCS_SHOTS' (ignored by
# git) as lossy WebP (quality 90: a fifteenth of the PNGs' size, text as sharp); FILTER makes
# only those tests' images, keeping the others
DOCS_SHOTS := ../../../../docs/images/generated
$(eval $(call ui_test_rule,docs))
docs-shots: $(BUILD)/test_docs
	@mkdir -p $(VAR_LIB) $(SYS) $(BUILD)/docs-shots && touch $(VAR_LIB)/.phone-ui-tests $(SYS)/.phone-ui-tests
	@rm -f $(BUILD)/docs-shots/*.png
	@flock $(BUILD)/.run $(SANDBOX) env DOCS_SHOTS=$(abspath $(BUILD))/docs-shots ./$< $(FILTER)
	@mkdir -p $(DOCS_SHOTS) && $(if $(FILTER),,rm -f $(DOCS_SHOTS)/*.png $(DOCS_SHOTS)/*.webp &&) \
		find $(BUILD)/docs-shots -name '*.png' -print0 | xargs -0 -r -n1 -P"$$(nproc)" sh -c \
		'cwebp -quiet -q 90 -m 6 "$$1" -o $(DOCS_SHOTS)/"$$(basename "$$1" .png)".webp' _
	@echo "Screenshots: $(abspath $(DOCS_SHOTS))"

# The simulator (`just sim`, sim.c): phone-ui in a window, in real time, with the host's
# libcurl and ALSA (not wrapped) instead of fake_curl.c and fake_alsa.c; the modem and
# Wi-Fi fakes as the tests'. Its own sandbox: the network, the display's sockets, sound,
# /var/lib kept in build/sim/var-lib, with the repository's demo files (books, photos, music)
# in each of its folders that is not there yet. ARGS are sim's (--scale 0.5, --open <app>).
SIM := $(abspath $(BUILD))/sim
SIM_OBJS := $(filter-out $(BUILD)/fake_curl.o $(BUILD)/fake_alsa.o,$(UI_OBJS)) $(BUILD)/sim.o
$(BUILD)/sim.o: override CFLAGS += $(shell pkg-config --cflags sdl3)
$(BUILD)/sim/sim: $(SIM_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -Wl,--wrap=clock_gettime -Wl,--wrap=time -Wl,--wrap=ioctl $(DRM_WRAPS:%=-Wl,--wrap=%) -o $@ $^ \
		$(UI_LDLIBS) -lcurl $(shell pkg-config --libs sdl3)
SIM_SANDBOX = bwrap --ro-bind / / --dev /dev --proc /proc --tmpfs /tmp \
	$(if $(wildcard /dev/snd),--dev-bind /dev/snd /dev/snd) \
	$(if $(wildcard /tmp/.X11-unix),--ro-bind /tmp/.X11-unix /tmp/.X11-unix) \
	--bind $(SIM)/var-lib /var/lib --bind $(SIM)/sys /sys --bind $(SIM) $(SIM) --chdir $(SIM) \
	--setenv __EGL_VENDOR_LIBRARY_FILENAMES "$(EGL_VENDOR)" --setenv TZ UTC0 \
	--setenv ASAN_OPTIONS detect_leaks=0 --die-with-parent --
sim: $(BUILD)/sim/sim
	@mkdir -p $(SIM)/var-lib $(SIM)/sys
	@for d in books photos music; do [ -e $(SIM)/var-lib/$$d ] || cp -r $(DEMO)/$$d $(SIM)/var-lib/; done
	$(SIM_SANDBOX) $(SIM)/sim $(ARGS)

$(BUILD)/src/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ui/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ui/%.o: $(SRC)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d $(BUILD)/src/*.d $(BUILD)/ui/*.d $(BUILD)/ui/*/*.d)

.PHONY: run i18n-check coverage clean docs-shots sim
