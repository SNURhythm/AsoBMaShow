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

    def test_settings_card_keeps_controller_controls_compact(self) -> None:
        source = (ROOT / "src/scene/SettingsSceneInput.cpp").read_text(
            encoding="utf-8"
        )
        self.assertNotIn(
            "Optional 5-key and 7-key mobile controls. It stays above the selected gameplay skin.",
            source,
        )
        self.assertNotIn("Spin the platter. Each 3° turn", source)
        self.assertNotIn("Flick Mode: swipe the scratch vertically", source)

    def test_settings_card_offers_the_one_or_two_player_side(self) -> None:
        source = (ROOT / "src/scene/SettingsSceneInput.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn('"Player: 2P"', source)
        self.assertIn('"Player: 1P"', source)
        self.assertIn("VirtualControllerPlayer::Player2", source)

    def test_hispeed_auto_adjust_button_measures_its_longest_label(self) -> None:
        source = (ROOT / "src/scene/SettingsSceneLayout.cpp").read_text(
            encoding="utf-8"
        )
        self.assertRegex(
            source,
            r'hispeedAutoAdjustModeText\s*=\s*makeText\(\s*'
            r'"Hi-Speed Auto Adjust: Off"[\s\S]{0,300}'
            r'hispeedAutoAdjustButtonWidth\s*=\s*std::max\('
            r'metrics\.actionButtonWidth,\s*'
            r'hispeedAutoAdjustModeText->textureWidth\(\)\s*\+\s*'
            r'hispeedAutoAdjustHorizontalPadding\)'
            r'[\s\S]{0,220}makeControlButton\(\s*'
            r'hispeedAutoAdjustButtonWidth',
        )


if __name__ == "__main__":
    unittest.main()
