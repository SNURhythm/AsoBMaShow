"""Regression contract for TextView's per-frame bgfx allocations."""

from pathlib import Path
import re
import unittest


class TextViewTransientBufferContractTests(unittest.TestCase):
    def test_text_submission_checks_transient_capacity_before_allocating(self):
        source = (Path(__file__).resolve().parents[1] / "src/view/TextView.cpp").read_text()
        submit = source[source.index("const auto submitText"):source.index("if (clip)")]

        capacity_check = re.search(
            r"getAvailTransientVertexBuffer\(\s*4,\s*"
            r"rendering::PosTexVertex::ms_decl\s*\)\s*<\s*4\s*\|\|\s*"
            r"bgfx::getAvailTransientIndexBuffer\(\s*6\s*\)\s*<\s*6",
            submit,
        )
        self.assertIsNotNone(
            capacity_check,
            "TextView must refuse an unavailable transient buffer before writing it",
        )
        self.assertLess(
            capacity_check.start(),
            submit.index("bgfx::allocTransientVertexBuffer"),
            "capacity must be checked before the transient allocation",
        )


if __name__ == "__main__":
    unittest.main()
