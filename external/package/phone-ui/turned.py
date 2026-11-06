#!/usr/bin/env python3
"""For gen-fonts.sh: a font (CFF, as Font Awesome's) of Font Awesome icons copied to other
codepoints, turned about the middle of their bounds, as wide as they were or as they
became (a quarter turn: as they became), centered in that. From the directory holding
fa-solid.otf, fa-regular.otf and fa-brands.otf:

    turned.py out.otf STYLE CODEPOINT DEGREES TARGET...

four arguments an icon: its style (solid, regular, brands), its codepoint in Font Awesome (hex),
degrees clockwise and its codepoint in out.otf (hex).
"""

import math
import sys

from fontTools.fontBuilder import FontBuilder
from fontTools.misc.transform import Identity
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont


def main():
    out, args = sys.argv[1], sys.argv[2:]
    fonts = {}
    order = [".notdef"]
    charstrings = {".notdef": T2CharStringPen(0, None).getCharString()}
    metrics = {".notdef": (0, 0)}
    cmap = {}
    for style, code, degrees, target in zip(args[0::4], args[1::4], args[2::4], args[3::4]):
        font = fonts.setdefault(style, TTFont(f"fa-{style}.otf"))
        glyphs = font.getGlyphSet()
        glyph = glyphs[font.getBestCmap()[int(code, 16)]]
        bounds = BoundsPen(glyphs)
        glyph.draw(bounds)
        x0, y0, x1, y1 = bounds.bounds
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        turn = Identity.translate(cx, cy).rotate(-math.radians(int(degrees))).translate(-cx, -cy)
        bounds = BoundsPen(glyphs)
        glyph.draw(TransformPen(bounds, turn))
        x0, y0, x1, y1 = bounds.bounds
        advance = max(glyph.width, math.ceil(x1 - x0))
        if int(degrees) % 180 == 90:
            advance = math.ceil(x1 - x0)  # a quarter turn: its old width is now its height
        dx = advance / 2 - (x0 + x1) / 2
        pen = T2CharStringPen(advance, None)
        glyph.draw(TransformPen(pen, Identity.translate(dx, 0).transform(turn)))
        name = f"u{target}"
        order.append(name)
        charstrings[name] = pen.getCharString()
        metrics[name] = (advance, round(x0 + dx))
        cmap[int(target, 16)] = name

    source = next(iter(fonts.values()))
    os2 = source["OS/2"]
    builder = FontBuilder(source["head"].unitsPerEm, isTTF=False)
    builder.setupGlyphOrder(order)
    builder.setupCharacterMap(cmap)
    builder.setupCFF("PhoneUiTurned", {"FullName": "PhoneUiTurned"}, charstrings, {})
    builder.setupHorizontalMetrics(metrics)
    builder.setupHorizontalHeader(ascent=source["hhea"].ascent, descent=source["hhea"].descent)
    builder.setupNameTable({"familyName": "PhoneUiTurned", "styleName": "Regular"})
    builder.setupOS2(sTypoAscender=os2.sTypoAscender, sTypoDescender=os2.sTypoDescender,
                     usWinAscent=os2.usWinAscent, usWinDescent=os2.usWinDescent)
    builder.setupPost()
    builder.save(out)


if __name__ == "__main__":
    main()
