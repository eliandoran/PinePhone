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
# fake_bluez.c, BlueZ on a dbus-daemon of the test's own.
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

# One make at a time in a build (several agents, a run by hand): another one building
# meanwhile replaces objects and plugins under this one, which then links or loads them
# built against other headers (a plugin refused as built for another phone-ui), or runs
# programs it is relinking. The make asked runs itself again under the build's lock, for
# building and running; the simulator's window comes after, without it.
ifndef TESTS_LOCKED
GOALS := $(or $(MAKECMDGOALS),$(.DEFAULT_GOAL))
$(firstword $(GOALS)): locked
	@$(if $(filter sim,$(GOALS)),TESTS_LOCKED=1 $(MAKE) --no-print-directory sim)
$(wordlist 2,$(words $(GOALS)),$(GOALS)): locked ; @:
locked:
	@mkdir -p $(BUILD)
	@flock -n $(BUILD)/.lock true || echo "Waiting for another make in $(abspath $(BUILD))"
	@TESTS_LOCKED=1 flock $(BUILD)/.lock $(MAKE) --no-print-directory $(GOALS:sim=sim-build)
.PHONY: $(GOALS) locked
else

# Built against another LVGL (a patch of ours changed it: another store path): the objects
# and programs made again, as nothing depends on the library's files (dated 1970 in the store)
ifneq ($(LVGL),$(shell cat $(BUILD)/lvgl 2>/dev/null))
$(shell mkdir -p $(BUILD) && find $(BUILD) -path $(BUILD)/coverage -prune -o -type f \
	\( -name '*.o' -o -name '*.so' -o -name 'test_*' -perm -u+x \) -print -delete >/dev/null; \
	echo '$(LVGL)' > $(BUILD)/lvgl)
endif

TESTS := json data calculator sms_pdu emoji i18n abi correct usage
UI_TESTS := home cpu external navigation notes tasks alarms phone messages notifications contacts settings clock calendar files gallery snake lock shade volume weather maps browser books assistant recorder keyboard music compass wifi bluetooth modem mobile_data ussd calls camera screenshot splash web terminal github telegram email pairing doom genesis usb_mode plugins trilium

