#!/usr/bin/env python3
"""Regression contract for replay-video UI batch activation."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ReplayVideoUiBatchContracts(unittest.TestCase):
    def test_export_cancellation_is_externally_owned_before_skin_preflight(self) -> None:
        header = (ROOT / "src/ReplayVideoExporter.h").read_text(encoding="utf-8")
        source = (ROOT / "src/ReplayVideoExporter.cpp").read_text(encoding="utf-8")
        self.assertIn("std::stop_token stop;", header)
        self.assertIn("resolved.stop = options.stop;", source)
        self.assertEqual(
            2,
            source.count("GameplaySkinSessionStopOwner skinSessionStopOwner(options.stop)"),
            "normal and course exports register external cancellation before preflight",
        )

    def test_every_main_menu_replay_job_forwards_its_active_stop_token(self) -> None:
        source = (ROOT / "src/scene/MainMenuScene.cpp").read_text(encoding="utf-8")
        start = source.index("void MainMenuScene::startAutoPlayVideoExport")
        end = source.index("MainMenuScene::activeReplayIrServerOrigin", start)
        replay_jobs = source[start:end]
        self.assertEqual(
            3,
            replay_jobs.count("exportOptions.stop = *stopToken;"),
            "autoplay, normal replay, and course replay jobs must forward their "
            "jthread cancellation authority into exporter preparation",
        )

    def test_lua_skin_audio_never_uses_the_live_mixer(self) -> None:
        source = (ROOT / "src/ReplayVideoExporter.cpp").read_text(encoding="utf-8")
        start = source.index("replayGameplaySkinSessionServices")
        end = source.index("replayExportPersistedScore", start)
        services = source[start:end]
        self.assertIn("createLuaSkinNoOutputAudioBackend", services)
        self.assertNotIn("createLuaSkinApplicationAudioBackend", services)
        self.assertNotIn("jukebox.audioRuntime", services)

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
