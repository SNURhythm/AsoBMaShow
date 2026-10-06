"""Exercise the note editor's production lane selection and edit callbacks."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from gameplay_terminal_scene_extract import extract
from support.fixture_compiler import FixtureCompiler


class SettingsNoteEditingTests(unittest.TestCase):
    def test_follow_changes_storage_without_adding_scratch_or_sharing_selection(self):
        root = Path(__file__).resolve().parents[1]
        method = extract((root / "src/scene/SettingsSceneControls.cpp").read_text(),
                         "void SettingsScene::appendBuiltInNoteControls(")
        # Compile the production preparation through its edit callback. The rest
        # only constructs widgets from these lanes, targets, and callback.
        preparation = method.split("  body->addView(", 1)[0]
        preparation += "  offeredLanes = lanes; editableTargets = targets; applyEdit = apply;\n}\n"
        fixture = r'''
#include "AppSettings.h"
#include "bms_parser.hpp"
#include "settings/BuiltInNoteEditing.h"
#include "scene/play/StartLaneIndicatorGeometry.h"
#include <cassert>
#include <functional>
#include <set>
struct View {};
struct LayoutMetrics {};
struct SettingsScene {
  struct { AppSettings settings; } context;
  std::map<int, std::set<int>> builtInNoteLanes;
  int builtInNoteType = 0;
  int lastLayoutWidth = 0;
  int commits = 0, syncs = 0;
  std::vector<int> offeredLanes;
  std::vector<built_in_notes::LaneTarget> editableTargets;
  std::function<void(built_in_notes::EditKind, int)> applyEdit;
  void persistSettings() { ++commits; }
  void syncPreviewPresentationConfiguration() { ++syncs; }
  void appendBuiltInNoteControls(View *, const LayoutMetrics &, int keyMode);
};
PRODUCTION_PREPARATION
int main() {
  using namespace built_in_notes;
  for (const int keyCount : {5, 7}) {
    for (const bool follow : {false, true}) {
      SettingsScene scene;
      auto &presentation = scene.context.settings.presentation();
      presentation.skin.follow5K1S = follow;
      presentation.skin.follow7K1S = follow;
      // Parent and scratchless editors intentionally have different selections.
      scene.builtInNoteLanes[keyCount] = {7};
      scene.builtInNoteLanes[-keyCount] = {0, keyCount - 1};
      const int storedMode = follow ? keyCount : -keyCount;
      presentation.builtInNotes[storedMode][0][Type::Normal] = {0x123456, 140};
      presentation.builtInNotes[storedMode][7][Type::Normal] = {0xABCDEF, 100};
      scene.appendBuiltInNoteControls(nullptr, {}, -keyCount);
      assert(scene.offeredLanes.size() == static_cast<std::size_t>(keyCount));
      assert(scene.offeredLanes.front() == 0 && scene.offeredLanes.back() == keyCount - 1);
      assert(scene.editableTargets.size() == 2);
      assert(scene.editableTargets[0].lane == 0 && scene.editableTargets[1].lane == keyCount - 1);
      assert(scene.editableTargets[0].palette != Palette::Scratch &&
             scene.editableTargets[1].palette != Palette::Scratch);
      scene.applyEdit(EditKind::Color, 0x778899);
      assert((presentation.builtInNotes.at(storedMode).at(0).at(Type::Normal) == Style{0x778899, 140}));
      assert(presentation.builtInNotes.at(storedMode).at(keyCount - 1).at(Type::Normal).color == 0x778899);
      assert(presentation.builtInNotes.at(storedMode).at(7).at(Type::Normal).color == 0xABCDEF);
      assert(!presentation.builtInNotes.contains(-storedMode));
      assert(scene.builtInNoteLanes.at(keyCount) == std::set<int>{7});
      assert((scene.builtInNoteLanes.at(-keyCount) == std::set<int>{0, keyCount - 1}));
      assert(scene.commits == 1 && scene.syncs == 1 && scene.lastLayoutWidth == -1);
    }
  }
}
'''.replace("PRODUCTION_PREPARATION", preparation)
        with tempfile.TemporaryDirectory(prefix="settings-note-editing-") as directory:
            directory = Path(directory)
            source = directory / "fixture.cpp"
            source.write_text(fixture)
            compiler = FixtureCompiler.from_environment()
            binary = directory / ("fixture" + compiler.executable_suffix)
            try:
                compiler.build([source, root / "src/settings/AudioVideoSettings.cpp"],
                               binary, directory, standard=23, includes=[root / "src"])
                subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            except subprocess.CalledProcessError as error:
                self.fail(error.stderr)


if __name__ == "__main__":
    unittest.main()
