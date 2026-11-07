#!/usr/bin/env python3
"""For gen-fonts.sh: a font (CFF, as Font Awesome's) of icons ui_font_<size> cannot take from
Font Awesome's fonts as they are: Font Awesome icons copied to other codepoints, and ours,
drawn in SVG. Each is turned about the middle of its bounds, as wide as it was or as it
became (a quarter turn: as it became), centered in that. From the directory holding
fa-solid.otf, fa-regular.otf and fa-brands.otf:

    symbol-font.py out.otf STYLE SOURCE DEGREES TARGET...

four arguments an icon: its style (solid, regular, brands, or svg for ours), its codepoint in
Font Awesome (hex) or its SVG file, degrees clockwise and its codepoint in out.otf (hex).

An SVG is drawn as Font Awesome's are: a viewBox "0 0 WIDTH 512", 512 the font's em, its
bottom on the descender (-64).
"""

import math
import sys
import xml.etree.ElementTree as ET

from fontTools.fontBuilder import FontBuilder
from fontTools.misc.transform import Identity
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.pens.transformPen import TransformPen
from fontTools.svgLib.path import SVGPath
from fontTools.ttLib import TTFont


class SvgGlyph:
    """An SVG file as a glyph of the font: its width and draw(pen), y up from the baseline"""

    def __init__(self, path, ascent):
        x, y, self.width, height = (float(v) for v in ET.parse(path).getroot().get("viewBox").split())
        if (x, y, height) != (0, 0, 512):
            sys.exit(f"{path}: viewBox \"0 0 WIDTH 512\", as Font Awesome's")
        self.path = SVGPath(path, transform=(1, 0, 0, -1, 0, ascent))

    def draw(self, pen):
        self.path.draw(pen)


def main():
    out, args = sys.argv[1], sys.argv[2:]
    fonts = {"solid": TTFont("fa-solid.otf")}  # its metrics are the font's
    ascent = fonts["solid"]["OS/2"].sTypoAscender
    order = [".notdef"]
    charstrings = {".notdef": T2CharStringPen(0, None).getCharString()}
    metrics = {".notdef": (0, 0)}
    cmap = {}
    for style, source, degrees, target in zip(args[0::4], args[1::4], args[2::4], args[3::4]):
        if style == "svg":
            glyphs, glyph = None, SvgGlyph(source, ascent)
        else:
            font = fonts.setdefault(style, TTFont(f"fa-{style}.otf"))
            glyphs = font.getGlyphSet()
            glyph = glyphs[font.getBestCmap()[int(source, 16)]]
        bounds = BoundsPen(glyphs)
        glyph.draw(bounds)
        x0, y0, x1, y1 = bounds.bounds
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        turn = Identity.translate(cx, cy).rotate(-math.radians(int(degrees))).translate(-cx, -cy)
        bounds = BoundsPen(glyphs)
        glyph.draw(TransformPen(bounds, turn))
        x0, y0, x1, y1 = bounds.bounds
        advance = max(math.ceil(glyph.width), math.ceil(x1 - x0))
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

    source = fonts["solid"]
    os2 = source["OS/2"]
    builder = FontBuilder(source["head"].unitsPerEm, isTTF=False)
    builder.setupGlyphOrder(order)
    builder.setupCharacterMap(cmap)
    builder.setupCFF("PhoneUiSymbols", {"FullName": "PhoneUiSymbols"}, charstrings, {})
    builder.setupHorizontalMetrics(metrics)
    builder.setupHorizontalHeader(ascent=source["hhea"].ascent, descent=source["hhea"].descent)
    builder.setupNameTable({"familyName": "PhoneUiSymbols", "styleName": "Regular"})
    builder.setupOS2(sTypoAscender=os2.sTypoAscender, sTypoDescender=os2.sTypoDescender,
                     usWinAscent=os2.usWinAscent, usWinDescent=os2.usWinDescent)
    builder.setupPost()
    builder.save(out)


if __name__ == "__main__":
    main()