# What each unit test links besides itself and stubs.c (sources of ../src)
json_SRCS := system/json.c
data_SRCS := data/data.c system/json.c system/i18n.c
calculator_SRCS := gen/ui_font_48.c gen/ui_font_32.c gen/ui_font_26.c system/i18n.c
sms_pdu_SRCS := modem/sms_pdu.c
i18n_SRCS := system/i18n.c
correct_SRCS := ui/correct.c
usage_SRCS := system/usage.c
emoji_LDLIBS := -lpng
# test_emoji.c copies the images from there into its own EMOJI_DIR
$(BUILD)/test_emoji.o: override CFLAGS += -DEMOJI_IMAGES='"$(EMOJI)"'
# test_docs.c's pictures, music and books: the repository's demo files
DEMO := ../../../../demo
$(BUILD)/test_docs.o: override CFLAGS += -DDEMO_DIR='"$(abspath $(DEMO))"'
# The translations: ../po's catalogs in C, as for the image (gen-translations.py)
PO := ../po
LANGUAGES := $(BUILD)/i18n_languages.o
$(BUILD)/i18n_languages.c: ../gen-translations.py $(wildcard $(PO)/*.po)
	@python3 ../gen-translations.py $(PO) $@
$(LANGUAGES): $(BUILD)/i18n_languages.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_i18n $(BUILD)/test_data $(BUILD)/test_calculator: $(LANGUAGES)
# The keyboard's dictionaries: ../words' lists in C, as for the image (gen-words.py)
WORDS := $(BUILD)/words.o
$(BUILD)/words.c: ../gen-words.py $(wildcard ../words/*.txt)
	@python3 ../gen-words.py ../words $@
$(WORDS): $(BUILD)/words.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_correct: $(WORDS)
# test_i18n.c reads the sources for the texts marked for translation
$(BUILD)/test_i18n.o: override CFLAGS += -DSRC_DIR='"$(abspath $(SRC))"'
# test_abi.c runs gen-abi.py on headers of its own
$(BUILD)/test_abi.o: override CFLAGS += -DGEN_ABI='"$(abspath ../gen-abi.py)"'

SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=undefined
ifdef ANALYZE
# GCC's static analyzer instead (`make analyze`): UBSan's checks, which abort, would end
# the paths it follows
SANITIZE :=
endif
# As phone-ui's (Makefile.build), but errors: the tests keep the tree free of warnings,
# while a toolchain that warns about more does not stop the image's build
WARNINGS := -Wall -Wextra -Wno-missing-field-initializers -Wshadow -Wvla -Wimplicit-fallthrough -Werror
FLAGS := -O1 -g $(WARNINGS) -fno-omit-frame-pointer $(SANITIZE) -MMD -MP \
	-DLV_CONF_INCLUDE_SIMPLE -I$(LVGL)/usr/include/lvgl -I$(SRC)
override CFLAGS += $(FLAGS) -std=gnu17 -Wstrict-prototypes -Wmissing-prototypes
ifdef ANALYZE
# Its findings reported, not errors
override CFLAGS += -fanalyzer -Wno-error
endif
override CXXFLAGS += $(FLAGS) -std=c++17 -D_GLIBCXX_ASSERTIONS
LDFLAGS := $(SANITIZE)
LDLIBS := $(LVGL)/usr/lib/liblvgl.a -lm
# As phone-ui.mk links it, but libcurl: fake_curl.c answers what the tests serve
UI_LDLIBS := $(LVGL)/usr/lib/liblvgl.a -lpng -ljpeg -lwebp -ldrm -llitehtml -lgumbo -lmpg123 -lz \
	-lasound -ldbus-1 -lgbm -lGLESv2 -lEGL -lstdc++ -lpthread -lm

ifdef COVERAGE
# -Og, not -O0: nix's compiler wrapper sets _FORTIFY_SOURCE, which warns without optimization
override CFLAGS += --coverage -Og -DTEST_COVERAGE
override CXXFLAGS += --coverage -Og
LDFLAGS += --coverage
endif
# Generated sources (fonts, images), not in the report
GENERATED := ui_font_.*|md_fonts|shortcodes|md4c|icons|emoji_list|splash_image

# Unit tests: contacts and conversations in the build, the fonts' emoji fallbacks
# (emoji.c, with all of its images) LVGL's Montserrat
$(BUILD)/src/system/usage.o $(BUILD)/test_usage.o: override CFLAGS += -DUSAGE_DIR='"$(abspath $(BUILD))/usage"'
$(BUILD)/src/data/data.o $(BUILD)/test_data.o: override CFLAGS += -DCONTACTS='"$(abspath $(BUILD))/contacts"' \
	-DMESSAGES='"$(abspath $(BUILD))/messages"'
FONTS := 48 32 26
$(foreach s,$(FONTS),$(eval $(BUILD)/src/gen/ui_font_$(s).o: override CFLAGS += -Dui_emoji_$(s)=lv_font_montserrat_$(s)))

# UI tests: phone-ui as in the image
UI_SRCS := $(filter-out main.c,$(shell cd $(SRC) && find . -name '*.c' | sed 's|^\./||'))
UI_CXX_SRCS := $(shell cd $(SRC) && find . -name '*.cpp' | sed 's|^\./||')
UI_OBJS := $(UI_SRCS:%.c=$(BUILD)/ui/%.o) $(UI_CXX_SRCS:%.cpp=$(BUILD)/ui/%.o) $(BUILD)/ui_test.o $(BUILD)/fake_curl.o $(BUILD)/fake_alsa.o \
	$(BUILD)/fake_wpa.o $(BUILD)/fake_modem.o $(BUILD)/fake_camera.o $(BUILD)/fake_drm.o $(BUILD)/fake_bluez.o $(LANGUAGES) $(WORDS)
$(BUILD)/ui/%.o: override CFLAGS += $(shell pkg-config --cflags libdrm dbus-1) -I$(BUILD)/ui
$(BUILD)/fake_drm.o: override CFLAGS += $(shell pkg-config --cflags libdrm)
# BlueZ: fake_bluez.c's bus, in the sandbox's /tmp
$(BUILD)/fake_bluez.o: override CFLAGS += $(shell pkg-config --cflags dbus-1)
$(BUILD)/ui/net/bluetooth.o: override CFLAGS += -DBT_BUS='"unix:path=/tmp/bus"'
# The emoji images of the image, from the tests shell (EMOJI)
$(BUILD)/ui/ui/emoji.o: override CFLAGS += -DEMOJI_DIR='"$(EMOJI)"'
$(BUILD)/ui/system/external.o: $(BUILD)/ui/abi.h
# wpa_supplicant's control sockets: fake_wpa.c's, in the sandbox's /tmp
# and the routing table, test_wifi.c's (the sandbox has no network)
$(BUILD)/ui/apps/settings/wifi.o: override CFLAGS += -DWPA_CTRL_DIR='"/tmp/wpa_supplicant"' \
	-DPROC_NET_ROUTE='"/tmp/route"'
# The board's modem.conf, ui_test.c's (its port fake_modem.c's pty)
$(BUILD)/ui/modem/modem.o: override CFLAGS += -DMODEM_CONF='"/tmp/modem.conf"'
# The apps' folders for the board's USB mode script (MTP), in the sandbox's /tmp
$(BUILD)/ui/system/folders.o: override CFLAGS += -DFOLDERS_LIST='"/tmp/folders"'
# The neighbour table (the computer's address on USB), the tests'
$(BUILD)/ui/net/usb_net.o: override CFLAGS += -DPROC_NET_ARP='"/tmp/arp"'
$(BUILD)/ui/system/usage_sample.o: override CFLAGS += -DUSAGE_PROC='"/tmp/proc"'
# The kernel's command line (the boot slot in About), the tests' (the host's is not a phone's)
$(BUILD)/ui/apps/settings/about.o: override CFLAGS += -DPROC_CMDLINE='"/tmp/cmdline"'
# The board's USB mode script, ui_test.c's (logs what it is asked)
$(BUILD)/ui/net/usb_mode.o: override CFLAGS += -DUSB_MODE_SCRIPT='"/tmp/usb-mode.sh"'
# Computers asking for SSH access: over the sandbox's loopback, not usb0
$(BUILD)/ui/net/pairing.o: override CFLAGS += -DPAIRING_DEVICE='"lo"'
# The boot splash's once-per-boot mark, in the sandbox's /tmp (/run is the host's)
$(BUILD)/ui/shell/splash.o: override CFLAGS += -DMARKER='"/tmp/phone-ui.splash"'
# The mark that the home screen was drawn, in the sandbox's /tmp too
$(BUILD)/ui/shell/home.o: override CFLAGS += -DHOME_SHOWN='"/tmp/phone-ui.home"'

# The plugins' ABI and what phone-ui exports to them, as Makefile.build makes them
# (gen-abi.py: a fingerprint per declaration, the functions and variables declared)
$(BUILD)/ui/abi.h $(BUILD)/ui/exports.list &: ../gen-abi.py $(SRC)/ui.h $(SRC)/json.h $(SRC)/i18n.h
	@mkdir -p $(dir $@)
	python3 ../gen-abi.py --exports $(BUILD)/ui/exports.list $(BUILD)/ui/abi.h $(LVGL)/usr/include/lvgl $(filter %.h,$^)

# The sandbox: /var/lib and /sys are directories of the build that ui_test.c fills
# (a phone's sysfs), $(1)/var-lib and $(1)/sys; no network but loopback, the time zone
# UTC as on the phone (no zoneinfo there, the host's /etc/localtime here). Mesa's shader
# cache in the build: llvmpipe compiling LVGL's shaders again was half of a UI test. Two
# drawing threads each: a thread per core in every program (all run at once) was slower.
# LeakSanitizer's suppressions (test.h checks each test), lsan.supp.
VAR_LIB := $(abspath $(BUILD))/var-lib
SYS := $(abspath $(BUILD))/sys
sandbox = bwrap --ro-bind / / --dev /dev --proc /proc --tmpfs /tmp --unshare-net \
	--bind $(1)/var-lib /var/lib --bind $(1)/sys /sys --bind $(abspath $(BUILD)) $(abspath $(BUILD)) \
	--setenv __EGL_VENDOR_LIBRARY_FILENAMES "$(EGL_VENDOR)" \
	--setenv MESA_SHADER_CACHE_DIR $(abspath $(BUILD))/mesa-cache --setenv TZ UTC0 \
	--setenv LP_NUM_THREADS 2 --setenv LSAN_OPTIONS suppressions=$(abspath lsan.supp) \
	--die-with-parent --
SANDBOX = $(call sandbox,$(abspath $(BUILD)))

# All of ../src first (../i18n-check.py): each text a user reads marked for translation,
# or NON-NLS; the catalogs checked by gettext (formats). Then the plugin apps with a po/
# of their own the same way, their catalogs complete too (test_i18n.c's checks for phone-ui)
APP_CATALOGS := $(patsubst %/po,%,$(wildcard ../../../../apps/*/po))
i18n-check:
	@python3 ../i18n-check.py --strict $(SRC)
	@for po in $(PO)/*.po; do msgfmt --check-format --check-domain -o /dev/null $$po || exit 1; done
	@for app in $(APP_CATALOGS); do \
		python3 ../i18n-check.py --app $$app --strict $$app || exit 1; \
		for po in $$app/po/*.po; do msgfmt --check-format --check-domain -o /dev/null $$po || exit 1; done; \
	done

# The programs JOBS at a time (a core each; more and the tests' real-time waits run out
# on CI's few cores), each with its /var/lib and /sys (build/run/test_<name>/), their
# output shown in order as each ends. FILTER: the tests whose name contains it, in every
# program; `program/filter` (`settings/slider`, `settings/` for all of it): only that
# program's, the only one built, without the translations' check.
RUN := $(abspath $(BUILD))/run
JOBS ?= $(shell nproc)
PROGRAM := $(if $(findstring /,$(FILTER)),$(firstword $(subst /, ,$(FILTER))))
ifneq ($(PROGRAM),)
ifeq ($(filter $(PROGRAM),$(TESTS) $(UI_TESTS)),)
$(error No test program "$(PROGRAM)": $(TESTS) $(UI_TESTS))
endif
endif
NAME_FILTER := $(if $(PROGRAM),$(patsubst $(PROGRAM)/%,%,$(FILTER)),$(FILTER))
run: $(if $(PROGRAM),$(BUILD)/test_$(PROGRAM),$(TESTS:%=$(BUILD)/test_%) $(UI_TESTS:%=$(BUILD)/test_%)) \
	| $(if $(PROGRAM),,i18n-check)
	@sh -c ' \
		rm -rf $(RUN); started=0; \
		for t in "$$@"; do \
			while [ $$((started - $$(cat $(RUN)/*/status 2>/dev/null | wc -l))) -ge $(JOBS) ]; do \
				sleep 0.1; \
			done; \
			started=$$((started + 1)); \
			d=$(RUN)/$${t##*/}; mkdir -p $$d/var-lib $$d/sys; \
			touch $$d/var-lib/.phone-ui-tests $$d/sys/.phone-ui-tests; \
			{ $(call sandbox,$$d) ./$$t $(NAME_FILTER) > $$d/log 2>&1; echo $$? > $$d/status; } & \
		done; \
		failed=0; \
		for t in "$$@"; do \
			d=$(RUN)/$${t##*/}; while [ ! -s $$d/status ]; do sleep 0.1; done; \
			cat $$d/log; [ "$$(cat $$d/status)" = 0 ] || failed=1; \
		done; \
		exit $$failed' sh $^

# The report even when tests fail (then fails too); counts from earlier runs dropped;
# no limit on hit counts: loops over every pixel of the screen reach gcovr's 2^32 "suspicious" one;
# the browser's nanosvg headers (not ours) left out
# Then the external apps' (../../../../apps, those with tests: their .so built in build/coverage/apps)
APPS := ../../../../apps
COV := $(BUILD)/coverage
coverage:
	@mkdir -p $(COV) && find $(COV) -name '*.gcda' -delete
	@$(MAKE) --no-print-directory run BUILD=$(COV) COVERAGE=1; status=$$?; \
	mkdir -p $(COV)/html && \
	gcovr --root $(SRC) --object-directory $(COV) --exclude '.*/($(GENERATED))\.c' --exclude '.*/nanosvg(rast)?\.h' \
		--gcov-suspicious-hits-threshold 0 \
		--txt --print-summary --html-details $(COV)/html/index.html \
		--json-summary $(COV)/summary.json $(COV) && \
	jq -r '.files[].filename' $(COV)/summary.json | sort > $(COV)/tested && \
	(cd $(SRC) && find . -name '*.c' | sed 's|^\./||' | grep -Ev '(^|/)($(GENERATED))\.c$$' | sort) \
		| comm -23 - $(COV)/tested | tr '\n' ' ' | sed 's/^./No tests: &/;s/ $$/\n/' && \
	echo "Line by line: $(abspath $(COV))/html/index.html" && \
	mkdir -p $(COV)/apps $(COV)/html/apps && \
	gcovr --root $(APPS) --object-directory $(COV)/apps \
		--gcov-suspicious-hits-threshold 0 \
		--txt --print-summary --html-details $(COV)/html/apps/index.html $(COV)/apps && \
	ls $(APPS) | grep -vxF "$$(ls $(COV)/apps)" | tr '\n' ' ' | sed 's/^./External apps without tests: &/;s/ $$/\n/' && \
	echo "Line by line: $(abspath $(COV))/html/apps/index.html" && exit $$status

# `make analyze` (`just analyze`): GCC's static analyzer over what the phone runs, phone-ui's
# sources and the plugin apps' (not the tests' code), compiled again each time in
# build/analyze, nothing linked. analyze.awk reports the findings analyze.known does not list
# (those reviewed and left: false ones, allocations not checked) and fails on them
AN := $(BUILD)/analyze
AN_PLUGINS := web/web terminal/terminal github/github assistant/assistant email/email \
	telegram/telegram doom/doom genesis/genesis trilium/trilium
analyze:
	@rm -rf $(AN) && mkdir -p $(AN)
	@$(MAKE) --no-print-directory --output-sync=target -k BUILD=$(AN) ANALYZE=1 $(AN)/ui/main.o $(UI_SRCS:%.c=$(AN)/ui/%.o) \
		$(AN_PLUGINS:%=$(AN)/apps/%.so) > /dev/null 2> $(AN)/log; status=$$?; \
	awk -f analyze.awk analyze.known $(AN)/log || status=1; \
	[ $$status = 0 ] || echo "Full output: $(abspath $(AN))/log"; exit $$status

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
# alarms.c's wake timers (ui_test.c): the sandbox may not set them
WAKE_WRAPS := -Wl,--wrap=timerfd_create -Wl,--wrap=timerfd_settime
UI_WRAPS := -Wl,--wrap=clock_gettime -Wl,--wrap=time $(WAKE_WRAPS) $(PCM_WRAPS:%=-Wl,--wrap=snd_pcm_%) -Wl,--wrap=ioctl $(DRM_WRAPS:%=-Wl,--wrap=%)
# The plugin apps' time() the program's (exported), so ui_test_wall_clock() sets their dates
# too; not clock_gettime(): Doom and Genesis pace their games by the real monotonic clock
PLUGIN_WRAPS := -Wl,--wrap=time
# What the programs loading plugin apps export to them, as phone-ui (Makefile.build): what
# their headers declare and LVGL, not the rest of phone-ui; then the fakes they call
EXPORTS = -Wl,--dynamic-list=$(BUILD)/ui/exports.list -Wl,--dynamic-list=exports-tests.list
$(UI_TESTS:%=$(BUILD)/test_%) $(BUILD)/test_docs $(BUILD)/sim/sim: | $(BUILD)/ui/exports.list
define ui_test_rule
$(BUILD)/test_$(1): $(BUILD)/test_$(1).o $(UI_OBJS)
	$$(CXX) $$(LDFLAGS) $(UI_WRAPS) -o $$@ $$^ $$(UI_LDLIBS)
endef
$(foreach t,$(filter-out web terminal github assistant telegram email doom genesis plugins trilium,$(UI_TESTS)),$(eval $(call ui_test_rule,$(t))))

# The fake plugin app (test_plugins.c): fake_plugin.c as fake.so, its app_abi the test's,
# the program linked as test_web
FAKE_APP := $(BUILD)/apps/fake
$(FAKE_APP)/fake.so: fake_plugin.c $(BUILD)/ui/abi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(BUILD)/ui -fPIC -shared -o $@ $<
$(BUILD)/test_plugins.o: override CFLAGS += -I$(BUILD)/ui -DFAKE_APP='"$(abspath $(FAKE_APP))"'
$(BUILD)/test_plugins.o: $(BUILD)/ui/abi.h
$(BUILD)/test_plugins: $(BUILD)/test_plugins.o $(UI_OBJS) | $(FAKE_APP)/fake.so
	$(CXX) $(LDFLAGS) $(UI_WRAPS) $(EXPORTS) -o $@ $^ -Wl,--whole-archive $(LVGL)/usr/lib/liblvgl.a \
		-Wl,--no-whole-archive $(UI_LDLIBS)

# Browser, the WebKit plugin app (test_web.c): ../../../../apps/web/web.c built for the host as
# web.so (app.json beside it, the test installs both), against fake_webkit.c: libwpe's and
# WPEBackend-fdo's real headers (the flake's), WebKit's declarations in fake_webkit/. The
# program exports phone-ui's functions and all of LVGL to it, as phone-ui.mk links phone-ui.
# A plugin's catalogs in C, as its Makefile makes them (gen-translations.py --app):
# $(BUILD)/apps/<name>-i18n.c, compiled into it with -DUI_APP_CATALOG
define app_catalog
$(BUILD)/apps/$(1)-i18n.c: ../gen-translations.py $(wildcard ../../../../apps/$(1)/po/*.po)
	@mkdir -p $$(dir $$@)
	@python3 ../gen-translations.py --app ../../../../apps/$(1)/po $$@
endef
$(foreach a,web terminal github assistant email trilium,$(eval $(call app_catalog,$(a))))

WEB := ../../../../apps/web
WEB_APP := $(BUILD)/apps/web
# libwpe's headers as the system's, as apps/web's Makefile: declarations without prototypes
WEB_CFLAGS = -Ifake_webkit $(patsubst -I%,-isystem %,$(shell pkg-config --cflags wpe-1.0 wpebackend-fdo-1.0 gio-2.0))
$(WEB_APP)/web.so: $(WEB)/web.c $(BUILD)/apps/web-i18n.c $(WEB)/app.json $(BUILD)/ui/abi.h fake_webkit/wpe/webkit.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WEB_CFLAGS) -I$(BUILD)/ui -DUI_APP_CATALOG -fPIC -shared $(PLUGIN_WRAPS) -o $@ $(filter %.c,$^)
	cp $(WEB)/app.json $(WEB_APP)/
$(BUILD)/fake_webkit.o: override CFLAGS += $(WEB_CFLAGS)
$(BUILD)/test_web.o: override CFLAGS += $(WEB_CFLAGS) -DWEB_APP='"$(abspath $(WEB_APP))"'
$(BUILD)/test_web: $(BUILD)/test_web.o $(UI_OBJS) $(BUILD)/fake_webkit.o | $(WEB_APP)/web.so
	$(CXX) $(LDFLAGS) $(UI_WRAPS) $(EXPORTS) -o $@ $^ -Wl,--whole-archive $(LVGL)/usr/lib/liblvgl.a \
		-Wl,--no-whole-archive $(UI_LDLIBS) $(shell pkg-config --libs gio-2.0)

# Terminal, a plugin app too (test_terminal.c): ../../../../apps/terminal built for the host as
# terminal.so, its shell the script each test writes (TERMINAL_SHELL: the host user's would be
# theirs), the program linked as test_web
TERMINAL := ../../../../apps/terminal
TERMINAL_APP := $(BUILD)/apps/terminal
TERMINAL_I18N := $(BUILD)/apps/terminal-i18n.c
$(TERMINAL_APP)/terminal.so: $(TERMINAL)/terminal.c $(TERMINAL_I18N) $(TERMINAL)/app.json \
		$(BUILD)/ui/abi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(BUILD)/ui -DUI_APP_CATALOG -DTERMINAL_SHELL='"/tmp/terminal-shell"' -fPIC -shared $(PLUGIN_WRAPS) \
		-o $@ $(filter %.c,$^)
	cp $(TERMINAL)/app.json $(TERMINAL_APP)/
$(BUILD)/test_terminal.o: override CFLAGS += -DTERMINAL_APP='"$(abspath $(TERMINAL_APP))"'
$(BUILD)/test_terminal: $(BUILD)/test_terminal.o $(UI_OBJS) | $(TERMINAL_APP)/terminal.so
	$(CXX) $(LDFLAGS) $(UI_WRAPS) $(EXPORTS) -o $@ $^ -Wl,--whole-archive $(LVGL)/usr/lib/liblvgl.a \
		-Wl,--no-whole-archive $(UI_LDLIBS)

# GitHub, a plugin app too (test_github.c): ../../../../apps/github built for the host as
# github.so, its requests answered by fake_curl.c through phone-ui's ui_fetch, the program
# linked as test_web
GITHUB := ../../../../apps/github
GITHUB_APP := $(BUILD)/apps/github
$(GITHUB_APP)/github.so: $(wildcard $(GITHUB)/*.c $(GITHUB)/*.h) $(BUILD)/apps/github-i18n.c $(GITHUB)/app.json \
		$(BUILD)/ui/abi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(BUILD)/ui -DUI_APP_CATALOG -fPIC -shared $(PLUGIN_WRAPS) -o $@ $(filter %.c,$^) -lpng -lm
	cp $(GITHUB)/app.json $(GITHUB_APP)/
$(BUILD)/test_github.o: override CFLAGS += -DGITHUB_APP='"$(abspath $(GITHUB_APP))"' -DGITHUB_FIXTURES='"$(abspath github)"'
$(BUILD)/test_github: $(BUILD)/test_github.o $(UI_OBJS) | $(GITHUB_APP)/github.so
	$(CXX) $(LDFLAGS) $(UI_WRAPS) $(EXPORTS) -o $@ $^ -Wl,--whole-archive $(LVGL)/usr/lib/liblvgl.a \
		-Wl,--no-whole-archive $(UI_LDLIBS)

# Assistant, a plugin app too (test_assistant.c): ../../../../apps/assistant built for the host
# as assistant.so, the model's API answered by fake_curl.c through phone-ui's ui_fetch, the
# program linked as test_web
ASSISTANT := ../../../../apps/assistant
ASSISTANT_APP := $(BUILD)/apps/assistant
$(ASSISTANT_APP)/assistant.so: $(wildcard $(ASSISTANT)/*.c $(ASSISTANT)/*.h) $(BUILD)/apps/assistant-i18n.c \
		$(ASSISTANT)/app.json $(BUILD)/ui/abi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(BUILD)/ui -DUI_APP_CATALOG -fPIC -shared $(PLUGIN_WRAPS) -o $@ $(filter %.c,$^)
	cp $(ASSISTANT)/app.json $(ASSISTANT_APP)/
$(BUILD)/test_assistant.o: override CFLAGS += -DASSISTANT_APP='"$(abspath $(ASSISTANT_APP))"'
$(BUILD)/test_assistant: $(BUILD)/test_assistant.o $(UI_OBJS) | $(ASSISTANT_APP)/assistant.so
	$(CXX) $(LDFLAGS) $(UI_WRAPS) $(EXPORTS) -o $@ $^ -Wl,--whole-archive $(LVGL)/usr/lib/liblvgl.a \
		-Wl,--no-whole-archive $(UI_LDLIBS)

# Email, a plugin app too (test_email.c): ../../../../apps/email built for the host as
# email.so with libetpan (the flake's), the providers' settings answered by fake_curl.c, its
# IMAP server fake_imap.c on the sandbox's loopback; the program linked as test_web
EMAIL := ../../../../apps/email
EMAIL_APP := $(BUILD)/apps/email
$(EMAIL_APP)/email.so: $(wildcard $(EMAIL)/*.c $(EMAIL)/*.h) $(BUILD)/apps/email-i18n.c $(EMAIL)/app.json \
		$(BUILD)/ui/abi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(BUILD)/ui -DUI_APP_CATALOG -fPIC -shared $(PLUGIN_WRAPS) -o $@ $(filter %.c,$^) -letpan -lssl -lcrypto -lpthread
	cp $(EMAIL)/app.json $(EMAIL_APP)/
$(BUILD)/test_email.o: override CFLAGS += -DEMAIL_APP='"$(abspath $(EMAIL_APP))"'
$(BUILD)/test_email: $(BUILD)/test_email.o $(UI_OBJS) $(BUILD)/fake_imap.o | $(EMAIL_APP)/email.so
	$(CXX) $(LDFLAGS) $(UI_WRAPS) $(EXPORTS) -o $@ $^ -Wl,--whole-archive $(LVGL)/usr/lib/liblvgl.a \
		-Wl,--no-whole-archive $(UI_LDLIBS)

# Trilium, a plugin app too (test_trilium.c): ../../../../apps/trilium built for the host as
# trilium.so with the host's SQLite and OpenSSL (the flake's), its libcurl the program's (fake_curl.c,
# exported: its sync runs requests on a thread of its own), Trilium's schema in C as its Makefile makes
# it; the program linked as test_web, with SQLite for the test's own queries
TRILIUM := ../../../../apps/trilium
TRILIUM_APP := $(BUILD)/apps/trilium
$(BUILD)/apps/trilium-schema.c: $(TRILIUM)/schema.sql $(TRILIUM)/gen-schema.sh
	@mkdir -p $(dir $@)
	@sh $(TRILIUM)/gen-schema.sh $< $@
$(TRILIUM_APP)/trilium.so: $(wildcard $(TRILIUM)/*.c $(TRILIUM)/*.h) $(BUILD)/apps/trilium-i18n.c \
		$(BUILD)/apps/trilium-schema.c $(TRILIUM)/app.json $(BUILD)/ui/abi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(BUILD)/ui -I$(TRILIUM) -DUI_APP_CATALOG -fPIC -shared $(PLUGIN_WRAPS) -o $@ $(filter %.c,$^) \
		-lsqlite3 -lcrypto -lpthread
	cp $(TRILIUM)/app.json $(TRILIUM_APP)/
$(BUILD)/test_trilium.o: override CFLAGS += -DTRILIUM_APP='"$(abspath $(TRILIUM_APP))"'
$(BUILD)/test_trilium: $(BUILD)/test_trilium.o $(UI_OBJS) | $(TRILIUM_APP)/trilium.so
	$(CXX) $(LDFLAGS) $(UI_WRAPS) $(EXPORTS) -o $@ $^ -Wl,--whole-archive $(LVGL)/usr/lib/liblvgl.a \
		-Wl,--no-whole-archive $(UI_LDLIBS) -lsqlite3 -lcrypto

# Telegram, a plugin app too (test_telegram.c):../../../../apps/telegram built for the host
# as telegram.so (its runpath lib/, as `just app telegram` builds it), TDLib fake_tdjson.c in
# the program, reached through fake_tdjson/libtdjson.c's libtdjson.so in the app's lib/; the
# program linked as test_web
TELEGRAM := ../../../../apps/telegram
TELEGRAM_APP := $(BUILD)/apps/telegram
$(eval $(call app_catalog,telegram))
$(TELEGRAM_APP)/telegram.so: $(wildcard $(TELEGRAM)/*.c $(TELEGRAM)/*.h) $(BUILD)/apps/telegram-i18n.c \
		$(TELEGRAM)/app.json $(BUILD)/ui/abi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(BUILD)/ui -DUI_APP_CATALOG -fPIC -shared $(PLUGIN_WRAPS) -Wl,-rpath,'$$ORIGIN/lib' -o $@ \
		$(filter %.c,$^) $(shell pkg-config --libs libwebp libsharpyuv)
	cp $(TELEGRAM)/app.json $(TELEGRAM_APP)/
$(TELEGRAM_APP)/lib/libtdjson.so: fake_tdjson/libtdjson.c
	@mkdir -p $(dir $@)
	$(CC) -O1 -g -std=gnu17 $(WARNINGS) -Wstrict-prototypes -Wmissing-prototypes -fPIC -shared -Wl,-soname,libtdjson.so -o $@ $<
$(BUILD)/test_telegram.o: override CFLAGS += -DTELEGRAM_APP='"$(abspath $(TELEGRAM_APP))"'
$(BUILD)/test_telegram: $(BUILD)/test_telegram.o $(UI_OBJS) $(BUILD)/fake_tdjson.o | \
		$(TELEGRAM_APP)/telegram.so $(TELEGRAM_APP)/lib/libtdjson.so
	$(CXX) $(LDFLAGS) $(UI_WRAPS) $(EXPORTS) -o $@ $^ -Wl,--whole-archive $(LVGL)/usr/lib/liblvgl.a \
		-Wl,--no-whole-archive $(UI_LDLIBS)

# Doom, a plugin app too (test_doom.c): ../../../../apps/doom's doom.so built for the host,
# its engine (lib/doomengine.so, loaded when a game starts) fake_doom/doomengine.c, whose
# state is test_doom.c's (fake_doom.h); the program linked as test_web
DOOM := ../../../../apps/doom
DOOM_APP := $(BUILD)/apps/doom
$(eval $(call app_catalog,doom))
$(DOOM_APP)/doom.so: $(DOOM)/plugin.c $(DOOM)/list.c $(DOOM)/screen.c $(DOOM)/touch.c $(DOOM)/wad.c $(wildcard $(DOOM)/*.h) \
		$(BUILD)/apps/doom-i18n.c $(DOOM)/app.json $(BUILD)/ui/abi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(BUILD)/ui -DUI_APP_CATALOG -fPIC -shared $(PLUGIN_WRAPS) -o $@ $(filter %.c,$^)
	cp $(DOOM)/app.json $(DOOM_APP)/
$(DOOM_APP)/lib/doomengine.so: fake_doom/doomengine.c fake_doom/fake_doom.h $(DOOM)/doom.h
	@mkdir -p $(dir $@)
	$(CC) -O1 -g -std=gnu17 $(WARNINGS) -Wstrict-prototypes -Wmissing-prototypes -fPIC -shared -o $@ $<
$(BUILD)/test_doom.o: override CFLAGS += -DDOOM_APP='"$(abspath $(DOOM_APP))"'
$(BUILD)/test_doom: $(BUILD)/test_doom.o $(UI_OBJS) | $(DOOM_APP)/doom.so $(DOOM_APP)/lib/doomengine.so
	$(CXX) $(LDFLAGS) $(UI_WRAPS) $(EXPORTS) -o $@ $^ -Wl,--whole-archive $(LVGL)/usr/lib/liblvgl.a \
		-Wl,--no-whole-archive $(UI_LDLIBS)

# Genesis, a plugin app too (test_genesis.c): ../../../../apps/genesis's genesis.so built for
# the host with the core's header (LIBRETRO, from the flake), its core
# (lib/genesis_plus_gx_libretro.so, loaded when a game starts) fake_genesis/libretro.c, whose
# state is test_genesis.c's (fake_genesis.h); the program linked as test_web
GENESIS := ../../../../apps/genesis
GENESIS_APP := $(BUILD)/apps/genesis
$(eval $(call app_catalog,genesis))
$(GENESIS_APP)/genesis.so: $(GENESIS)/genesis.c $(BUILD)/apps/genesis-i18n.c $(GENESIS)/app.json $(BUILD)/ui/abi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(BUILD)/ui -I$(LIBRETRO) -DUI_APP_CATALOG -fPIC -shared $(PLUGIN_WRAPS) -o $@ $(filter %.c,$^)
	cp $(GENESIS)/app.json $(GENESIS_APP)/
$(GENESIS_APP)/lib/genesis_plus_gx_libretro.so: fake_genesis/libretro.c fake_genesis/fake_genesis.h
	@mkdir -p $(dir $@)
	$(CC) -O1 -g -std=gnu17 $(WARNINGS) -Wstrict-prototypes -Wmissing-prototypes -I$(LIBRETRO) -fPIC -shared -o $@ $<
$(BUILD)/test_genesis.o: override CFLAGS += -DGENESIS_APP='"$(abspath $(GENESIS_APP))"' -I$(LIBRETRO)
$(BUILD)/test_genesis: $(BUILD)/test_genesis.o $(UI_OBJS) | $(GENESIS_APP)/genesis.so \
		$(GENESIS_APP)/lib/genesis_plus_gx_libretro.so
	$(CXX) $(LDFLAGS) $(UI_WRAPS) $(EXPORTS) -o $@ $^ -Wl,--whole-archive $(LVGL)/usr/lib/liblvgl.a \
		-Wl,--no-whole-archive $(UI_LDLIBS)

# The user documentation's screenshots (`just docs-shots`): test_docs.c, a UI test that
# `run` leaves out, saves them in the sandbox as PNG, then they replace DOCS_SHOTS' (ignored by
# git) as lossy WebP (quality 90: a fifteenth of the PNGs' size, text as sharp); FILTER makes
# only those tests' images, keeping the others
DOCS_SHOTS := ../../../../docs/images/generated
# The external apps' pages too (docs/telegram.c, github.c...): linked as their tests, with
# TDLib's fake, their apps built as for them
$(BUILD)/test_docs.o: override CFLAGS += -DTELEGRAM_APP='"$(abspath $(TELEGRAM_APP))"' \
	-DGITHUB_APP='"$(abspath $(GITHUB_APP))"' -DTERMINAL_APP='"$(abspath $(TERMINAL_APP))"' \
	-DASSISTANT_APP='"$(abspath $(ASSISTANT_APP))"' \
	-DDOOM_APP='"$(abspath $(DOOM_APP))"' -DGENESIS_APP='"$(abspath $(GENESIS_APP))"'
$(BUILD)/test_docs: $(BUILD)/test_docs.o $(UI_OBJS) $(BUILD)/fake_tdjson.o | \
		$(TELEGRAM_APP)/telegram.so $(TELEGRAM_APP)/lib/libtdjson.so $(GITHUB_APP)/github.so \
		$(TERMINAL_APP)/terminal.so $(ASSISTANT_APP)/assistant.so $(DOOM_APP)/doom.so $(DOOM_APP)/lib/doomengine.so \
		$(GENESIS_APP)/genesis.so $(GENESIS_APP)/lib/genesis_plus_gx_libretro.so
	$(CXX) $(LDFLAGS) $(UI_WRAPS) $(EXPORTS) -o $@ $^ -Wl,--whole-archive $(LVGL)/usr/lib/liblvgl.a \
		-Wl,--no-whole-archive $(UI_LDLIBS)
docs-shots: $(BUILD)/test_docs
	@mkdir -p $(VAR_LIB) $(SYS) $(BUILD)/docs-shots && touch $(VAR_LIB)/.phone-ui-tests $(SYS)/.phone-ui-tests
	@rm -f $(BUILD)/docs-shots/*.png
	@$(SANDBOX) env DOCS_SHOTS=$(abspath $(BUILD))/docs-shots ./$< $(FILTER)
	@mkdir -p $(DOCS_SHOTS) && $(if $(FILTER),,rm -f $(DOCS_SHOTS)/*.png $(DOCS_SHOTS)/*.webp &&) \
		find $(BUILD)/docs-shots -name '*.png' -print0 | xargs -0 -r -n1 -P"$$(nproc)" sh -c \
		'cwebp -quiet -q 90 -m 6 "$$1" -o $(DOCS_SHOTS)/"$$(basename "$$1" .png)".webp' _
	@echo "Screenshots: $(abspath $(DOCS_SHOTS))"

# The simulator (`just sim`, sim.c): phone-ui in a window, in real time, with the host's
# libcurl and ALSA (not wrapped) instead of fake_curl.c and fake_alsa.c; the modem and
# Wi-Fi fakes as the tests'. Its own sandbox: the network, the display's sockets, sound,
# /var/lib kept in build/sim/var-lib, with the repository's demo files (books, photos, music)
# in each of its folders that is not there yet, and the plugin apps the host can run (GitHub, Email,
# Assistant, Trilium, Terminal on /bin/sh) in its apps/, icons rendered as `just app` does: the program
# linked as test_web. The monitors' EDIDs in displays/, for the phone's real size. ARGS are sim's
# (--scale 0.5, --open <app>).
SIM := $(abspath $(BUILD))/sim
SIM_APPS := $(SIM)/var-lib/apps
SIM_OBJS := $(filter-out $(BUILD)/fake_curl.o $(BUILD)/fake_alsa.o,$(UI_OBJS)) $(BUILD)/sim.o
$(BUILD)/sim.o: override CFLAGS += $(shell pkg-config --cflags sdl3)
$(BUILD)/sim/sim: $(SIM_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -Wl,--wrap=clock_gettime -Wl,--wrap=time $(WAKE_WRAPS) -Wl,--wrap=ioctl $(DRM_WRAPS:%=-Wl,--wrap=%) \
		$(EXPORTS) -o $@ $^ -Wl,--whole-archive $(LVGL)/usr/lib/liblvgl.a -Wl,--no-whole-archive \
		$(UI_LDLIBS) -lcurl $(shell pkg-config --libs sdl3)
$(SIM_APPS)/github/github.so: $(GITHUB_APP)/github.so
	@mkdir -p $(dir $@)
	cp $< $(GITHUB_APP)/app.json $(dir $@)
$(SIM_APPS)/assistant/assistant.so: $(ASSISTANT_APP)/assistant.so
	@mkdir -p $(dir $@)
	cp $< $(ASSISTANT_APP)/app.json $(dir $@)
$(SIM_APPS)/trilium/trilium.so: $(TRILIUM_APP)/trilium.so
	@mkdir -p $(dir $@)
	cp $< $(TRILIUM_APP)/app.json $(dir $@)
$(SIM_APPS)/email/email.so: $(EMAIL_APP)/email.so
	@mkdir -p $(dir $@)
	cp $< $(EMAIL_APP)/app.json $(dir $@)
$(SIM_APPS)/terminal/terminal.so: $(TERMINAL)/terminal.c $(TERMINAL_I18N) $(TERMINAL)/app.json \
		$(BUILD)/ui/abi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(BUILD)/ui -DUI_APP_CATALOG -DTERMINAL_SHELL='"/bin/sh"' -MF $(SIM)/terminal.d -fPIC -shared \
		-o $@ $(filter %.c,$^)
	cp $(TERMINAL)/app.json $(dir $@)
$(SIM_APPS)/%/icon.png: ../../../../apps/%/icon.svg
	@mkdir -p $(dir $@)
	resvg -w 112 -h 112 $< $@
SIM_APP_FILES := $(foreach a,github email terminal assistant trilium,$(SIM_APPS)/$(a)/$(a).so $(SIM_APPS)/$(a)/icon.png)
SIM_SANDBOX = bwrap --ro-bind / / --dev /dev --proc /proc --tmpfs /tmp \
	$(if $(wildcard /dev/snd),--dev-bind /dev/snd /dev/snd) \
	$(if $(wildcard /tmp/.X11-unix),--ro-bind /tmp/.X11-unix /tmp/.X11-unix) \
	--bind $(SIM)/var-lib /var/lib --bind $(SIM)/sys /sys --bind $(SIM) $(SIM) --chdir $(SIM) \
	--setenv __EGL_VENDOR_LIBRARY_FILENAMES "$(EGL_VENDOR)" --setenv GALLIUM_DRIVER llvmpipe --setenv TZ UTC0 \
	--setenv ASAN_OPTIONS detect_leaks=0 --die-with-parent --
sim-build: $(BUILD)/sim/sim $(SIM_APP_FILES)
sim: sim-build
	@mkdir -p $(SIM)/var-lib $(SIM)/sys
	@for d in books photos music; do [ -e $(SIM)/var-lib/$$d ] || cp -r $(DEMO)/$$d $(SIM)/var-lib/; done
	@rm -rf $(SIM)/displays && mkdir -p $(SIM)/displays && for c in /sys/class/drm/card*-*; do \
		cat $$c/edid > $(SIM)/displays/"$${c#*/card*-}" 2>/dev/null; done; \
		find $(SIM)/displays -mindepth 1 -empty -delete
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

.PHONY: run i18n-check coverage analyze clean docs-shots sim sim-build
endif
