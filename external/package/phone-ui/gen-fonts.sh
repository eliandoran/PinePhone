#!/usr/bin/env bash
#
# Generates src/ui_font_<size>.c: the characters LVGL's built-in Montserrat fonts
# lack (Latin-1 and Latin Extended-A, Romanian ș ț, typographic quotes, dashes and
# ellipsis; € ™ √ π ∆ ✓ for the keyboard's symbols, ★ ☆ for favorites), falling
# back to the color emoji (ui_emoji_<size>, emoji.c), then lv_font_montserrat_<size>
# for everything else. Same font (Montserrat Medium, DejaVu Sans for ✓ ★ ☆, both
# from LVGL's sources), with Montserrat's line height and baseline so that text
# does not move.
#
# Run in `nix develop` (lv_font_conv) after Buildroot extracted LVGL:
#   external/package/phone-ui/gen-fonts.sh [lvgl source dir]
#

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
lvgl="${1:-$(ls -d "$here"/../../../output/build/lvgl-[0-9]* | tail -n 1)}"
ttf="$lvgl/scripts/generators/built_in_font"

for size in 16 20 24 26 32 48; do
	base="$lvgl/src/font/lv_font_montserrat_$size.c"
	line_height=$(sed -n 's/^ *\.line_height = \([0-9]*\),.*/\1/p' "$base")
	base_line=$(sed -n 's/^ *\.base_line = \([0-9]*\),.*/\1/p' "$base")
	out="$here/src/ui_font_$size.c"

	# From the fonts' directory, with relative paths: the options are recorded in the output
	(cd "$ttf" && lv_font_conv --no-compress --no-prefilter --bpp 4 --size "$size" --format lvgl \
		--font Montserrat-Medium.ttf -r 0xA0-0x17F,0x218-0x21B,0x2010,0x2012-0x2015,0x2018-0x201A,0x201C-0x201E \
		-r 0x2020-0x2022,0x2026,0x2030,0x2032-0x2033,0x2039-0x203A \
		-r 0x20AC,0x2122,0x221A,0x3C0,0x2206 \
		--font DejaVuSans.ttf -r 0x2605-0x2606,0x2713 \
		--lv-include lvgl.h --lv-font-name "ui_font_$size" -o "ui_font_$size.c")
	mv "$ttf/ui_font_$size.c" "$out"

	sed -i -e "s/^\( *\.line_height = \)[0-9]*,/\1$line_height,/" \
	       -e "s/^\( *\.base_line = \)[0-9]*,/\1$base_line,/" \
	       -e "s/^\( *\.fallback = \)NULL,/\1\&ui_emoji_$size,/" \
	       -e "s|^/\*Initialize a public general font descriptor\*/|extern const lv_font_t ui_emoji_$size; /* emoji.c, falls back to Montserrat */\n\n&|" "$out"
	grep -q "fallback = &ui_emoji_$size" "$out" || { echo "$out: no fallback" >&2; exit 1; }
done
