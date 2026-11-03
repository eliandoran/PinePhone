#!/usr/bin/env bash
#
# Generates src/ui_font_<size>.c: Inter Medium ($INTER, set by the dev shell) for
# ASCII, Latin-1 and Latin Extended-A, Romanian ș ț, typographic quotes, dashes and
# ellipsis, € ™ √ π ∆ for the keyboard's symbols; DejaVu Sans (from LVGL's sources)
# for ✓ ★ ☆. Falls back to the color emoji (ui_emoji_<size>, emoji.c), then
# lv_font_montserrat_<size>, which only adds LVGL's symbols (LV_SYMBOL_*). Line
# height and baseline are Montserrat's, which the layouts were made for.
#
# Also src/ui_font_clock.c, the home screen's clock: only digits and colon, from
# Inter Display Light, no fallback.
#
# Run in `nix develop` (lv_font_conv) after Buildroot extracted LVGL:
#   external/package/phone-ui/gen-fonts.sh [lvgl source dir]
#

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
lvgl="${1:-$(ls -d "$here"/../../../output/build/lvgl-[0-9]* | tail -n 1)}"

# From a copy, with relative paths: the store is read-only and the options recorded
# in the output stay free of paths.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cp "$INTER/Inter-Medium.ttf" "$INTER/Inter-Display-Light.ttf" \
   "$lvgl/scripts/generators/built_in_font/DejaVuSans.ttf" "$tmp"

for size in 16 20 24 26 32 48; do
	base="$lvgl/src/font/lv_font_montserrat_$size.c"
	line_height=$(sed -n 's/^ *\.line_height = \([0-9]*\),.*/\1/p' "$base")
	base_line=$(sed -n 's/^ *\.base_line = \([0-9]*\),.*/\1/p' "$base")
	out="$here/src/ui_font_$size.c"

	(cd "$tmp" && lv_font_conv --no-compress --no-prefilter --bpp 4 --size "$size" --format lvgl \
		--font Inter-Medium.ttf -r 0x20-0x7E,0xA0-0x17F,0x218-0x21B,0x2010,0x2012-0x2015,0x2018-0x201A,0x201C-0x201E \
		-r 0x2020-0x2022,0x2026,0x2030,0x2032-0x2033,0x2039-0x203A \
		-r 0x20AC,0x2122,0x221A,0x3C0,0x2206 \
		--font DejaVuSans.ttf -r 0x2605-0x2606,0x2713 \
		--lv-include lvgl.h --lv-font-name "ui_font_$size" -o "ui_font_$size.c")
	mv "$tmp/ui_font_$size.c" "$out"

	sed -i -e "s/^\( *\.line_height = \)[0-9]*,/\1$line_height,/" \
	       -e "s/^\( *\.base_line = \)[0-9]*,/\1$base_line,/" \
	       -e "s/^\( *\.fallback = \)NULL,/\1\&ui_emoji_$size,/" \
	       -e "s|^/\*Initialize a public general font descriptor\*/|extern const lv_font_t ui_emoji_$size; /* emoji.c, falls back to Montserrat */\n\n&|" "$out"
	grep -q "fallback = &ui_emoji_$size" "$out" || { echo "$out: no fallback" >&2; exit 1; }
done

# The clock: big, so only what "%H:%M" prints.
(cd "$tmp" && lv_font_conv --no-compress --no-prefilter --bpp 4 --size 160 --format lvgl \
	--font Inter-Display-Light.ttf -r 0x30-0x3A \
	--lv-include lvgl.h --lv-font-name ui_font_clock -o ui_font_clock.c)
mv "$tmp/ui_font_clock.c" "$here/src/ui_font_clock.c"
