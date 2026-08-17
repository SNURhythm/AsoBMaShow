"""Regression contract for TextView's dynamic-batched submission path."""

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

    def test_text_submission_uses_context_batcher_without_transient_buffers(self):
        source = (
            Path(__file__).resolve().parents[1] / "src/view/TextView.cpp"
        ).read_text()
        submit = source[source.index("const auto submitText"):source.index("if (clip)")]

        self.assertIn(
            "context.appendUiTextured",
            submit,
            "TextView must append its quad to the whole-tree UI batcher",
        )
        self.assertNotRegex(
            submit,
            r"(?:allocTransient|getAvailTransient)(?:Vertex|Index)Buffer",
            "TextView must not consume bgfx transient buffers per draw",
        )


if __name__ == "__main__":
    unittest.main()
