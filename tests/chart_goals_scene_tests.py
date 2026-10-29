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
            "std::string rateText(", "void ChartGoalsScene::init(", "void ChartGoalsScene::showGoal(", "void ChartGoalsScene::showChartGoals(",
            "bool ChartGoalsScene::checkProfile(", "void ChartGoalsScene::later(", "void ChartGoalsScene::back(", "void ChartGoalsScene::browseCharts(",
            "void ChartGoalsScene::saveGoal(", "void ChartGoalsScene::removeGoal(", "void ChartGoalsScene::recommendGoal(", "void ChartGoalsScene::changeDraftMode("))
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

    def test_picker_orders_unfinished_first_without_reordering_each_group(self):
        root = Path(__file__).resolve().parents[1]
        method = extract((root / "src/scene/ChartGoalsScene.cpp").read_text(), "void orderGoalRows(")
        fixture = r'''
#include <algorithm>
#include <cassert>
#include <vector>
namespace chart_goals { struct GoalProgress { int id; struct { bool achieved; } progress; }; }
METHOD
int main() {
  std::vector<chart_goals::GoalProgress> rows{{1,{true}}, {2,{false}}, {3,{true}}, {4,{false}}};
  orderGoalRows(rows);
  assert(rows[0].id==2 && rows[1].id==4 && rows[2].id==1 && rows[3].id==3);
  rows.clear(); orderGoalRows(rows); assert(rows.empty());
}
'''.replace("METHOD", method)
        with tempfile.TemporaryDirectory(prefix="goal-picker-order-") as directory:
            source, binary = Path(directory) / "order.cpp", Path(directory) / "order"
            source.write_text(fixture)
            FixtureCompiler.from_environment().build([source], binary, source.parent)
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_goal_kind_state_retains_pages_scroll_and_recommendation_filters(self):
        root = Path(__file__).resolve().parents[1]
        method = extract((root / "src/scene/ChartGoalsScene.cpp").read_text(), "void ChartGoalsScene::rememberNavigation(")
        fixture = r'''
#include "goals/GoalNavigation.h"
#include <cassert>
#include <memory>
struct Scroll { float value; float getScrollOffset() { return value; } };
struct ChartGoalsScene {
  std::shared_ptr<goal_navigation::State> navigationState_=std::make_shared<goal_navigation::State>();
  Scroll content{123}, navigation{45}; Scroll *scroll_=&content, *navigationScroll_=&navigation;
  std::string renderedScrollKey_="achieved:20";
  std::map<std::string,float> scrollOffsets_;
  float navigationOffset_=0;
  goal_navigation::ChartTab overviewTab_=goal_navigation::ChartTab::Achieved;
  int offset_=40, achievedOffset_=20, recommendationTable_=7;
  bool recommendationSortTable_=true;
  std::optional<int> recommendationChart_, recommendationRank_=24;
  struct Group { int offset; }; std::map<int,Group> recommendations_{{24,{20}}, {26,{40}}};
  void rememberNavigation();
};
METHOD
int main() {
  ChartGoalsScene s; s.rememberNavigation(); const auto &state=*s.navigationState_;
  assert(state.chartTab==goal_navigation::ChartTab::Achieved && state.chartOffset==40 && state.chartAchievedOffset==20);
  assert(state.chartScrollOffsets.at("achieved:20")==123 && state.chartNavigationOffset==45);
  assert(state.chartRecommendationRank==24 && state.chartRecommendationOffsets.at(26)==40);
  assert(state.chartRecommendationTable==7 && state.chartRecommendationSortTable);
  s.recommendationChart_=1; s.recommendationRank_=27; s.rememberNavigation();
  assert(state.chartRecommendationRank==24); // Scoped recommendations do not replace the global overview.
}
'''.replace("METHOD", method)
        with tempfile.TemporaryDirectory(prefix="goal-kind-state-") as directory:
            source, binary = Path(directory) / "state.cpp", Path(directory) / "state"
            source.write_text(fixture)
            FixtureCompiler.from_environment().build([source], binary, source.parent, includes=(root / "src",))
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_scroll_keys_separate_overview_pages_and_goal_states(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/scene/ChartGoalsScene.cpp").read_text()
        method = extract(source, "std::string ChartGoalsScene::scrollKey(")
        fixture = r'''
#include <cassert>
#include <map>
#include <optional>
#include <string>
struct ChartGoalsScene {
 enum class OverviewTab { Ongoing, Recommendations, Achieved };
 OverviewTab overviewTab_ = OverviewTab::Ongoing;
 struct { struct { std::string SHA256; } chart; int longNoteMode=0, id=0; } draft_;
 std::optional<int> selected_, recommendationRank_;
 struct Record { struct { std::string SHA256; } meta; };
 std::optional<Record> recommendationChart_, pickerChart_;
 bool editing_=false, recommendationSortTable_=false;
 int recommendationTable_=0, offset_=0, achievedOffset_=0, pickerOffset_=0;
 struct Group { int offset=0; }; std::map<int, Group> recommendations_;
 std::string scrollKey() const;
};
METHOD
int main() {
 ChartGoalsScene s; auto ongoing=s.scrollKey(); s.offset_=20; assert(s.scrollKey()!=ongoing);
 s.overviewTab_=ChartGoalsScene::OverviewTab::Recommendations; s.recommendationRank_=24;
 s.recommendations_[24].offset=20; auto page=s.scrollKey(); s.recommendations_[24].offset=0; assert(page!=s.scrollKey());
 auto ordinary=s.scrollKey(); s.recommendationChart_=ChartGoalsScene::Record{{"chart"}};
 assert(s.scrollKey()!=ordinary); s.recommendationChart_.reset(); assert(s.scrollKey()==ordinary);
 s.selected_=1; s.draft_.chart.SHA256="chart"; auto detail=s.scrollKey(); s.editing_=true; assert(s.scrollKey()!=detail);
 s.editing_=false; s.draft_.id=2; assert(s.scrollKey()!=detail);
 s.draft_.longNoteMode=2; assert(s.scrollKey()!=detail);
}
'''.replace("METHOD", method)
        with tempfile.TemporaryDirectory(prefix="goals-scroll-key-") as directory:
            path = Path(directory) / "key.cpp"
            binary = Path(directory) / "key"
            path.write_text(fixture)
            FixtureCompiler.from_environment().build([path], binary, path.parent)
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_compact_target_labels_and_maximums(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/scene/ChartGoalsScene.cpp").read_text()
        method = extract(source, "void targets(")
        fixture = r'''
#include <cassert>
#include <string>
#include <vector>
#include <initializer_list>
#include <utility>
struct View {};
namespace chart_goals { struct Target { int lamp, scoreNumerator, scoreDenominator; }; }
namespace kombo { std::string lampLabel(int n) { return n == 600 ? "FULL COMBO" : "HARD"; } }
constexpr int kClearTypeFullComboRank = 600;
std::vector<std::string> labels;
void lamp(View *, const std::string &s, int) { labels.push_back(s); }
void label(View *, const std::string &s) { labels.push_back(s); }
std::string scoreTarget(const chart_goals::Target &t) { return t.scoreDenominator == 10000 ? "100.00% EX" : (t.scoreNumerator == t.scoreDenominator ? "MAX" : "AAA"); }
namespace i18n { std::string format(const char *, std::initializer_list<std::pair<std::string,std::string>> values) { return values.begin()->second + " or above"; } }
METHOD
int main() {
 View v;
 targets(&v, {400,24,27});
 assert((labels == std::vector<std::string>{"HARD or above", "AAA or above"}));
 labels.clear(); targets(&v, {600,27,27});
 assert((labels == std::vector<std::string>{"FULL COMBO", "MAX"}));
 labels.clear(); targets(&v, {-1,10000,10000});
 assert((labels == std::vector<std::string>{"100.00% EX"}));
}
'''.replace("METHOD", method)
        with tempfile.TemporaryDirectory(prefix="goals-target-labels-") as directory:
            path = Path(directory) / "labels.cpp"
            binary = Path(directory) / "labels"
            path.write_text(fixture)
            FixtureCompiler.from_environment().build([path], binary, path.parent)
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_recommendations_use_active_profile_scores(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/scene/ChartGoalsScene.cpp").read_text()
        method = extract(source, "void ChartGoalsScene::loadRecommendations(")
        fixture = r'''
#include <algorithm>
#include <cassert>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
struct ScoreRepository {};
struct ChartMetaRecord { struct { std::string SHA256; } meta; };
struct GoalRecommendation { std::string sha256; };
struct ChartMetaQuery { std::optional<std::vector<std::string>> allowedSha256; bool rawSongData=false; };
struct Charts {
  ScoreRepository *active=nullptr;
  struct Session {
    std::vector<GoalRecommendation> GoalRecommendations(int,int,int,bool,const std::string &hash = {}) {
      return hash == "active-chart" ? std::vector<GoalRecommendation>{{"active-chart"}} : std::vector<GoalRecommendation>{{"other-chart"}};
    }
    void QueryChartMeta(const ChartMetaQuery &, std::vector<ChartMetaRecord> &rows) { rows.push_back({{"active-chart"}}); }
  };
  std::optional<Session> OpenSession(ScoreRepository *scores=nullptr) {
    if (scores!=active) throw std::runtime_error("wrong score database");
    return Session{};
  }
};
namespace i18n { std::string tr(const char *s) { return s; } }
void SDL_Log(const char *, const char *) {}
struct ChartGoalsScene {
  struct { Charts chartRepository; ScoreRepository scoreRepository; } context;
  struct Group { int offset=0; bool expanded=false; std::vector<std::pair<GoalRecommendation,ChartMetaRecord>> rows; };
  std::map<int,Group> recommendations_;
  int recommendationTable_=0; bool recommendationSortTable_=false;
  std::string message_;
  std::optional<ChartMetaRecord> recommendationChart_;
  void loadRecommendations(int,int);
};
METHOD
int main() {
  ChartGoalsScene scene;
  scene.context.chartRepository.active=&scene.context.scoreRepository;
  scene.recommendationChart_ = ChartMetaRecord{{"active-chart"}};
  scene.loadRecommendations(24,0);
  assert(scene.message_.empty());
  assert(scene.recommendations_[24].expanded && scene.recommendations_[24].rows.size()==1);
}
'''.replace("METHOD", method)
        with tempfile.TemporaryDirectory(prefix="goals-profile-") as directory:
            path = Path(directory) / "profile.cpp"
            binary = Path(directory) / "profile"
            path.write_text(fixture)
            FixtureCompiler.from_environment().build([path], binary, path.parent)
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

    def test_goal_kind_cycle_preserves_tabs_selector_callbacks_and_profile_guard(self):
        root = Path(__file__).resolve().parents[1]
        methods = "\n".join(extract((root / f"src/scene/{scene}.cpp").read_text(),
            f"void {scene}::switchGoalKind(") for scene in ("ChartGoalsScene", "FolderGoalsScene"))
        fixture = (root / "tests/goal_kind_navigation_fixture.cpp").read_text().replace("METHODS", methods)
        with tempfile.TemporaryDirectory(prefix="goal-kind-navigation-") as directory:
            path = Path(directory) / "navigation.cpp"
            binary = Path(directory) / "navigation"
            path.write_text(fixture)
            FixtureCompiler.from_environment().build([path], binary, path.parent, includes=(root / "src",))
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_goal_columns_keep_tabs_close_and_center_main_when_space_allows(self):
        root = Path(__file__).resolve().parents[1]
        fixture = r'''
#include "goals/GoalNavigation.h"
#include <cassert>
#include <cmath>
int main() {
  for (float width : {1884.0f, 2524.0f}) {
    const auto c = goal_navigation::columns(width, true);
    const float mainLeft = c.leftPadding + c.navigationWidth + goal_navigation::columnGap;
    assert(c.navigationWidth < width * 0.2f);
    assert(goal_navigation::columnGap <= 8);
    assert(std::abs(mainLeft + c.bodyWidth / 2 - width / 2) < 0.01f);
    assert(std::abs(mainLeft + c.bodyWidth + c.rightPadding - width) < 0.01f);
  }
  for (float width : {1044.0f, 552.0f}) {
    const auto c = goal_navigation::columns(width, true);
    assert(c.leftPadding == 0 && c.rightPadding == 0);
    assert(c.bodyWidth > c.navigationWidth);
    assert(std::abs(c.navigationWidth + goal_navigation::columnGap + c.bodyWidth - width) < 0.01f);
  }
  const auto detail = goal_navigation::columns(1884, false);
  assert(detail.navigationWidth == 0 && detail.bodyWidth == 960);
  assert(detail.leftPadding == detail.rightPadding);
}
'''
        with tempfile.TemporaryDirectory(prefix="goal-columns-") as directory:
            path = Path(directory) / "columns.cpp"
            binary = Path(directory) / "columns"
            path.write_text(fixture)
            FixtureCompiler.from_environment().build([path], binary, path.parent, includes=(root / "src",))
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
  bool checkProfile() {return true;} void reload() {} void build() { layout(); }
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
namespace i18n { std::string tr(const char *s) { return s; } std::string format(const char *s, std::initializer_list<std::pair<std::string,std::string>>) { return s; } }
std::vector<std::string> titles, actions;
View view;
View *card(View *) { return &view; }
View *label(View *, const std::string &s, int size=21) { if (size==24) titles.push_back(s); return &view; }
std::string scope(const chart_goals::Goal &) { return "LN"; }
void targets(View *, int) {}
void goalSummary(View *, const chart_goals::GoalProgress &) {}
namespace goal_ui { View *clickableRow(View *, std::function<void()>) { return &view; } }
struct ChartMetaRecord {};
struct Context {};
template<class T> std::map<std::string, ChartMetaRecord> loadGoalArtwork(T &, const std::vector<const chart_goals::GoalProgress *> &, int) { return {}; }
void goalIdentity(View *, const decltype(chart_goals::Goal::chart) &meta, const ChartMetaRecord *, int) {
  titles.push_back(meta.Title + (meta.SubTitle.empty() ? "" : " " + meta.SubTitle));
}
void action(View *, const std::string &s, std::function<void()>,
    app_theme::ActionCategory = app_theme::ActionCategory::Neutral, int = 0) { actions.push_back(s); }
template<class T> bool returnToScene(T &, int) { return true; }
struct Scene {
  struct { std::vector<chart_goals::GoalProgress> goals; } page_;
  struct Manager {} manager; struct { Manager *sceneManager; } context{&manager};
  int returnTarget_=0;
  void defer(std::function<bool()> f, int, bool) { f(); }
  int offset_=0, achievedOffset_=0;
  enum class OverviewTab { Ongoing, Recommendations, Achieved };
  OverviewTab overviewTab_=OverviewTab::Ongoing;
  void later(std::function<void()> f) { f(); }
  void showGoal(chart_goals::Goal) {}
  void browseCharts() {}
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
  scene.overviewTab_=Scene::OverviewTab::Achieved; scene.achievedOffset_=20; scene.build();
  assert(titles.size()==5 && titles.front()=="Done 20" && titles.back()=="Done 24");
  scene.overviewTab_=Scene::OverviewTab::Ongoing; scene.offset_=20; scene.build();
  assert(titles.size()==5 && titles.front()=="Active 20");
  scene.page_.goals.clear(); scene.build();
  assert(titles.empty() && actions==std::vector<std::string>{"goals.ui.browse_charts"} && scene.offset_==0);
  scene.page_.goals.push_back({{{"Done", "0"}}, {true}}); scene.build();
  assert(actions==std::vector<std::string>({"goals.ui.view_achieved", "goals.ui.browse_charts"}));
  scene.overviewTab_=Scene::OverviewTab::Achieved; scene.page_.goals.front().progress.achieved=false; scene.build();
  assert(actions==std::vector<std::string>({"goals.ui.view_active", "goals.ui.browse_charts"}));
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
