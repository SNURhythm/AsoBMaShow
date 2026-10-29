#include "goals/ChartGoal.h"
#include "goals/GoalNavigation.h"
#include <memory>
#include "LongNoteModeUtils.h"
#include <sstream>
#include <iomanip>
#include <cassert>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <utility>
namespace i18n { std::string tr(const char *key) { return key; } }
void SDL_Log(const char *, const char *) {}
struct Profile { std::string id = "one"; };
struct Profiles { Profile profile; const Profile &activeProfile() { return profile; } };
struct Record { bms_parser::ChartMeta meta; };
using ChartMetaRecord = Record;
int scoreLongNoteModeForClearLamp(const bms_parser::ChartMeta &, int mode) { return mode; }
struct Scores {
  std::optional<chart_goals::GoalProgress> stored;
  std::optional<chart_goals::GoalProgress> LoadChartGoal(const bms_parser::ChartMeta &meta, int mode, std::string &error, std::int64_t id = 0) {
    if (fail) { error = "storage failure"; return {}; }
    const auto goals = LoadChartGoalsForChart(meta, error);
    for (const auto &goal : *goals)
      if (id > 0 ? goal.goal.id == id : goal.goal.longNoteMode == mode) return goal;
    return {};
  }
  std::vector<chart_goals::GoalProgress> additional;
  std::optional<std::vector<chart_goals::GoalProgress>> LoadChartGoalsForChart(const bms_parser::ChartMeta &meta, std::string &error) {
    if (fail) { error = "storage failure"; return {}; }
    std::vector<chart_goals::GoalProgress> result;
    if (stored && stored->goal.chart.SHA256 == meta.SHA256) result.push_back(*stored);
    for (const auto &goal : additional) if (goal.goal.chart.SHA256 == meta.SHA256) result.push_back(goal);
    return result;
  }
  bool fail = false, achieved = false;
  int saves = 0, removals = 0;
  bool SaveChartGoal(chart_goals::Goal &, std::string &error) {
    ++saves; if (fail) error = "storage failure"; else if (achieved) error = "goal_already_achieved"; return !fail && !achieved;
  }
  bool RemoveChartGoal(const bms_parser::ChartMeta &, int, std::string &error, std::int64_t) {
    ++removals; if (fail) error = "storage failure"; return !fail;
  }
};
struct Manager { int returned = 0; };
bool returnToScene(Manager &manager, int) { ++manager.returned; return true; }
struct ChartGoalsScene {
  Manager manager;
  int returnTarget_ = 0;
  struct { Manager *sceneManager; Profiles profileManager; Scores scoreRepository;
    struct { std::string selectedLnMode = "HCN"; } settings; } context{.sceneManager=&manager};
  std::string profileId_ = "one", message_, customRate_;
  bool editing_ = true, customScore_ = false, confirmRemove_ = false, loadFailed_ = false;
  bool initialRecommendations_ = false;
  std::shared_ptr<goal_navigation::State> navigationState_ = std::make_shared<goal_navigation::State>();
  enum class OverviewTab { Ongoing, Recommendations, Achieved };
  OverviewTab overviewTab_ = OverviewTab::Ongoing;
  std::map<int, int> recommendations_;
  std::optional<int> recommendationRank_;
  int loadedRank = -1;
  void loadRecommendations(int rank, int offset) { assert(offset == 0); loadedRank = rank; }
  std::map<std::string, float> scrollOffsets_;
  std::string renderedScrollKey_;
  float navigationOffset_ = 0;
  void *navigationScroll_ = nullptr;
  int achievedOffset_ = 0;
  int offset_ = 20, builds = 0, reloads = 0;
  chart_goals::Goal draft_;
  chart_goals::ProgressPage page_;
  std::optional<chart_goals::GoalProgress> selected_;
  std::optional<Record> initial_, recommendationChart_, pickerChart_;
  std::optional<int> initialMode_, resolved_;
  std::vector<std::function<bool()>> queued;
  void defer(std::function<bool()> action, int, bool) { queued.push_back(action); }
  void build() { ++builds; }
  bool trackRecommendationScope = false;
  void reload() {
    ++reloads;
    if (pickerChart_) {
      std::string error;
      auto goals = context.scoreRepository.LoadChartGoalsForChart(pickerChart_->meta, error);
      loadFailed_ = !goals;
      if (goals) page_.goals = *goals;
    }
    if (trackRecommendationScope) recommendations_ = recommendationChart_
        ? std::map<int,int>{{24,1}} : std::map<int,int>{{26,2}};
  }
  void back();
  void init();
  void showChartGoals(const ChartMetaRecord &, int);
  void showGoal(chart_goals::Goal);
  void recommendGoal(const ChartMetaRecord &, int, int);
  void changeDraftMode(int);
  void resolveChart() {}
  bool checkProfile();
  void later(std::function<void()>);
  void saveGoal();
  void removeGoal();
  void flush() { auto work = std::exchange(queued, {}); for (auto &f : work) f(); }
};
PRODUCTION_METHODS
int main() {
  ChartGoalsScene recommended;
  recommended.initialRecommendations_ = true;
  recommended.recommendations_[24] = 1;
  recommended.init();
  assert(recommended.overviewTab_ == ChartGoalsScene::OverviewTab::Recommendations);
  assert(recommended.loadedRank == 24 && !recommended.initialRecommendations_);
  ChartGoalsScene noRecommendations;
  noRecommendations.initialRecommendations_ = true;
  noRecommendations.init();
  assert(noRecommendations.overviewTab_ == ChartGoalsScene::OverviewTab::Recommendations);
  assert(noRecommendations.loadedRank == -1);
  ChartGoalsScene chartRecommendations;
  chartRecommendations.editing_ = false;
  chartRecommendations.trackRecommendationScope = true;
  chartRecommendations.initial_ = Record{};
  chartRecommendations.initial_->meta.SHA256 = std::string(64, 'c');
  chartRecommendations.initialRecommendations_ = true;
  chartRecommendations.recommendations_[24] = 1;
  chartRecommendations.init();
  assert(chartRecommendations.overviewTab_ == ChartGoalsScene::OverviewTab::Recommendations);
  assert(!chartRecommendations.editing_ && !chartRecommendations.selected_);
  assert(chartRecommendations.recommendationChart_->meta.SHA256 == std::string(64, 'c'));
  assert(!chartRecommendations.initial_ && chartRecommendations.loadedRank == 24);
  chartRecommendations.recommendGoal(*chartRecommendations.recommendationChart_, 2, 24);
  assert(chartRecommendations.editing_);
  chartRecommendations.back(); chartRecommendations.flush();
  assert(!chartRecommendations.editing_ && chartRecommendations.recommendationChart_);
  chartRecommendations.back(); chartRecommendations.flush();
  assert(!chartRecommendations.recommendationChart_ && chartRecommendations.manager.returned == 0);
  assert(chartRecommendations.overviewTab_ == ChartGoalsScene::OverviewTab::Recommendations);
  assert(chartRecommendations.recommendations_.contains(26) && !chartRecommendations.recommendations_.contains(24));
  assert(chartRecommendations.loadedRank == 26);
  chartRecommendations.back(); chartRecommendations.flush();
  assert(chartRecommendations.manager.returned == 1);
  chartRecommendations.recommendationChart_ = Record{};
  chartRecommendations.context.profileManager.profile.id = "two";
  assert(!chartRecommendations.checkProfile() && !chartRecommendations.recommendationChart_);
  ChartGoalsScene rememberedOverview;
  rememberedOverview.overviewTab_ = ChartGoalsScene::OverviewTab::Recommendations;
  rememberedOverview.recommendations_[26] = 2;
  rememberedOverview.init();
  assert(rememberedOverview.loadedRank == 26 && !rememberedOverview.initialRecommendations_);
  ChartGoalsScene existing;
  chart_goals::Goal saved;
  saved.id = 1;
  saved.chart.SHA256 = std::string(64, 'a'); saved.longNoteMode = 2;
  saved.target = {.lamp=400, .scoreNumerator=24};
  existing.context.scoreRepository.stored = chart_goals::GoalProgress{saved, {}};
  existing.initial_ = Record{saved.chart}; existing.initialMode_ = 2;
  existing.init();
  assert(existing.selected_ && !existing.editing_ && existing.draft_.longNoteMode == 2);
  assert(existing.draft_.target.lamp == 400 && existing.draft_.target.scoreNumerator == 24);
  existing.showGoal({.chart=saved.chart, .longNoteMode=3});
  assert(!existing.selected_ && existing.editing_); // No saved target opens a fresh editor.
  assert(existing.draft_.longNoteMode == 3);
  existing.showGoal(saved);
  assert(existing.selected_ && !existing.editing_ && existing.draft_.target.lamp == 400);
  existing.recommendGoal(Record{saved.chart}, 2, 26);
  assert(existing.editing_ && !existing.customScore_);
  assert(existing.draft_.target.scoreNumerator == 26 && existing.draft_.target.scoreDenominator == 27);
  assert(existing.draft_.target.lamp == -1 && existing.draft_.longNoteMode == 2 && existing.draft_.id == 0);
  assert(existing.context.scoreRepository.saves == 0); // Recommendation only opens the editor.
  existing.recommendGoal(Record{saved.chart}, 3, 24);
  assert(!existing.selected_ && existing.editing_ && existing.draft_.longNoteMode == 3);
  assert(existing.draft_.target.scoreNumerator == 24);
  existing.recommendGoal(Record{saved.chart}, 2, 500);
  assert(existing.editing_ && existing.draft_.target.lamp == 500);
  assert(existing.draft_.target.scoreNumerator == 0 && existing.draft_.longNoteMode == 2);
  assert(existing.context.scoreRepository.saves == 0);
  existing.context.scoreRepository.stored->goal.target = {.lamp=400, .scoreNumerator=91, .scoreDenominator=100};
  existing.recommendGoal(Record{saved.chart}, 2, 500);
  assert(!existing.customScore_ && existing.draft_.id == 0);
  assert(existing.draft_.target.scoreNumerator == 0);
  existing.customScore_ = true; existing.customRate_ = "91.00";
  existing.draft_.target.scoreNumerator = 91; existing.draft_.target.scoreDenominator = 100;
  existing.changeDraftMode(3);
  assert(existing.editing_ && !existing.selected_ && existing.draft_.longNoteMode == 3);
  assert(existing.draft_.target.lamp == 500 && existing.customRate_ == "91.00");
  existing.changeDraftMode(2);
  assert(existing.editing_ && !existing.selected_ && existing.draft_.longNoteMode == 2);
  assert(existing.draft_.target.lamp == 500 && existing.customScore_);
  assert(existing.context.scoreRepository.saves == 0);
  ChartGoalsScene multiple;
  multiple.editing_ = false;
  multiple.context.scoreRepository.stored = chart_goals::GoalProgress{saved, {}};
  auto second = saved; second.id = 2; second.target = {.lamp=500};
  multiple.context.scoreRepository.additional.push_back({second, {}});
  multiple.initial_ = Record{saved.chart}; multiple.initialMode_ = 2;
  multiple.init();
  assert(multiple.pickerChart_ && !multiple.selected_ && !multiple.editing_);
  assert(multiple.page_.goals.size() == 2);
  multiple.showGoal(second);
  assert(multiple.selected_ && multiple.draft_.id == 2 && multiple.draft_.target.lamp == 500);
  multiple.back(); multiple.flush();
  assert(multiple.pickerChart_ && !multiple.selected_ && multiple.page_.goals.size() == 2);
  multiple.context.profileManager.profile.id = "two";
  assert(!multiple.checkProfile() && !multiple.pickerChart_ && multiple.page_.goals.empty());
  ChartGoalsScene scene;
  scene.saveGoal();
  assert(scene.context.scoreRepository.saves == 0 && scene.editing_);
  scene.draft_.target = {.lamp=400};
  scene.context.scoreRepository.fail = true;
  scene.saveGoal();
  assert(scene.context.scoreRepository.saves == 1 && scene.editing_);
  assert(scene.draft_.target.lamp == 400 && !scene.message_.empty());
  scene.customScore_ = true; scene.customRate_ = "99.99";
  scene.context.scoreRepository.fail = false;
  scene.context.scoreRepository.achieved = true;
  scene.saveGoal();
  assert(scene.editing_ && scene.message_ == "goals.ui.already_achieved");
  scene.context.scoreRepository.achieved = false;
  scene.saveGoal();
  assert(!scene.editing_ && scene.draft_.target.scoreNumerator == 9999);
  scene.selected_ = chart_goals::GoalProgress{scene.draft_, {}};
  scene.page_.goals = {*scene.selected_};
  scene.removeGoal();
  assert(scene.context.scoreRepository.removals == 0); // Confirmation required.
  scene.confirmRemove_ = true; scene.context.scoreRepository.fail = true;
  scene.removeGoal();
  assert(scene.selected_ && scene.offset_ == 20);
  scene.context.scoreRepository.fail = false;
  scene.removeGoal();
  assert(!scene.selected_ && scene.offset_ == 0);
  scene.editing_ = true; scene.initial_ = Record{}; scene.resolved_ = 1;
  scene.draft_.target.lamp = 600;
  int actions = 0;
  scene.later([&] { ++actions; });
  assert(actions == 0); // Never rebuild/delete the control inside its callback.
  scene.context.profileManager.profile.id = "two";
  scene.flush();
  assert(actions == 0 && scene.profileId_ == "two");
  assert(!scene.editing_ && !scene.initial_ && !scene.resolved_ && !scene.selected_);
  assert(scene.draft_.target.lamp == -1 && scene.offset_ == 0);
  scene.later([&] { ++actions; }); scene.flush(); assert(actions == 1);
}
