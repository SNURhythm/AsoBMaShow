#include "goals/ChartGoal.h"
#include "AppSettings.h"
#include "scene/GoalChallengeSettings.h"
#include "LongNoteModeUtils.h"
#include "scene/play/GameplayGaugeTypes.h"
#include <cassert>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
using Uint64 = std::uint64_t;
Uint64 SDL_GetTicks() { return 100; }
namespace ui { void invalidate() {} }
namespace i18n { std::string tr(const char *key) { return key; } }
struct Record { bms_parser::ChartMeta meta; };
struct SceneReturnTarget { static SceneReturnTarget Retained(void *) { return {}; } };
struct Viewer {
  template<class C, class... A> Viewer(C &, Record record, A &&...) : mode(record.meta.LnMode) {}
  int mode;
};
using ChartViewerScene = Viewer;
struct Manager {
  int opened = 0, mode = 0, returned = 0;
  void changeScene(std::unique_ptr<Viewer> scene, bool retained) {
    assert(retained); ++opened; mode = scene->mode;
  }
};
bool returnToScene(Manager &manager, const SceneReturnTarget &) { ++manager.returned; return true; }
struct Profile { std::string id = "one"; };
struct Profiles { Profile profile; const Profile &activeProfile() { return profile; } };
struct ChartGoalsScene {
  Manager manager;
  struct { Uint64 currentFrame = 0; Profiles profileManager; bool storageMaintenanceActive = false; Manager *sceneManager;
    AppSettings settings; bool saveSettings() { return true; } } context{.sceneManager=&manager};
  std::map<Uint64, std::pair<Uint64, std::vector<std::function<bool()>>>> deferred;
  std::mutex postedDeferredMutex_;
  std::vector<std::function<bool()>> postedDeferred_;
  bool isDead = false, available = true;
  std::string profileId_ = "one", message_;
  chart_goals::Goal draft_;
  std::optional<Record> resolved_;
  SceneReturnTarget returnTarget_;
  std::function<void(const Record &, int)> onSelectChart_;
  void resolveChart() { if (available) resolved_ = Record{draft_.chart}; else resolved_.reset(); }
  bool checkProfile() { profileId_ = context.profileManager.activeProfile().id; return false; }
  void reload() {} void build() {}
  void navigate(bool);
  DEFER_METHODS
};
NAVIGATE
int main() {
  ChartGoalsScene scene;
  scene.draft_.longNoteMode = 2;
  scene.draft_.id = 42;
  scene.context.settings.selectedGaugeType = "normal";
  scene.context.settings.selectedGaugeAutoShiftMode = "none";
  scene.draft_.target.lamp = kClearTypeHardClearRank;
  scene.navigate(true);
  scene.handleDeferred(); assert(scene.manager.opened == 0);
  ++scene.context.currentFrame;
  scene.handleDeferred(); assert(scene.manager.opened == 1 && scene.manager.mode == 2);
  assert(scene.context.settings.selectedLnMode == "CN");
  assert(scene.context.settings.selectedPacemakerTarget == "BEST"); // Practice does not challenge.

  scene.handleDeferred(); // Returning from retained practice must not reopen it.
  assert(scene.manager.opened == 1 && scene.deferred.empty());
  scene.navigate(true);
  scene.context.profileManager.profile.id = "two";
  ++scene.context.currentFrame;
  scene.handleDeferred(); assert(scene.manager.opened == 1 && scene.deferred.empty());
  scene.available = false; scene.navigate(false); ++scene.context.currentFrame;
  scene.handleDeferred(); assert(scene.manager.returned == 0 && !scene.message_.empty());
  scene.available = true; int selected = 0;
  scene.onSelectChart_ = [&](const Record &, int mode) { assert(mode == 2); ++selected; };
  scene.navigate(false); ++scene.context.currentFrame; scene.handleDeferred();
  assert(selected == 1 && scene.manager.returned == 1);
  assert(scene.context.settings.selectedPacemakerTarget == "GOAL");
  assert(scene.context.settings.selectedPacemakerGoalId == 42);
  assert(scene.context.settings.selectedGaugeType == "hard");
}
