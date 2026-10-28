#include "goals/ChartGoal.h"
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
int scoreLongNoteModeForClearLamp(const bms_parser::ChartMeta &, int mode) { return mode; }
struct Scores {
  std::optional<chart_goals::GoalProgress> stored;
  std::optional<chart_goals::GoalProgress> LoadChartGoal(const bms_parser::ChartMeta &meta, int mode, std::string &error) {
    if (fail) { error = "storage failure"; return {}; }
    return stored && stored->goal.chart.SHA256 == meta.SHA256 && stored->goal.longNoteMode == mode ? stored : std::nullopt;
  }
  bool fail = false, achieved = false;
  int saves = 0, removals = 0;
  bool SaveChartGoal(const chart_goals::Goal &, std::string &error) {
    ++saves; if (fail) error = "storage failure"; else if (achieved) error = "goal_already_achieved"; return !fail && !achieved;
  }
  bool RemoveChartGoal(const bms_parser::ChartMeta &, int, std::string &error) {
    ++removals; if (fail) error = "storage failure"; return !fail;
  }
};
struct ChartGoalsScene {
  struct { Profiles profileManager; Scores scoreRepository;
    struct { std::string selectedLnMode = "HCN"; } settings; } context;
  std::string profileId_ = "one", message_, customRate_;
  bool editing_ = true, customScore_ = false, confirmRemove_ = false, loadFailed_ = false;
  bool showAchieved_ = false;
  int achievedOffset_ = 0;
  int offset_ = 20, builds = 0, reloads = 0;
  chart_goals::Goal draft_;
  chart_goals::ProgressPage page_;
  std::optional<chart_goals::GoalProgress> selected_;
  std::optional<Record> initial_;
  std::optional<int> initialMode_, resolved_;
  std::vector<std::function<bool()>> queued;
  void defer(std::function<bool()> action, int, bool) { queued.push_back(action); }
  void build() { ++builds; }
  void reload() { ++reloads; }
  void init();
  void showGoal(chart_goals::Goal);
  void resolveChart() {}
  bool checkProfile();
  void later(std::function<void()>);
  void saveGoal();
  void removeGoal();
  void flush() { auto work = std::exchange(queued, {}); for (auto &f : work) f(); }
};
PRODUCTION_METHODS
int main() {
  ChartGoalsScene existing;
  chart_goals::Goal saved;
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
  existing.showGoal({.chart=saved.chart, .longNoteMode=2});
  assert(existing.selected_ && !existing.editing_ && existing.draft_.target.lamp == 400);
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
