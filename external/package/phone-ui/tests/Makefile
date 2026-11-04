# phone-ui's tests, on the host: `just test [filter]`, which runs make here in the
# flake's `tests` shell (LVGL: lvgl-host, EGL_VENDOR: Mesa's, the libraries). Built with
# ASan and UBSan. One program per test file, run in a sandbox (bubblewrap: the host
# read-only, /var/lib in build/var-lib, no devices, the hardware's sysfs empty);
# build/test_<name> [filter] runs one, in `$(SANDBOX)`.
#
# Unit tests (TESTS) link the sources they list and stubs.c. UI tests (UI_TESTS) link
# all of phone-ui but main.c, and ui_test.c that starts it (ui_test.h).
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

TESTS := json data calculator
UI_TESTS := notes

# What each unit test links besides itself and stubs.c (sources of ../src)
json_SRCS := json.c
data_SRCS := data.c json.c
calculator_SRCS := ui_font_48.c ui_font_32.c ui_font_26.c

SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=undefined
FLAGS := -O1 -g -Wall -fno-omit-frame-pointer $(SANITIZE) -MMD -MP \
	-DLV_CONF_INCLUDE_SIMPLE -I$(LVGL)/usr/include/lvgl -I$(SRC)
override CFLAGS += $(FLAGS)
override CXXFLAGS += $(FLAGS) -std=c++17
LDFLAGS := $(SANITIZE)
LDLIBS := $(LVGL)/usr/lib/liblvgl.a -lm
# As phone-ui.mk links it
UI_LDLIBS := $(LVGL)/usr/lib/liblvgl.a -lcurl -lpng -ljpeg -ldrm -llitehtml -lgumbo -lmpg123 \
	-lasound -lgbm -lGLESv2 -lEGL -lstdc++ -lpthread -lm

ifdef COVERAGE
# -Og, not -O0: nix's compiler wrapper sets _FORTIFY_SOURCE, which warns without optimization
override CFLAGS += --coverage -Og -DTEST_COVERAGE
override CXXFLAGS += --coverage -Og
LDFLAGS += --coverage
endif
# Generated sources (fonts, images), not in the report
GENERATED := ui_font_.*|icons|emoji_list|splash_image

# Unit tests: contacts in the build, the fonts' emoji fallbacks (emoji.c, with all of
# its images) LVGL's Montserrat
$(BUILD)/src/data.o $(BUILD)/test_data.o: override CFLAGS += -DCONTACTS='"$(abspath $(BUILD))/contacts"'
FONTS := 48 32 26
$(foreach s,$(FONTS),$(eval $(BUILD)/src/ui_font_$(s).o: override CFLAGS += -Dui_emoji_$(s)=lv_font_montserrat_$(s)))

# UI tests: phone-ui as in the image
UI_SRCS := $(filter-out main.c,$(patsubst $(SRC)/%,%,$(wildcard $(SRC)/*.c $(SRC)/*/*.c)))
UI_CXX_SRCS := $(patsubst $(SRC)/%,%,$(wildcard $(SRC)/*/*.cpp))
UI_OBJS := $(UI_SRCS:%.c=$(BUILD)/ui/%.o) $(UI_CXX_SRCS:%.cpp=$(BUILD)/ui/%.o) $(BUILD)/ui_test.o
$(BUILD)/ui/%.o: override CFLAGS += $(shell pkg-config --cflags libdrm) -I$(BUILD)/ui
$(BUILD)/ui/external.o: $(BUILD)/ui/abi.h

# The plugins' ABI, as phone-ui.mk makes it
$(BUILD)/ui/abi.h: $(SRC)/ui.h $(SRC)/json.h
	@mkdir -p $(dir $@)
	{ cat $^; find $(LVGL)/usr/include/lvgl -name '*.h' | LC_ALL=C sort | xargs cat; } \
		| sha256sum | cut -c1-16 | sed 's/.*/#define UI_ABI "&"/' > $@

# The sandbox. Only existing directories can be masked (the host's sysfs is read-only).
VAR_LIB := $(abspath $(BUILD))/var-lib
HARDWARE := /sys/class/backlight /sys/class/leds /sys/class/power_supply /sys/class/thermal \
	/sys/bus/iio/devices /sys/devices/system/cpu/cpufreq
SANDBOX = bwrap --ro-bind / / --dev /dev --proc /proc --tmpfs /tmp \
	--bind $(VAR_LIB) /var/lib --bind $(abspath $(BUILD)) $(abspath $(BUILD)) \
	$(foreach d,$(HARDWARE),$(if $(wildcard $(d)),--tmpfs $(d))) \
	--setenv __EGL_VENDOR_LIBRARY_FILENAMES "$(EGL_VENDOR)" --setenv MESA_SHADER_CACHE_DISABLE true \
	--die-with-parent --

run: $(TESTS:%=$(BUILD)/test_%) $(UI_TESTS:%=$(BUILD)/test_%)
	@mkdir -p $(VAR_LIB) && touch $(VAR_LIB)/.phone-ui-tests
	@failed=0; for t in $^; do $(SANDBOX) ./$$t $(FILTER) || failed=1; done; exit $$failed

# The report even when tests fail (then fails too); counts from earlier runs dropped
COV := $(BUILD)/coverage
coverage:
	@mkdir -p $(COV) && find $(COV) -name '*.gcda' -delete
	@$(MAKE) --no-print-directory run BUILD=$(COV) COVERAGE=1; status=$$?; \
	mkdir -p $(COV)/html && \
	gcovr --root $(SRC) --object-directory $(COV) --exclude '.*/($(GENERATED))\.c' \
		--txt --print-summary --html-details $(COV)/html/index.html \
		--json-summary $(COV)/summary.json $(COV) && \
	jq -r '.files[].filename' $(COV)/summary.json | sort > $(COV)/tested && \
	(cd $(SRC) && find . -name '*.c' | sed 's|^\./||' | grep -Ev '^($(GENERATED))\.c$$' | sort) \
		| comm -23 - $(COV)/tested | tr '\n' ' ' | sed 's/^./No tests: &/;s/ $$/\n/' && \
	echo "Line by line: $(abspath $(COV))/html/index.html" && exit $$status

define test_rule
$(BUILD)/test_$(1): $(BUILD)/test_$(1).o $(BUILD)/stubs.o $($(1)_SRCS:%.c=$(BUILD)/src/%.o)
	$$(CC) $$(LDFLAGS) -o $$@ $$^ $$(LDLIBS)
endef
$(foreach t,$(TESTS),$(eval $(call test_rule,$(t))))

define ui_test_rule
$(BUILD)/test_$(1): $(BUILD)/test_$(1).o $(UI_OBJS)
	$$(CXX) $$(LDFLAGS) -o $$@ $$^ $$(UI_LDLIBS)
endef
$(foreach t,$(UI_TESTS),$(eval $(call ui_test_rule,$(t))))

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

.PHONY: run coverage clean
