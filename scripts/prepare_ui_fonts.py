#!/usr/bin/env python3
"""Prepare full static UI fonts; requires fonttools==4.61.1.

Download the two pinned upstream files documented in docs/ui-fonts.md first.
No glyphs are removed. Restore one legacy Unicode mapping to an existing glyph.
"""
import argparse
import hashlib
from pathlib import Path

from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
FONTS = (
    ("NotoSansCJKjp-Regular.otf", "notosanscjkjp.ttf",
     "68a3fc98800b2a27b371f2fb79991daf3633bd89309d4ffaa6946fd587f375b5"),
    ("NotoSansCJKjp-Bold.otf", "notosanscjkjp-bold.otf",
     "e53dcb0dcb2922e45d01aae1ebd2f382bb81d4229b18b6b883bd170678af1f76"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "assets/fonts")
    args = parser.parse_args()
    for source_name, _, checksum in FONTS:
        source = args.source_dir / source_name
        if hashlib.sha256(source.read_bytes()).hexdigest() != checksum:
            raise ValueError(f"Unexpected upstream font: {source}")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for source_name, output_name, _ in FONTS:
        font = TTFont(args.source_dir / source_name, recalcTimestamp=False)
        # Noto omits U+2252 for its wider ecosystem's fallback policy. The actual
        # Source Han glyph remains CID+858; retain our legacy standalone coverage.
        assert "cid00858" in font.getGlyphOrder()
        for table in font["cmap"].tables:
            if table.isUnicode() and table.format in (4, 12):
                table.cmap[0x2252] = "cid00858"
        assert font["maxp"].numGlyphs == 65535 and "fvar" not in font
        output = args.output_dir / output_name
        font.save(output)
        print(f"{output.name}: {output.stat().st_size} bytes, "
              f"SHA-256 {hashlib.sha256(output.read_bytes()).hexdigest()}")
        font.close()


if __name__ == "__main__":
    main()
