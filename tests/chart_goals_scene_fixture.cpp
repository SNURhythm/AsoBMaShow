#include "goals/ChartGoal.h"
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
struct Scores {
  bool fail = false;
  int saves = 0, removals = 0;
  bool SaveChartGoal(const chart_goals::Goal &, std::string &error) {
    ++saves; if (fail) error = "storage failure"; return !fail;
  }
  bool RemoveChartGoal(const bms_parser::ChartMeta &, int, std::string &error) {
    ++removals; if (fail) error = "storage failure"; return !fail;
  }
};
struct ChartGoalsScene {
  struct { Profiles profileManager; Scores scoreRepository; } context;
  std::string profileId_ = "one", message_, customRate_;
  bool editing_ = true, customScore_ = false, confirmRemove_ = false, loadFailed_ = false;
  int offset_ = 20, builds = 0, reloads = 0;
  chart_goals::Goal draft_;
  chart_goals::ProgressPage page_;
  std::optional<chart_goals::GoalProgress> selected_;
  std::optional<int> initial_, resolved_;
  std::vector<std::function<bool()>> queued;
  void defer(std::function<bool()> action, int, bool) { queued.push_back(action); }
  void build() { ++builds; }
  void reload() { ++reloads; }
  bool checkProfile();
  void later(std::function<void()>);
  void saveGoal();
  void removeGoal();
  void flush() { auto work = std::exchange(queued, {}); for (auto &f : work) f(); }
};
PRODUCTION_METHODS
int main() {
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
  scene.editing_ = true; scene.initial_ = 1; scene.resolved_ = 1;
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
