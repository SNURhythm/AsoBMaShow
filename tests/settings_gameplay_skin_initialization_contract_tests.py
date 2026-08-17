#!/usr/bin/env python3
"""Regression contract for retained Gameplay Skins tab initialization."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class SettingsGameplaySkinInitializationContracts(unittest.TestCase):
    def test_retained_gameplay_skins_tab_refreshes_before_first_layout(self) -> None:
        source = (ROOT / "src/scene/SettingsScene.cpp").read_text(encoding="utf-8")
        init = re.search(
            r"void SettingsScene::init\(\) \{(?P<body>[\s\S]*?)\n\}", source
        )
        self.assertIsNotNone(init)
        body = init.group("body")
        self.assertIn(
            "updateGameplaySkinSettingsController();",
            body,
            "The retained Gameplay Skins tab must refresh its controller "
            "before its initial layout.",
        )
        refresh = body.index("updateGameplaySkinSettingsController();")
        first_layout = body.index("ensureLayoutUpToDate();")
        self.assertLess(
            refresh,
            first_layout,
            "The retained Gameplay Skins tab must build from a refreshed "
            "controller snapshot on its first frame.",
        )


if __name__ == "__main__":
    unittest.main()
