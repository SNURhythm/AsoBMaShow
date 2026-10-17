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

    def test_scene_canvas_transition_refreshes_projection_before_drawing(self):
        source = (ROOT / "src/main.cpp").read_text()
        start = source.index("      } else if (bgfxLock.owns_lock() &&")
        end = source.index("#if TARGET_OS_IPHONE", start)
        refresh = source[start:end].replace("} else if", "if", 1) + "}"
        self.compile_and_run('''
#include <atomic>
#include <cassert>
#include <cmath>
#include <mutex>
namespace rendering { int window_width = 1371, window_height = 617; }
struct Context {
  struct Presentation { float laneAngleDegrees = 20, laneLength = 4; };
  struct Settings {
    Presentation value;
    const Presentation &presentation() const { return value; }
  } settings;
  std::atomic<bool> replayVideoExportActive{false};
  struct Access {
    bool requested = false;
    bool exportRequested() const { return requested; }
  } rendererAccess;
  std::mutex bgfxRenderMutex;
  int projectionWidth = 1371, projectionHeight = 617, restores = 0;
  void restoreGameplayRenderViews() {
    projectionWidth = rendering::window_width;
    projectionHeight = rendering::window_height;
    ++restores;
  }
};
int main() {
  Context context;
  float appliedLaneAngleDegrees = 20, appliedLaneLength = 4;
  int appliedUiCanvasWidth = 1371, appliedUiCanvasHeight = 617;
  std::unique_lock<std::mutex> bgfxLock(context.bgfxRenderMutex);
  auto refresh = [&] { REFRESH_TRANSFORMS };
  refresh();
  assert(context.restores == 0);
  // Android phone menu -> gameplay at unchanged 2400 x 1080 drawable size.
  rendering::window_width = 1920;
  rendering::window_height = 864;
  refresh();
  assert(context.projectionWidth == 1920 && context.projectionHeight == 864 &&
         "gameplay must draw with its own canvas projection on the first frame");
  refresh();
  assert(context.restores == 1 && "unchanged frames must not rebuild transforms");
  // Home/resume may already restore gameplay; exiting still needs menu projection.
  context.restoreGameplayRenderViews();
  rendering::window_width = 1371;
  rendering::window_height = 617;
  context.rendererAccess.requested = true;
  refresh();
  assert(context.projectionWidth == 1920 && "export retains renderer ownership");
  context.rendererAccess.requested = false;
  context.replayVideoExportActive = true;
  refresh();
  assert(context.projectionWidth == 1920);
  context.replayVideoExportActive = false;
  refresh();
  assert(context.projectionWidth == 1371 && context.projectionHeight == 617 &&
         "returning from gameplay must restore the menu projection");
  assert(context.restores == 3);
  rendering::window_height = 620;
  bgfxLock.unlock();
  refresh();
  assert(context.projectionHeight == 617 && "a frame without renderer ownership must defer");
  bgfxLock.lock();
  refresh();
  assert(context.projectionHeight == 620 && "height-only changes also refresh");
}
'''.replace("REFRESH_TRANSFORMS", refresh))

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

    def test_paused_gameplay_native_configuration_changes_wake_drawing(self):
        source = (ROOT / "src/scene/play/GamePlayScene.cpp").read_text()
        methods = extract(source,
                          "bool GamePlayScene::needsContinuousPresentation() const")
        methods += "\n" + extract(source,
                                  "void GamePlayScene::refreshRuntimePresentationConfiguration()")
        self.compile_and_run('''
#include "ui/PresentationSchedule.h"
#include <cassert>
enum class PresentationMode { BuiltIn, Skin };
struct Configuration {
  int visibleTimeDurationMilliseconds = 1000, notesDisplayTimingMilliseconds = 0;
  float configuredHispeed = 1, hispeedMultiplier = 1, laneCoverHispeedFactor = 1;
  bool laneCoverEnabled = true, liftEnabled = false, hiddenEnabled = false;
  float noteStartPositionPercent = 0, liftRatio = 0, hiddenRatio = 0;
};
struct Store {
  Configuration applied;
  int calls = 0;
  void setConfiguration(const Configuration &value) { applied = value; ++calls; }
};
struct Presentation {
  Configuration applied;
  int calls = 0;
  PresentationMode activeMode() const { return PresentationMode::BuiltIn; }
  void configure(const Configuration &value) { applied = value; ++calls; }
};
struct Hispeed {
  float value = 4;
  float hispeed() const { return value; }
};
struct GamePlayScene {
  bool playbackInitializationFailed = false, guidedAccessReminderPending = false;
  bool practiceMenuActive = false, coursePauseHoldActive = false, coursePauseHoldRewinding = false;
  long long practiceMenuStartPressedMicros = 0;
  struct State {} ownedState;
  State *state = &ownedState;
  struct {
    struct {
      bool isPaused() const { return true; }
      bool hasActiveVideoVisuals() const { return false; }
    } jukebox;
    struct { int notesDisplayTimingMilliseconds = 0; } settings;
  } context;
  Store store;
  Store *playfieldVisualStateStore = &store;
  Presentation builtIn;
  Presentation *presentation = &builtIn;
  Hispeed speed;
  Hispeed *playfieldHispeedState = &speed;
  Configuration playfieldPresentationConfiguration;
  bool playfieldLaneCoverEnabled = true, playfieldLiftEnabled = false, playfieldHiddenEnabled = false;
  float playfieldLaneCoverPercent = 0, playfieldLiftRatio = 0, playfieldHiddenRatio = 0;
  int duration = 1000;
  int effectiveVisibleTimeDurationMilliseconds() const { return duration; }
  bool needsContinuousPresentation() const;
  void refreshRuntimePresentationConfiguration();
};
GAMEPLAY_METHODS
int main() {
  GamePlayScene scene;
  ui::PresentationSchedule schedule;
  auto settle = [&] { schedule.didPresent(ui::revisions(), {}, 0); };
  settle();
  assert(!scene.needsContinuousPresentation());
  scene.playfieldVisualStateStore = nullptr;
  scene.refreshRuntimePresentationConfiguration();
  assert(!schedule.shouldRender(1, scene.needsContinuousPresentation()));
  scene.playfieldVisualStateStore = &scene.store;

  // Native lane-cover commands remain delivered during pause, without an SDL
  // event. The real refresh publishes the cover to both playfield consumers.
  scene.playfieldLaneCoverPercent = 40;
  scene.refreshRuntimePresentationConfiguration();
  assert(scene.store.calls == 1 && scene.builtIn.calls == 1);
  assert(scene.store.applied.noteStartPositionPercent == 40);
  assert(scene.builtIn.applied.noteStartPositionPercent == 40);
  assert(schedule.shouldRender(1, scene.needsContinuousPresentation()) &&
         "native lane-cover changes must wake a paused built-in playfield");

  settle();
  scene.speed.value = 6;
  scene.duration = 800;
  scene.refreshRuntimePresentationConfiguration();
  assert(scene.store.applied.configuredHispeed == 6);
  assert(scene.builtIn.applied.visibleTimeDurationMilliseconds == 800);
  assert(schedule.shouldRender(1, scene.needsContinuousPresentation()) &&
         "native speed changes must wake a paused built-in playfield");
}
'''.replace("GAMEPLAY_METHODS", methods))

    def test_paused_preview_native_lane_events_wake_drawing(self):
        scene_source = (ROOT / "src/scene/SettingsScene.cpp").read_text()
        preview_source = (ROOT / "src/scene/SettingsScenePreview.cpp").read_text()
        methods = extract(scene_source,
                          "bool SettingsScene::needsContinuousPresentation() const")
        methods += "\n" + extract(preview_source,
                                  "void SettingsScene::consumePreviewTransactions(")
        # MIDI/gyroscope events reach this owner-thread consumer through the
        # input registry, without main.cpp's SDL event invalidation. Empty lane
        # hits update only playfield state, with no judgement TextView mutation.
        self.compile_and_run('''
#include "ui/PresentationSchedule.h"
#include <array>
#include <cassert>
#include <cstddef>
#include <span>
enum class PresentationMode { BuiltIn, Skin };
struct Presentation {
  PresentationMode activeMode() const { return PresentationMode::BuiltIn; }
};
namespace gameplay {
enum class LaneVisualAction { Press, Release };
enum class NoteKind { LongTail };
enum class LongNoteRule { Classic };
struct GameplayInputResult {
  std::size_t noteId = 0;
  bool hasJudge = false, hasReplayEvent = false, hasLaneVisual = false;
  int judge = 0;
  struct { int action = 0, lane = 0; long long songTimeMicros = 0; } replayEvent;
  struct {
    LaneVisualAction action = LaneVisualAction::Press;
    int lane = 0, judge = 0;
    long long visualTimeMicros = 0;
  } laneVisual;
};
}
namespace judgement_indicator {
bool recordsInputTiming(int, bool) { return false; }
}
int makePlayfieldJudgeEventClock(long long, int, int) { return 0; }
struct Definition {
  struct Note {
    gameplay::NoteKind kind = gameplay::NoteKind::LongTail;
    gameplay::LongNoteRule longNoteRule = gameplay::LongNoteRule::Classic;
  } value;
  std::size_t noteCount() const { return 0; }
  const Note &note(std::size_t) const { return value; }
};
struct Events {
  bool pressed = false;
  void onLanePressed(int, int, long long) { pressed = true; }
  void onLaneReleased(int, long long) { pressed = false; }
  void onTimingSample(int, int) {}
};
struct SettingsScene {
  bool previewActive = true, previewPaused = true, previewRendererDirty = false;
  Presentation presentation;
  Presentation *previewPresentation = &presentation;
  Definition *previewDefinition = nullptr;
  Events events;
  Events *previewPresentationEvents = &events;
  long long previewElapsedMicros = 0;
  void publishPreviewJudgement(int, long long, int) {
    assert(false && "empty lane input must not rely on judgement label updates");
  }
  bool needsContinuousPresentation() const;
  void consumePreviewTransactions(std::span<const gameplay::GameplayInputResult>);
};
PREVIEW_METHODS
int main() {
  SettingsScene scene;
  ui::PresentationSchedule schedule;
  auto settle = [&] { schedule.didPresent(ui::revisions(), {}, 0); };
  settle();
  assert(!scene.needsContinuousPresentation());
  scene.consumePreviewTransactions({});
  assert(!schedule.shouldRender(1, scene.needsContinuousPresentation()));

  std::array<gameplay::GameplayInputResult, 1> transactions{};
  transactions[0].hasLaneVisual = true;
  scene.consumePreviewTransactions(transactions);
  assert(scene.events.pressed);
  assert(schedule.shouldRender(1, scene.needsContinuousPresentation()) &&
         "native lane press must wake a paused preview before maintenance");

  settle();
  transactions[0].laneVisual.action = gameplay::LaneVisualAction::Release;
  scene.consumePreviewTransactions(transactions);
  assert(!scene.events.pressed);
  assert(schedule.shouldRender(1, scene.needsContinuousPresentation()) &&
         "native lane release must clear a paused preview promptly");
}
'''.replace("PREVIEW_METHODS", methods))


if __name__ == "__main__":
    unittest.main()
