#!/usr/bin/env bash
#
# Generates term_font.c: DejaVu Sans Mono at 20 px (12 px per cell, 25 px lines) for
# ASCII, Latin-1, box drawing and block elements; other characters fall back to
# phone-ui's ui_font_20 (proportional: they do not line up).
#
# Run in `nix develop` (lv_font_conv): apps/terminal/gen-font.sh
#

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
fonts="$(nix build --inputs-from "$here/../.." nixpkgs#dejavu_fonts --no-link --print-out-paths)"

# In a scratch directory, with relative paths: the options are recorded in the output
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
ln -s "$fonts/share/fonts/truetype/DejaVuSansMono.ttf" "$tmp/"
(cd "$tmp" && lv_font_conv --no-compress --no-prefilter --bpp 4 --size 20 --format lvgl \
	--font DejaVuSansMono.ttf -r 0x20-0x7E,0xA0-0xFF,0x2500-0x259F \
	--lv-include lvgl.h --lv-font-name term_font -o term_font.c)

sed -i -e 's/^\( *\.fallback = \)NULL,/\1\&ui_font_20,/' \
       -e "s|^/\*Initialize a public general font descriptor\*/|extern const lv_font_t ui_font_20; /* phone-ui's */\n\n&|" \
       "$tmp/term_font.c"
grep -q "fallback = &ui_font_20" "$tmp/term_font.c" || { echo "term_font.c: no fallback" >&2; exit 1; }
mv "$tmp/term_font.c" "$here/term_font.c"
