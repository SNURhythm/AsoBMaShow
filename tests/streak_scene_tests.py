"""Run streak UI callbacks through the production Scene deferred queue."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from gameplay_terminal_scene_extract import extract
from support.fixture_compiler import FixtureCompiler


class StreakSceneTests(unittest.TestCase):
    def test_first_routine_offers_reminders_only_after_success(self):
        root = Path(__file__).resolve().parents[1]
        scene = (root / "src/scene/StreakScene.cpp").read_text()
        methods = "\n".join(extract(scene, signature) for signature in (
            "void StreakScene::saveRoutine(", "bool StreakScene::saveReminders(",
            "void StreakScene::reminderPrompt("))
        fixture = (root / "tests/streak_reminder_prompt_fixture.cpp").read_text()
        fixture = fixture.replace("PRODUCTION_METHODS", methods)
        with tempfile.TemporaryDirectory(prefix="asobmashow-reminder-prompt-") as directory:
            source = Path(directory) / "prompt.cpp"
            binary = Path(directory) / "prompt"
            source.write_text(fixture)
            FixtureCompiler.from_environment(shared_headers=True).build(
                [source, root / "src/streak/StreakModel.cpp"], binary, source.parent,
                includes=(root / "src",))
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_assignment_shortcuts_open_requested_folder_and_clear_filters(self):
        root = Path(__file__).resolve().parents[1]
        main = extract((root / "src/scene/MainMenuScene.cpp").read_text(),
                       "void MainMenuScene::openStreakAssignment(")
        custom = extract((root / "src/scene/MusicSelectScene.cpp").read_text(),
                         "void MusicSelectScene::openStreakAssignment(")
        fixture = (root / "tests/streak_navigation_fixture.cpp").read_text()
        fixture = fixture.replace("MAIN_NAVIGATION", main).replace("CUSTOM_NAVIGATION", custom)
        with tempfile.TemporaryDirectory(prefix="asobmashow-streak-navigation-") as directory:
            source = Path(directory) / "navigation.cpp"
            binary = Path(directory) / "navigation"
            source.write_text(fixture)
            FixtureCompiler.from_environment(shared_headers=True).build([source], binary, source.parent)
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_level_summary_groups_tables_in_selection_order(self):
        root = Path(__file__).resolve().parents[1]
        helper = extract((root / "src/scene/StreakScene.cpp").read_text(), "std::string levelsText(")
        fixture = r'''#include "streak/StreakModel.h"
#include <algorithm>
#include <cassert>
HELPER
int main() {
  streak::Assignment a;
  assert(levelsText(a).empty());
  a.levels = {{"satellite", "0", "Satellite · sl0"}, {"stella", "0", "Stella · st0"},
              {"satellite", "1", "Satellite · sl1"}, {"satellite", "2", "Satellite · sl2"},
              {"stella", "1", "Stella · st1"}};
  assert(levelsText(a) == "Satellite · sl0, sl1, sl2 / Stella · st0, st1");
  a.levels = {{"first", "0", "Shared · x0"}, {"second", "1", "Shared · x1"}};
  assert(levelsText(a) == "Shared · x0 / Shared · x1");
  a.levels = {{"legacy", "2", ""}, {"legacy", "3", "Custom label"},
              {"table", "1", "Name · with dot · lv1"}, {"table", "2", "Name · with dot · lv2"}};
  assert(levelsText(a) == "2, Custom label / Name · with dot · lv1, lv2");
}
'''.replace("HELPER", helper)
        with tempfile.TemporaryDirectory(prefix="asobmashow-streak-levels-") as directory:
            source = Path(directory) / "levels.cpp"
            binary = Path(directory) / "levels"
            source.write_text(fixture)
            FixtureCompiler.from_environment(shared_headers=True).build(
                [source], binary, source.parent, includes=(root / "src",))
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_form_error_decoration_and_custom_rate_visibility(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/scene/StreakScene.cpp").read_text()
        error = extract(source, "TextView *fieldError(")
        visibility = extract(source, "const auto showCustomRate =") + ";"
        fixture = r'''#include <cassert>
#include <string>
#include <vector>
enum { YGDisplayNone, YGDisplayFlex };
namespace app_theme { int coral() { return 1; } }
namespace i18n { std::string tr(const std::string &key) { return key; } }
struct View {
  std::string name;
  int border = 0, color = 0, display = YGDisplayFlex;
  bool visible = true;
  std::vector<View *> children;
  void setName(const std::string &n) { name = n; }
  View *setBorderWidth(int width) { border = width; return this; }
  View *setThemedBorderColor(int (*provider)()) { color = provider(); return this; }
  void setDisplay(int next) { display = next; }
  void setVisible(bool next) { visible = next; }
};
struct TextView : View {
  std::string text;
  void setThemedColor(int (*provider)()) { color = provider(); }
};
TextView *label(View *parent, const std::string &text, int) {
  auto *view = new TextView(); view->text = text; parent->children.push_back(view); return view;
}
ERROR_HELPER
int main() {
  View parent, control;
  parent.children.push_back(&control);
  assert(fieldError(&parent, &control, "") == nullptr && control.border == 0);
  auto *error = fieldError(&parent, &control, "invalid input");
  assert(control.border == 2 && control.color == 1);
  assert(parent.children.back() == error && error->color == 1 && error->text == "invalid input");
  View rateGroup; auto *customRate = &rateGroup;
  VISIBILITY
  showCustomRate(false);
  assert(!rateGroup.visible && rateGroup.display == YGDisplayNone);
  showCustomRate(true);
  assert(rateGroup.visible && rateGroup.display == YGDisplayFlex);
  showCustomRate(false);
  assert(!rateGroup.visible && rateGroup.display == YGDisplayNone);
  delete error;
}
'''.replace("ERROR_HELPER", error).replace("VISIBILITY", visibility)
        with tempfile.TemporaryDirectory(prefix="asobmashow-streak-fields-") as directory:
            source = Path(directory) / "fields.cpp"
            binary = Path(directory) / "fields"
            source.write_text(fixture)
            FixtureCompiler.from_environment(shared_headers=True).build([source], binary, source.parent)
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_result_drawer_retry_capture_and_dismissal(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/scene/ResultScene.cpp").read_text()
        methods = "\n".join(extract(source, signature) for signature in (
            "void ResultScene::prepareStreakProgress()",
            "void ResultScene::updateStreakProgress()",
            "bool ResultScene::streakOwnsInput() const",
            "void ResultScene::dismissStreakProgress(",
            "bool ResultScene::handleStreakProgressEvent("))
        capture = extract(source, ".captureLegacyInputGeneration = [this]")
        capture = capture[capture.index("[this]"):]
        browser = extract(source, ".openBrowser = [this]")
        browser = browser[browser.index("[this]"):]
        fixture = (root / "tests/streak_result_input_fixture.cpp").read_text().replace("PRODUCTION_METHODS", methods)
        fixture = fixture.replace("CAPTURE_INPUT", capture).replace("OPEN_BROWSER", browser)
        with tempfile.TemporaryDirectory(prefix="asobmashow-streak-result-") as directory:
            source = Path(directory) / "result.cpp"
            binary = Path(directory) / "result"
            source.write_text(fixture)
            FixtureCompiler.from_environment(shared_headers=True).build(
                [source, root / "src/streak/StreakModel.cpp"], binary, source.parent,
                includes=(root / "src",))
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_sounds_coexist_with_loaded_skin_audio(self):
        root = Path(__file__).resolve().parents[1]
        sounds = (root / "src/streak/StreakSounds.h").read_text()
        sounds = sounds[sounds.index("namespace streak {"):]
        fixture = (root / "tests/streak_sound_fixture.cpp").read_text().replace("SOUNDS", sounds)
        with tempfile.TemporaryDirectory(prefix="asobmashow-streak-sounds-") as directory:
            source = Path(directory) / "sounds.cpp"
            binary = Path(directory) / "sounds"
            source.write_text(fixture)
            FixtureCompiler.from_environment(shared_headers=True).build([source], binary, source.parent)
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_adjustment_rechecks_day_inside_transaction(self):
        root = Path(__file__).resolve().parents[1]
        repository = (root / "src/repositories/ScoreRepositoryStreak.cpp").read_text()
        for name, parameter, operation in (
                ("AdjustStreakAssignment", "const streak::Assignment &", "adjustAssignment"),
                ("ResetStreakExtraChallenge", "const std::string &", "resetExtraChallenge")):
            method = extract(repository, "bool ScoreRepository::" + name + "(")
            fixture = (root / "tests/streak_adjust_transaction_fixture.cpp").read_text()
            fixture = fixture.replace("AdjustStreakAssignment", name).replace("adjustAssignment", operation)
            fixture = fixture.replace("const streak::Assignment &", parameter)
            if operation == "resetExtraChallenge":
                fixture = fixture.replace("const Assignment &", "const std::string &")
            fixture = fixture.replace("PRODUCTION_METHOD", method)
            with tempfile.TemporaryDirectory(prefix="asobmashow-streak-midnight-") as directory:
                source = Path(directory) / "midnight.cpp"
                binary = Path(directory) / "midnight"
                source.write_text(fixture)
                FixtureCompiler.from_environment(shared_headers=True).build([source], binary, source.parent)
                subprocess.run([str(binary)], check=True, timeout=5)

    def test_rebuild_keeps_scroll_and_animation_phase(self):
        root = Path(__file__).resolve().parents[1]
        scene = (root / "src/scene/StreakScene.cpp").read_text()
        fixture = (root / "tests/streak_rebuild_fixture.cpp").read_text()
        fixture = fixture.replace("BUILD", extract(scene, "void StreakScene::build("))
        fixture = fixture.replace("METER", extract(scene, "void meter("))
        with tempfile.TemporaryDirectory(prefix="asobmashow-streak-rebuild-") as directory:
            source = Path(directory) / "rebuild.cpp"
            binary = Path(directory) / "rebuild"
            source.write_text(fixture)
            FixtureCompiler.from_environment(shared_headers=True).build(
                [source], binary, source.parent, includes=(root / "src",))
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_actions_complete_once_and_allow_the_next_action(self):
        root = Path(__file__).resolve().parents[1]
        scene = (root / "src/scene/Scene.h").read_text()
        streak = (root / "src/scene/StreakScene.cpp").read_text()
        methods = "\n".join(extract(scene, signature) for signature in (
            "void defer(", "void handleDeferred()"))
        later = extract(streak, "void StreakScene::later(")
        navigation = extract(streak, "[open = openAssignment_")
        fixture = r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
using Uint64 = std::uint64_t;
Uint64 SDL_GetTicks() { return 100; }
namespace ui { void invalidate() {} }
class StreakScene {
 public:
  struct { Uint64 currentFrame = 0; } context;
  std::map<Uint64, std::pair<Uint64, std::vector<std::function<bool()>>>> deferred;
  std::mutex postedDeferredMutex_;
  std::vector<std::function<bool()>> postedDeferred_;
  bool isDead = false;
  int builds = 0;
  bool embeddedTab_ = true;
  std::function<void(const std::string &)> openAssignment_;
  void queueNavigation() { const std::string id="assignment"; defer(NAVIGATION, 0, true); }
  void build() { ++builds; }
  void later(std::function<void()>);
  METHODS
};
LATER
int main() {
 StreakScene scene;
 int first = 0, second = 0;
 scene.later([&] { ++first; });
 scene.handleDeferred();
 assert(first == 0);
 ++scene.context.currentFrame;
 scene.handleDeferred();
 assert(first == 1 && scene.builds == 1);
 scene.later([&] { ++second; });
 ++scene.context.currentFrame;
 scene.handleDeferred();
 scene.handleDeferred();
 assert(first == 1 && second == 1 && scene.builds == 2);
 assert(scene.deferred.empty());
 int opened = 0;
 scene.openAssignment_ = [&](const std::string &id) { assert(id=="assignment"); ++opened; };
 scene.queueNavigation();
 ++scene.context.currentFrame;
 scene.handleDeferred();
 scene.handleDeferred(); // Re-entering the embedded tab must not repeat navigation.
 assert(opened == 1 && scene.deferred.empty());
 auto *standalone = new StreakScene;
 standalone->embeddedTab_ = false;
 standalone->openAssignment_ = [&](const std::string &) { ++opened; delete standalone; standalone=nullptr; };
 standalone->queueNavigation();
 ++standalone->context.currentFrame;
 standalone->handleDeferred();
 assert(standalone == nullptr && opened == 2);
}
'''.replace("METHODS", methods).replace("LATER", later).replace("NAVIGATION", navigation)
        with tempfile.TemporaryDirectory(prefix="asobmashow-streak-ui-") as directory:
            source = Path(directory) / "streak.cpp"
            binary = Path(directory) / "streak"
            source.write_text(fixture)
            FixtureCompiler.from_environment(shared_headers=True).build([source], binary, source.parent)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
