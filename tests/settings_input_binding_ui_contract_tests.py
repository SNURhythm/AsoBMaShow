#!/usr/bin/env python3
"""Regression contracts for input-binding row actions."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class SettingsInputBindingUiContracts(unittest.TestCase):
    def test_each_saved_binding_row_offers_transactional_unbind(self) -> None:
        source = (ROOT / "src/scene/SettingsSceneInput.cpp").read_text(
            encoding="utf-8"
        )
        self.assertRegex(
            source,
            re.compile(
                r"for \(const auto &binding : visibleBindings\) \{"
                r"[\s\S]*?makeText\(\"Unbind\""
                r"[\s\S]*?setOnClickListener\("
                r"\[this, bindingId = binding\.id\]\(\) \{"
                r"[\s\S]*?inputCaptureController->cancel\(\);"
                r"[\s\S]*?inputCaptureAction\.reset\(\);"
                r"[\s\S]*?inputCaptureController->removeBinding\(bindingId\);"
                r"[\s\S]*?requestInputViewRebuild\(\);",
            ),
        )


if __name__ == "__main__":
    unittest.main()
