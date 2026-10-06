"""Exercise production cover adjustment without the graphics/audio runtime."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from gameplay_terminal_scene_extract import extract
from support.fixture_compiler import FixtureCompiler


class LaneCoverSceneInputTests(unittest.TestCase):
    def test_replay_adjustments_leave_recorded_and_personal_covers_untouched(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/scene/play/GamePlayScene.cpp").read_text()
        methods = "\n".join(extract(source, signature) for signature in (
            "lane_cover::State GamePlayScene::laneCoverState() const noexcept",
            "void GamePlayScene::adjustLaneCoverFromInput(float deltaPercent)",
        ))
        fixture = r'''
#include "AppSettings.h"
#include "replay/ReplayLaneCoverChange.h"
#include <cassert>
#include <memory>
struct HiSpeed {
  void setLaneCover(float, double, bool) {}
  void resetHispeed(double) {}
};
struct GamePlayScene {
  struct Jukebox { long long getTimeMicros() const { return 1000; } };
  struct { AppSettings settings; Jukebox jukebox; } context;
  bool replay = false;
  float playfieldLaneCoverPercent = 50;
  bool playfieldLaneCoverEnabled = true;
  bool playfieldLiftEnabled = true;
  float playfieldLiftRatio = 0.4F;
  bool playfieldHiddenEnabled = true;
  float playfieldHiddenRatio = 0.3F;
  bool playfieldChangeLiftTarget = true;
  bool playfieldLaneCoverResetPending = false;
  bool floatingLaneCoverSettingsDirty = false;
  std::unique_ptr<HiSpeed> playfieldHispeedState = std::make_unique<HiSpeed>();
  int saves = 0, configurations = 0, recordings = 0;
  ReplayLaneCoverChangeKind recordedKind = ReplayLaneCoverChangeKind::Value;
  bool isReplayPlayback() const { return replay; }
  bool courseNoSpeed() const { return false; }
  bool practiceInputAllowed(long long) const { return true; }
  long long getGameplayTimeMicros(long long value) const { return value; }
  double noteDisplayBpmAtGameplayTime(long long) const { return 120; }
  void refreshRuntimePresentationConfiguration() { ++configurations; }
  void persistFloatingLaneCoverSettings() { ++saves; }
  void appendReplayLaneCoverEvent(float, long long, bool, ReplayLaneCoverChangeKind kind) {
    ++recordings;
    recordedKind = kind;
  }
  lane_cover::State laneCoverState() const noexcept;
  void adjustLaneCoverFromInput(float deltaPercent);
};
PRODUCTION_METHODS
int main() {
  for (int target = 0; target < 3; ++target) {
    GamePlayScene scene;
    scene.playfieldLaneCoverEnabled = target == 0;
    scene.playfieldChangeLiftTarget = target != 2;
    scene.context.settings.presentation().setLaneCoverState({
        .laneCoverPercent = 12, .laneCoverEnabled = false,
        .liftEnabled = false, .liftRatio = 0.15F,
        .hiddenEnabled = false, .hiddenRatio = 0.2F});
    const auto personal = scene.context.settings.presentation();
    const auto recorded = scene.laneCoverState();
    scene.replay = true;
    scene.adjustLaneCoverFromInput(0.5F);
    assert(scene.context.settings.presentation() == personal);
    assert(scene.laneCoverState() == recorded);
    assert(scene.saves == 0 && scene.configurations == 0 && scene.recordings == 0);
    assert(!scene.floatingLaneCoverSettingsDirty);

    // The same entry point still adjusts and records every mode in live play.
    scene.replay = false;
    scene.context.settings.presentation().setLaneCoverState(recorded);
    scene.adjustLaneCoverFromInput(0.5F);
    assert(scene.laneCoverState() != recorded);
    assert(scene.context.settings.presentation().laneCoverState() == scene.laneCoverState());
    assert(scene.saves == 1 && scene.configurations == 1 && scene.recordings == 1);
    const auto expected = target == 0 ? ReplayLaneCoverChangeKind::Value
                        : target == 1 ? ReplayLaneCoverChangeKind::Lift
                                      : ReplayLaneCoverChangeKind::Hidden;
    assert(scene.recordedKind == expected);
  }
}
'''.replace("PRODUCTION_METHODS", methods)
        compiler = FixtureCompiler.from_environment()
        with tempfile.TemporaryDirectory(prefix="lane-cover-scene-") as directory:
            directory = Path(directory)
            source = directory / "fixture.cpp"
            source.write_text(fixture)
            binary = directory / ("fixture" + compiler.executable_suffix)
            compiler.build([source, root / "src/settings/AudioVideoSettings.cpp"],
                           binary, directory, standard=23, includes=[root / "src"])
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
