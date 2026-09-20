#!/usr/bin/env python3
"""Regression contract for replay-video UI batch activation."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ReplayVideoUiBatchContracts(unittest.TestCase):
    def test_export_cancellation_is_externally_owned_before_skin_preflight(self) -> None:
        header = (ROOT / "src/ReplayVideoExportTypes.h").read_text(encoding="utf-8")
        source = (ROOT / "src/ReplayVideoExporter.cpp").read_text(encoding="utf-8")
        self.assertIn("std::stop_token stop;", header)
        self.assertIn("resolved.stop = options.stop;", source)
        self.assertEqual(
            2,
            source.count("GameplaySkinSessionStopOwner skinSessionStopOwner(options.stop)"),
            "normal and course exports register external cancellation before preflight",
        )

    def test_lua_skin_audio_never_uses_the_live_mixer(self) -> None:
        source = (ROOT / "src/ReplayVideoExporter.cpp").read_text(encoding="utf-8")
        start = source.index("replayGameplaySkinSessionServices")
        end = source.index("replayExportPersistedScore", start)
        services = source[start:end]
        self.assertIn("createLuaSkinNoOutputAudioBackend", services)
        self.assertNotIn("createLuaSkinApplicationAudioBackend", services)
        self.assertNotIn("jukebox.audioRuntime", services)

    def test_all_result_surfaces_use_capture_sessions(self) -> None:
        source = (ROOT / "src/ReplayVideoExporter.cpp").read_text(encoding="utf-8")
        for name, elapsed in (("resultPresentation", "videoTimeMicros - gameplayDurationMicros"),
                              ("stageResultPresentation", "resultOffsetMicros"),
                              ("courseResultPresentation", "resultOffsetMicros")):
            self.assertIn(f"{name}.prepare(", source)
            self.assertIn(f"{name}.render(renderContext,", source)
            self.assertIn(f"{name}.reset();", source)
            start = source.index(f"{name}.render(renderContext,")
            self.assertIn(elapsed, source[start:start + 220])
            self.assertGreater(source.rfind("RenderContext::UiBatchScope", 0, start),
                               source.rfind("[&]() {", 0, start))
        start = source.index("class PreparedReplayResultPresentation")
        end = source.index("replayExportPersistedScore", start)
        helper = source[start:end]
        self.assertIn("renderForVideoExport", helper)
        self.assertNotIn("renderForExport(", helper)
        self.assertIn("createLuaSkinNoOutputAudioBackend", helper)
        self.assertIn(".stop = stop", helper)
        self.assertNotIn("createLuaSkinApplicationAudioBackend", helper)

    def test_result_data_keeps_replay_mode_and_shared_lane_projection(self) -> None:
        source = (ROOT / "src/ReplayVideoExporter.cpp").read_text(encoding="utf-8")
        start = source.index("void populateReplayResultSkinData(")
        end = source.index("replayExportPersistedScore", start)
        projection = source[start:end]
        self.assertIn("data.keyModeOverride = replay.chartMeta.KeyMode;", projection)
        self.assertIn("resultReplayLanePattern(", projection)
        self.assertIn("beatorajaResultTimingStatistics(", projection)
        self.assertIn("data.courseResult = true;", source)
        self.assertIn("context, data, 15, resolvedOptions.stop", source)
        self.assertIn("context, data, 7, resolvedOptions.stop", source)

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
