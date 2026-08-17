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

    def test_layout_reset_clears_every_retained_gameplay_skin_view(self) -> None:
        source = (ROOT / "src/scene/SettingsSceneLayout.cpp").read_text(
            encoding="utf-8"
        )
        reset = re.search(
            r"void SettingsScene::resetViewState\(\) \{(?P<body>[\s\S]*?)\n\}",
            source,
        )
        self.assertIsNotNone(reset)
        body = reset.group("body")
        for pointer in (
            "gameplaySkinStatusText",
            "gameplaySkinUiMessageText",
            "gameplaySkinConfigurationDigestText",
            "gameplaySkinSafetyOverlayRoot",
            "gameplaySkinBusyOverlayRoot",
            "gameplaySkinBusyOverlayStatusText",
            "gameplaySkinBusyOverlayCancelButton",
        ):
            self.assertIn(
                f"{pointer} = nullptr;",
                body,
                f"{pointer} must not outlive a deleted settings view tree",
            )

    def test_busy_rebuild_reenables_ordinary_controls_when_idle(self) -> None:
        source = (ROOT / "src/scene/SettingsSceneSkins.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "gameplaySkinControlsBuiltDisabled = !ordinaryActionsEnabled;",
            source,
            "the tab must remember when a busy rebuild disables controls",
        )
        self.assertRegex(
            source,
            r"gameplaySkinControlsBuiltDisabled\s*&&\s*"
            r"skin::gameplaySkinSettingsActionAvailability\(snapshot\)"
            r"\.ordinaryActions",
            "returning to an idle snapshot must request one control rebuild",
        )

    def test_configuration_actions_load_the_latest_snapshot(self) -> None:
        source = (ROOT / "src/scene/SettingsSceneSkins.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "gameplaySkinOffsetForEntry(entry, name)",
            source,
            "offset edits must merge into the latest settings snapshot",
        )
        self.assertIn(
            "gameplaySkinViewportForEntry(entry)",
            source,
            "viewport edits must merge into the latest settings snapshot",
        )


if __name__ == "__main__":
    unittest.main()
