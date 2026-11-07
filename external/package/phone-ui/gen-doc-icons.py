#!/usr/bin/env python3
"""The icons phone-ui draws as glyphs, for the user documentation: writes
.claude/skills/user-docs/icons.md, a table of each LV_SYMBOL_* and UI_SYMBOL_* used in src/
with its Zensical shortcode (:fontawesome-solid-trash-can:), so a page shows the icon the
screen shows.

LVGL's symbols are Font Awesome glyphs: their codepoints, looked up in Font Awesome 7 Free
(the dev shell's FONTAWESOME), give the names; ours (symbols.txt) have them already.
Run in the dev shell after changing symbols.txt or the symbols src/ uses:

    python3 external/package/phone-ui/gen-doc-icons.py

LVGL's header comes from $LVGL (the tests shell's lvgl-host), else `nix build .#lvgl-host`.
"""

import os
import re
import subprocess
import sys
from collections import defaultdict

from fontTools.ttLib import TTFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "../../.."))
SRC = os.path.join(HERE, "src")
OUT = os.path.join(ROOT, ".claude/skills/user-docs/icons.md")


def lvgl_header():
    lvgl = os.environ.get("LVGL") or subprocess.check_output(
        ["nix", "build", "--no-link", "--print-out-paths", f"{ROOT}#lvgl-host"], text=True).strip()
    return os.path.join(lvgl, "usr/include/lvgl/include/lvgl/font/lv_symbol_def.h")


def font_names(path):
    """codepoint -> glyph name (Font Awesome's icon name)"""
    return TTFont(path).getBestCmap()


def main():
    fa = os.environ.get("FONTAWESOME")
    if not fa:
        sys.exit("FONTAWESOME is not set: run it in the dev shell (nix develop)")
    solid = font_names(os.path.join(fa, "Font Awesome 7 Free-Solid-900.otf"))
    brands = font_names(os.path.join(fa, "Font Awesome 7 Brands-Regular-400.otf"))

    # Where src/ uses each symbol
    used = defaultdict(set)
    for dirpath, _, files in os.walk(SRC):
        for f in files:
            if not f.endswith(".c") or f.startswith("ui_font_"):
                continue
            path = os.path.join(dirpath, f)
            for sym in set(re.findall(r"\b(?:LV|UI)_SYMBOL_[A-Z0-9_]+\b", open(path, encoding="utf-8").read())):
                used[sym].add(os.path.relpath(path, SRC).replace(os.sep, "/"))

    rows = []
    hdr = open(lvgl_header(), encoding="utf-8").read()
    # The codepoint from the UTF-8 string, not the comment after it (FILE's says 0xF158)
    for name, utf8 in re.findall(r'#define\s+(LV_SYMBOL_\w+)\s+"((?:\\x[0-9A-Fa-f]{2})+)"', hdr):
        if name not in used:
            continue
        cp = ord(bytes.fromhex(utf8.replace("\\x", "")).decode("utf-8"))
        if cp in solid:
            code = f":fontawesome-solid-{solid[cp]}:"
        elif cp in brands:
            code = f":fontawesome-brands-{brands[cp]}:"
        else:
            code = "(none in Font Awesome 7 Free: say it in words)"
        rows.append((name, code, ", ".join(sorted(used[name]))))

    for line in open(os.path.join(HERE, "symbols.txt"), encoding="utf-8"):
        m = re.match(r"([a-z0-9-]+)\s+svg\s*(?:#\s*(.*))?$", line.strip())
        if m:  # ours, not Font Awesome's
            name = "UI_SYMBOL_" + m.group(1).upper().replace("-", "_")
            code = f"(none: phone-ui's own, symbols/{m.group(1)}.svg: say it in words)"
            rows.append((name, code, m.group(2) or ", ".join(sorted(used[name]))))
            continue
        m = re.match(r"([a-z0-9-]+)\s+[0-9a-f]+(?:\s+(solid|regular|brands))?(?:\s+(\d+))?\s*(?:#\s*(.*))?$",
                     line.strip())
        if not m:
            continue
        icon, style, turn, comment = m.group(1), m.group(2) or "solid", m.group(3), m.group(4) or ""
        name = "UI_SYMBOL_" + icon.upper().replace("-", "_") + ("_REGULAR" if style == "regular" else "")
        code = f":fontawesome-{style}-{icon}:"
        if turn:
            name += f"_{turn}"
            code = f"{code}{{ style=\"transform: rotate({turn}deg)\" }}"
        rows.append((name, code, comment or ", ".join(sorted(used[name]))))

    with open(OUT, "w", encoding="utf-8", newline="\n") as out:
        out.write("<!-- Written by external/package/phone-ui/gen-doc-icons.py: do not edit -->\n")
        out.write("# The UI's icons as Zensical shortcodes\n\n")
        out.write("Each glyph phone-ui draws (`LV_SYMBOL_*` from LVGL, `UI_SYMBOL_*` from\n")
        out.write("`symbols.txt`), the shortcode that shows the same Font Awesome icon in a page, and\n")
        out.write("where phone-ui uses it. LVGL's glyphs are an older Font Awesome's: the same icon,\n")
        out.write("drawn a little differently.\n\n")
        out.write("| Symbol | Shortcode | Used in |\n|---|---|---|\n")
        for name, code, where in rows:
            out.write(f"| `{name}` | `{code}` | {where} |\n")
    print(f"{os.path.relpath(OUT, ROOT)}: {len(rows)} icons")


if __name__ == "__main__":
    main()
