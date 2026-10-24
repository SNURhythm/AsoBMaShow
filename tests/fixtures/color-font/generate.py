"""Regenerate the original, CC0 color-glyph fixture (requires fonttools 4.67.0).

Tests consume the checked-in font; fonttools is not a test dependency.
"""
from pathlib import Path

from fontTools.colorLib.builder import buildCOLR, buildCPAL
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable


def rectangle(left, right):
    pen = TTGlyphPen(None)
    pen.moveTo((left, 0))
    pen.lineTo((right, 0))
    pen.lineTo((right, 700))
    pen.lineTo((left, 700))
    pen.closePath()
    return pen.glyph()


builder = FontBuilder(1000, isTTF=True)
names = [".notdef", "emoji", "red", "green", "wide", "wideRed", "wideGreen"]
builder.setupGlyphOrder(names)
builder.setupCharacterMap({0x1F642: "emoji"})
builder.setupGlyf({
    ".notdef": TTGlyphPen(None).glyph(),
    "emoji": rectangle(100, 600),
    "red": rectangle(100, 350),
    "green": rectangle(350, 600),
    "wide": rectangle(100, 1100),
    "wideRed": rectangle(100, 600),
    "wideGreen": rectangle(600, 1100),
})
builder.setupHorizontalMetrics({
    ".notdef": (700, 0), "emoji": (700, 100),
    "red": (700, 100), "green": (700, 350),
    "wide": (1400, 100), "wideRed": (1400, 100), "wideGreen": (1400, 600),
})
builder.setupHorizontalHeader(ascent=800, descent=-200)
builder.setupOS2(sTypoAscender=800, sTypoDescender=-200,
                 usWinAscent=800, usWinDescent=200)
builder.setupNameTable({
    "familyName": "AsoBMaShow Color Test",
    "styleName": "Regular",
    "uniqueFontIdentifier": "AsoBMaShowColorTest-Regular",
    "fullName": "AsoBMaShow Color Test Regular",
    "psName": "AsoBMaShowColorTest-Regular",
    "version": "Version 1.0",
    "copyright": "Original test fixture, dedicated to the public domain (CC0).",
})
builder.setupPost()
builder.font["COLR"] = buildCOLR({
    "emoji": [("red", 0), ("green", 1)],
    "wide": [("wideRed", 0), ("wideGreen", 1)],
}, version=0)
builder.font["CPAL"] = buildCPAL([[(1, 0, 0, 1), (0, 1, 0, 1)]])
# A deliberately wider emoji variation exposes dropped selectors in UI runs.
variation = CmapSubtable.newSubtable(14)
variation.platformID, variation.platEncID, variation.language = 0, 5, 0
variation.cmap = {}
variation.uvsDict = {0xFE0F: [(0x1F642, "wide")]}
builder.font["cmap"].tables.append(variation)
builder.font["head"].created = builder.font["head"].modified = 3406620153
builder.font.recalcTimestamp = False
builder.save(Path(__file__).with_name("color-emoji.ttf"))
