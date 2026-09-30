#!/usr/bin/env python3
"""Regression contracts for the virtual-controller settings lifecycle."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class SettingsVirtualControllerUiContracts(unittest.TestCase):
    def test_rebuild_clears_the_destroyed_editor_overlay_pointer(self) -> None:
        source = (ROOT / "src/scene/SettingsSceneLayout.cpp").read_text(
            encoding="utf-8"
        )
        reset = re.search(
            r"void SettingsScene::resetViewState\(\) \{(?P<body>[\s\S]*?)\n\}\n\n"
            r"void SettingsScene::ensureLayoutUpToDate",
            source,
        )
        self.assertIsNotNone(reset)
        self.assertIn(
            "inputVirtualControllerEditorOverlayRoot = nullptr;",
            reset.group("body"),
        )

if __name__ == "__main__":
    unittest.main()
