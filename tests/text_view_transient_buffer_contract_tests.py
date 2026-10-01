"""Regression contract for TextView's dynamic-batched submission path."""

from pathlib import Path
import unittest


class TextViewTransientBufferContractTests(unittest.TestCase):
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
