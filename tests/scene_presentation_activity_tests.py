"""Compile real scene activity methods against lightweight service snapshots."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.gameplay_terminal_scene_extract import extract
from tests.support.fixture_compiler import FixtureCompiler

ROOT = Path(__file__).resolve().parents[1]


class ScenePresentationActivityTests(unittest.TestCase):
    def compile_and_run(self, source):
        compiler = FixtureCompiler.from_environment()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.cpp"
            binary = Path(directory) / ("fixture" + compiler.executable_suffix)
            path.write_text(source)
            compiler.build([path], binary, directory, standard=23,
                           includes=[ROOT / "src"])
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_static_scene_overrides_and_unknown_scene_default(self):
        base = extract((ROOT / "src/scene/Scene.h").read_text(),
                       "virtual bool needsContinuousPresentation() const")
        classes, checks = [], []
        for name in ("IntroScene", "ChartRecordsScene", "LibraryTasksScene",
                     "IrUploadsScene", "MusicSelectSkinErrorScene", "MusicSelectScene"):
            method = extract((ROOT / "src/scene" / f"{name}.h").read_text(),
                             "bool needsContinuousPresentation() const override")
            classes.append(f"struct {name} : Scene {{ {method} }};")
            expected = "true" if name == "MusicSelectScene" else "false"
            checks.append(f"assert({name}().needsContinuousPresentation() == {expected});")
        self.compile_and_run("#include <cassert>\nstruct Scene { " + base + " };\n" +
                             "\n".join(classes) + "\nint main() {\n" +
                             "assert(Scene().needsContinuousPresentation());\n" +
                             "\n".join(checks) + "\n}\n")

    def test_production_scene_activity(self):
        methods = []
        for name in ("SettingsScene", "ChartViewerScene", "GamePlayScene",
                     "MusicPlayerScene", "MainMenuScene", "ResultScene"):
            directory = "src/scene/play" if name == "GamePlayScene" else "src/scene"
            methods.append(extract((ROOT / directory / f"{name}.cpp").read_text(),
                                   f"bool {name}::needsContinuousPresentation() const"))
        fixture = (ROOT / "tests/scene_presentation_activity_fixture.cpp").read_text()
        for skins in (0, 1):
            self.compile_and_run(f"#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS {skins}\n" +
                                 fixture.replace("SCENE_ACTIVITY_METHODS", "\n".join(methods)))

    def test_programmatic_canvas_changes_wake_idle_drawing(self):
        source = (ROOT / "src/scene/ChartViewerScene.cpp").read_text()
        methods = "\n".join(extract(source, signature) for signature in (
            "void setShowInvisibleNotes(", "void setActivePracticeMarker(",
            "void setPlaybackTime(", "void clearPlaybackTime()", "void clearGhostReplay()"))
        self.compile_and_run('''
#include "ui/PresentationSchedule.h"
#include <algorithm>
#include <cassert>
#include <vector>
namespace practice { enum class Marker { Start, End }; }
struct Canvas {
  bool showInvisibleNotes = false, playbackActive = false;
  long long playbackTimeMicros = 0;
  struct { practice::Marker active = practice::Marker::Start; } practiceRange;
  std::vector<int> replayGhostEvents, replayMissMarkers;
  void autofocusPlaybackCursor() {}
CANVAS_METHODS
};
int main() {
  Canvas canvas;
  ui::PresentationSchedule schedule;
  auto settle = [&] { schedule.didPresent(ui::revisions(), {}, 0); };
  settle();
  canvas.setShowInvisibleNotes(false);
  canvas.setActivePracticeMarker(practice::Marker::Start);
  canvas.setPlaybackTime(0, false);
  canvas.clearPlaybackTime();
  canvas.clearGhostReplay();
  assert(!schedule.shouldRender(1));
  canvas.setShowInvisibleNotes(true);
  assert(schedule.shouldRender(1));
  settle();
  canvas.setActivePracticeMarker(practice::Marker::End);
  assert(schedule.shouldRender(1));
  settle();
  canvas.setPlaybackTime(100, true);
  assert(schedule.shouldRender(1));
  settle();
  canvas.setPlaybackTime(100, true);
  assert(!schedule.shouldRender(1));
  canvas.clearPlaybackTime();
  assert(schedule.shouldRender(1));
  settle();
  canvas.replayMissMarkers.push_back(1);
  canvas.clearGhostReplay();
  assert(schedule.shouldRender(1));
}
'''.replace("CANVAS_METHODS", methods))

    def test_paused_music_seek_wakes_custom_progress_drawing(self):
        source = (ROOT / "src/scene/MusicPlayerScene.cpp").read_text()
        method = extract(source, "void setFraction(float value)")
        self.compile_and_run('''
#include "ui/PresentationSchedule.h"
#include <algorithm>
#include <cassert>
struct Fill { float fraction = 0; FILL_METHOD };
int main() {
  Fill fill;
  ui::PresentationSchedule schedule;
  schedule.didPresent(ui::revisions(), {}, 0);
  fill.setFraction(-1);
  assert(!schedule.shouldRender(1));
  fill.setFraction(0.5f);
  assert(schedule.shouldRender(1));
  schedule.didPresent(ui::revisions(), {}, 0);
  fill.setFraction(0.5f);
  assert(!schedule.shouldRender(1));
}
'''.replace("FILL_METHOD", method))


if __name__ == "__main__":
    unittest.main()
