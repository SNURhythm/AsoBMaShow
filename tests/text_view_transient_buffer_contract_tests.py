"""Regression contract for TextView's per-frame bgfx allocations."""

from pathlib import Path
import re
import unittest


class TextViewTransientBufferContractTests(unittest.TestCase):
    def test_every_text_view_executable_links_the_sdl_ttf_lifetime_runtime(self):
        cmake = (Path(__file__).resolve().parents[1] / "CMakeLists.txt").read_text()
        executable_blocks = re.findall(
            r"add_executable\(\s*([A-Za-z0-9_]+)(.*?)^\s*\)",
            cmake,
            re.DOTALL | re.MULTILINE,
        )
        text_view_targets = {
            target: sources
            for target, sources in executable_blocks
            if "src/view/TextView.cpp" in sources
        }
        self.assertTrue(text_view_targets)
        missing_runtime = [
            target
            for target, sources in text_view_targets.items()
            if "src/view/SdlTtfRuntime.cpp" not in sources
        ]
        self.assertEqual(missing_runtime, [])

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
