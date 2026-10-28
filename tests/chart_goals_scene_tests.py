"""Exercise production goal editor and deferred profile guards with small fakes."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from gameplay_terminal_scene_extract import extract
from support.fixture_compiler import FixtureCompiler


class ChartGoalsSceneTests(unittest.TestCase):
    def test_edits_failures_deletion_and_profile_handoffs(self):
        root = Path(__file__).resolve().parents[1]
        scene = (root / "src/scene/ChartGoalsScene.cpp").read_text()
        methods = "\n".join(extract(scene, signature) for signature in (
            "std::string rateText(", "void ChartGoalsScene::init(", "void ChartGoalsScene::showGoal(",
            "bool ChartGoalsScene::checkProfile(", "void ChartGoalsScene::later(",
            "void ChartGoalsScene::saveGoal(", "void ChartGoalsScene::removeGoal(", "void ChartGoalsScene::recommendGoal("))
        fixture = (root / "tests/chart_goals_scene_fixture.cpp").read_text()
        fixture = fixture.replace("PRODUCTION_METHODS", methods)
        with tempfile.TemporaryDirectory(prefix="asobmashow-goals-scene-") as directory:
            source = Path(directory) / "goals.cpp"
            binary = Path(directory) / "goals"
            source.write_text(fixture)
            FixtureCompiler.from_environment(shared_headers=True).build(
                [source, root / "src/goals/ChartGoal.cpp"], binary, source.parent,
                includes=(root / "src",))
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_navigation_is_deferred_and_practice_returns_once(self):
        root = Path(__file__).resolve().parents[1]
        scene = (root / "src/scene/ChartGoalsScene.cpp").read_text()
        base = (root / "src/scene/Scene.h").read_text()
        fixture = (root / "tests/chart_goals_navigation_fixture.cpp").read_text()
        fixture = fixture.replace("NAVIGATE", extract(scene, "void ChartGoalsScene::navigate("))
        fixture = fixture.replace("DEFER_METHODS", "\n".join(extract(base, signature) for signature in (
            "void defer(", "void handleDeferred()")))
        with tempfile.TemporaryDirectory(prefix="asobmashow-goals-nav-") as directory:
            source = Path(directory) / "navigation.cpp"
            binary = Path(directory) / "navigation"
            source.write_text(fixture)
            FixtureCompiler.from_environment(shared_headers=True).build(
                [source], binary, source.parent, includes=(root / "src",))
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_best_score_displays_selector_rank(self):
        root = Path(__file__).resolve().parents[1]
        scene = (root / "src/scene/ChartGoalsScene.cpp").read_text()
        method = extract(scene, "std::string bestScoreText(")
        fixture = r'''
#include "ScoreRankUtils.h"
#include <cassert>
#include <optional>
#include <string>
struct ScoreBestSnapshot { int score=0, maxScore=0; };
namespace i18n { std::string tr(const char *) { return "No record"; } }
METHOD
int main() {
  assert(bestScoreText({}) == "No record");
  assert(bestScoreText(ScoreBestSnapshot{1777,2000}) == "1777 EX / 2000 · AA");
  assert(bestScoreText(ScoreBestSnapshot{1778,2000}) == "1778 EX / 2000 · AAA");
  assert(bestScoreText(ScoreBestSnapshot{2000,2000}) == "2000 EX / 2000 · MAX");
  assert(bestScoreText(ScoreBestSnapshot{5,0}) == "5 EX");
}
'''.replace("METHOD", method)
        with tempfile.TemporaryDirectory(prefix="goal-score-rank-") as directory:
            path = Path(directory) / "rank.cpp"
            binary = Path(directory) / "rank"
            path.write_text(fixture)
            FixtureCompiler.from_environment().build([path], binary, path.parent, includes=(root / "src",))
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_goal_artwork_path_prefers_jacket_and_falls_back_safely(self):
        root = Path(__file__).resolve().parents[1]
        scene = (root / "src/scene/ChartGoalsScene.cpp").read_text()
        method = extract(scene, "std::optional<std::filesystem::path> goalArtworkPath(")
        fixture = r'''
#include <cassert>
#include <filesystem>
#include <optional>
using path_t = std::filesystem::path;
struct ChartMetaRecord {
  struct { path_t Folder, StageFile, Banner; } meta;
  bool unavailable=false, solidArchive=false;
};
METHOD
int main() {
  ChartMetaRecord record; record.meta.Folder="charts/song";
  assert(!goalArtworkPath(nullptr) && !goalArtworkPath(&record));
  record.meta.Banner="banner.png";
  assert(*goalArtworkPath(&record)==path_t("charts/song/banner.png"));
  record.meta.StageFile="jacket.png";
  assert(*goalArtworkPath(&record)==path_t("charts/song/jacket.png"));
  record.unavailable=true; assert(!goalArtworkPath(&record));
  record.unavailable=false; record.solidArchive=true; assert(!goalArtworkPath(&record));
}
'''.replace("METHOD", method)
        with tempfile.TemporaryDirectory(prefix="goal-artwork-") as directory:
            path = Path(directory) / "artwork.cpp"
            binary = Path(directory) / "artwork"
            path.write_text(fixture)
            FixtureCompiler.from_environment().build([path], binary, path.parent)
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_same_orientation_resize_refreshes_layout(self):
        root = Path(__file__).resolve().parents[1]
        scene = (root / "src/scene/ChartGoalsScene.cpp").read_text()
        method = extract(scene, "void ChartGoalsScene::update(")
        fixture = r'''
#include <cassert>
namespace rendering { int window_width=1000, window_height=700; }
struct View { int w=1000,h=700; int getWidth() {return w;} int getHeight() {return h;} };
struct ChartGoalsScene {
  View view, *root_=&view; int layouts=0;
  bool checkProfile() {return true;} void reload() {} void build() {}
  void layout() { ++layouts; view.w=rendering::window_width; view.h=rendering::window_height; }
  void update(float);
};
METHOD
int main() { ChartGoalsScene scene; scene.update(0); assert(scene.layouts==0);
  rendering::window_width=800; scene.update(0); assert(scene.layouts==1);
  scene.update(0); assert(scene.layouts==1);
  rendering::window_height=600; scene.update(0); assert(scene.layouts==2); }
'''.replace("METHOD", method)
        with tempfile.TemporaryDirectory(prefix="goal-resize-") as directory:
            path = Path(directory) / "resize.cpp"
            binary = Path(directory) / "resize"
            path.write_text(fixture)
            FixtureCompiler.from_environment().build([path], binary, path.parent)
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_active_and_achieved_sections_page_independently(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/scene/ChartGoalsScene.cpp").read_text()
        start = source.index("    const auto renderGroup =")
        end = source.index("\n  }\n  if (!message_", start)
        fixture = r'''
#include <algorithm>
#include <cassert>
#include <functional>
#include <map>
#include <string>
#include <vector>
enum class FlexDirection { Row }; constexpr int YGAlignCenter=0;
struct View { View *setThemedColor(int) { return this; } View *setFlexDirection(FlexDirection) { return this; } View *setAlignItems(int) { return this; } View *setGap(int) { return this; } View *setFlexShrink(int) { return this; } View *setFlex(int) { return this; } View *setMinWidth(int) { return this; } void addView(View *) {} };
namespace chart_goals {
constexpr int pageSize = 20;
struct Goal { struct { std::string Title, SubTitle, SHA256; } chart; int target = 0; };
std::string normalizedHash(const std::string &s) { return s; }
struct GoalProgress { Goal goal; struct { bool achieved = false; } progress; };
}
namespace app_theme { constexpr int textSecondary=0; enum class ActionCategory { Neutral }; }
namespace ui_icons { constexpr int kHide=1, kPlus=2; }
namespace i18n { std::string tr(const char *s) { return s; } }
std::vector<std::string> titles, actions;
View view;
View *card(View *) { return &view; }
View *label(View *, const std::string &s, int size) { if (size==24) titles.push_back(s); return &view; }
std::string scope(const chart_goals::Goal &) { return "LN"; }
void targets(View *, int) {}
struct ChartMetaRecord {};
struct Context {};
std::map<std::string, ChartMetaRecord> loadGoalArtwork(Context &, const std::vector<const chart_goals::GoalProgress *> &, int) { return {}; }
void goalArtwork(View *, const ChartMetaRecord *) {}
void action(View *, const std::string &s, std::function<void()>,
    app_theme::ActionCategory = app_theme::ActionCategory::Neutral, int = 0) { actions.push_back(s); }
struct Scene {
  struct { std::vector<chart_goals::GoalProgress> goals; } page_;
  Context context;
  int offset_=0, achievedOffset_=0;
  bool showAchieved_=false;
  void later(std::function<void()> f) { f(); }
  void showGoal(chart_goals::Goal) {}
  void build() { titles.clear(); actions.clear(); View *body=&view; GROUPS }
};
int main() {
  Scene scene;
  for (int i=0;i<25;++i) {
    scene.page_.goals.push_back({{{"Done", std::to_string(i)}}, {true}});
    scene.page_.goals.push_back({{{"Active", std::to_string(i)}}, {false}});
  }
  scene.build();
  assert(titles.size()==20 && titles.front()=="Active 0" && titles.back()=="Active 19");
  assert(actions.back()=="goals.ui.show_achieved");
  scene.showAchieved_=true; scene.achievedOffset_=20; scene.build();
  assert(titles.size()==25 && titles[20]=="Done 20" && titles.back()=="Done 24");
  scene.offset_=20; scene.build();
  assert(titles.size()==10 && titles.front()=="Active 20" && titles[5]=="Done 20");
  scene.page_.goals.clear(); scene.build();
  assert(titles.empty() && actions.empty() && scene.offset_==0);
}
'''.replace("GROUPS", source[start:end])
        with tempfile.TemporaryDirectory(prefix="asobmashow-goal-groups-") as directory:
            path = Path(directory) / "groups.cpp"
            binary = Path(directory) / "groups"
            path.write_text(fixture)
            FixtureCompiler.from_environment().build([path], binary, path.parent)
            subprocess.run([str(binary)], check=True, timeout=5)



if __name__ == "__main__":
    unittest.main()
