#!/usr/bin/env python3
"""Regression contract for replay-video UI batch activation."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ReplayVideoUiBatchContracts(unittest.TestCase):
    def test_every_gameplay_presentation_render_is_scoped(self) -> None:
        source = (ROOT / "src/ReplayVideoExporter.cpp").read_text(encoding="utf-8")
        gameplay_calls = list(
            re.finditer(
                r"(?:preparedGameplay\.presentation\s*->|presentation\s*\.)renderFrame\s*\(",
                source,
            )
        )
        self.assertEqual(
            2,
            len(gameplay_calls),
            "the exporter has one regular and one course gameplay render path",
        )
        for call in gameplay_calls:
            scope_start = source.rfind("RenderContext::UiBatchScope", 0, call.start())
            self.assertNotEqual(-1, scope_start)
            self.assertGreater(
                scope_start,
                source.rfind("[&]() {", 0, call.start()),
                "each gameplay render must activate UiBatchRenderer inside its frame callback",
            )


if __name__ == "__main__":
    unittest.main()
