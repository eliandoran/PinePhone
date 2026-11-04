# phone-ui's unit tests, on the host: `just test [filter]`, which builds LVGL for the
# host (lvgl-host in the flake) and runs `make LVGL=<it>` here. Built with ASan and
# UBSan. One program per test file, build/test_<name> [filter] runs one.
#
# `make coverage` (`just coverage`): the same tests built with --coverage in
# build/coverage (-Og, for exact lines) and run, then gcovr's report of ../src: per file
# here, line by line in build/coverage/html/index.html, and the sources no test reaches.

ifeq ($(LVGL)$(filter clean,$(MAKECMDGOALS)),)
$(error LVGL is not set: run the tests with `just test`)
endif
SRC := ../src
BUILD := build

TESTS := json data calculator

# What each test links besides itself and stubs.c (sources of ../src)
json_SRCS := json.c
data_SRCS := data.c json.c
calculator_SRCS := ui_font_48.c ui_font_32.c ui_font_26.c

SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=undefined
override CFLAGS += -O1 -g -Wall -std=gnu11 -fno-omit-frame-pointer $(SANITIZE) -MMD -MP \
	-DLV_CONF_INCLUDE_SIMPLE -I$(LVGL)/usr/include/lvgl -I$(SRC) \
	-DCONTACTS='"$(abspath $(BUILD))/contacts"'
LDFLAGS := $(SANITIZE)
LDLIBS := $(LVGL)/usr/lib/liblvgl.a -lm

ifdef COVERAGE
# -Og, not -O0: nix's compiler wrapper sets _FORTIFY_SOURCE, which warns without optimization
override CFLAGS += --coverage -Og -DTEST_COVERAGE
LDFLAGS += --coverage
endif
# Generated sources (fonts, images), not in the report
GENERATED := ui_font_.*|icons|emoji_list|splash_image

# The fonts' emoji fallbacks (emoji.c, with all of its images): LVGL's Montserrat
FONTS := 48 32 26
$(foreach s,$(FONTS),$(eval $(BUILD)/src/ui_font_$(s).o: override CFLAGS += -Dui_emoji_$(s)=lv_font_montserrat_$(s)))

run: $(TESTS:%=$(BUILD)/test_%)
	@failed=0; for t in $^; do ./$$t $(FILTER) || failed=1; done; exit $$failed

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

$(BUILD)/src/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d $(BUILD)/src/*.d)

.PHONY: run coverage clean
