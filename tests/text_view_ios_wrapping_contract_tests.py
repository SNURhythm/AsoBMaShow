"""Regression contract for the iOS wrapped TextView raster path."""

from pathlib import Path
import unittest


class TextViewIosWrappingContractTests(unittest.TestCase):
    def test_ios_wrapped_primary_text_uses_the_per_line_compositor(self):
        source = (Path(__file__).resolve().parents[1] / "src/view/TextView.cpp").read_text()
        branch_start = source.index(
            "if (usePrimaryFont && wrapEnabled && rasterWrapWidth > 0)"
        )
        branch_end = source.index("} else if (usePrimaryFont)", branch_start)
        branch = source[branch_start:branch_end]

        self.assertIn("#if TARGET_OS_IOS || TARGET_OS_SIMULATOR", branch)
        self.assertIn("renderFallbackTextSurface(rasterWrapWidth", branch)
        self.assertIn("#else", branch)
        self.assertIn("TTF_RenderUTF8_Blended_Wrapped", branch)


if __name__ == "__main__":
    unittest.main()
