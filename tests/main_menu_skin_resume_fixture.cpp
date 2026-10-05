#include "REPOSITORY_ROOT/src/scene/ReplayRecordTask.h"
#include <cassert>
#include <functional>
#include <memory>
#include <set>
#include <optional>
#include <string>
#include <vector>

// Stub the package acquisition boundary; launch-policy behavior has its own tests.
namespace skin {
struct SkinDiagnostic { std::string code, message; };
struct GameplaySkinActivationRequest { int sessionSerial = 0; };
enum class GameplaySkinAcquisitionDisposition { BuiltIn, Ready, Failed };
struct GameplaySkinAcquisitionFailure { SkinDiagnostic diagnostic; };
struct GameplaySkinAcquisition {
  GameplaySkinAcquisitionDisposition disposition = GameplaySkinAcquisitionDisposition::BuiltIn;
  std::optional<GameplaySkinActivationRequest> request;
  std::optional<GameplaySkinAcquisitionFailure> failure;
};
}
enum class MusicSelectLaunchKind { BuiltIn, SelectedSkin, Error };
struct MusicSelectLaunchDecision {
  MusicSelectLaunchKind kind;
  std::optional<skin::GameplaySkinActivationRequest> request;
  std::string selectedSkinPath;
  std::vector<skin::SkinDiagnostic> diagnostics;
};
MusicSelectLaunchDecision decideMusicSelectLaunch(skin::GameplaySkinAcquisition value) {
  if (value.disposition == skin::GameplaySkinAcquisitionDisposition::BuiltIn)
    return {.kind = MusicSelectLaunchKind::BuiltIn};
  if (value.disposition == skin::GameplaySkinAcquisitionDisposition::Ready)
    return {.kind = MusicSelectLaunchKind::SelectedSkin, .request = std::move(value.request)};
  return {.kind = MusicSelectLaunchKind::Error,
          .diagnostics = {value.failure->diagnostic}};
}

struct Scene { virtual ~Scene() = default; };
struct MusicSelectScene : Scene {
  skin::GameplaySkinActivationRequest request;
  template <class Context>
  MusicSelectScene(Context &, skin::GameplaySkinActivationRequest value)
      : request(std::move(value)) {}
};
struct MusicSelectSkinErrorScene : Scene {
  std::string path;
  std::vector<skin::SkinDiagnostic> diagnostics;
  template <class Context>
  MusicSelectSkinErrorScene(Context &, std::string value,
                            std::vector<skin::SkinDiagnostic> errors)
      : path(std::move(value)), diagnostics(std::move(errors)) {}
};
struct SceneManager {
  std::unique_ptr<Scene> current;
  std::string registered;
  void changeScene(const std::string &name) { registered = name; }
  void registerScene(const std::string &, std::unique_ptr<Scene>) {}
  void changeScene(std::unique_ptr<Scene> scene) { current = std::move(scene); }
};
struct Lifecycle {
  int calls = 0;
  bool ready = true;
  bool presentationReady() const { return ready; }
  skin::GameplaySkinAcquisition next;
  skin::GameplaySkinAcquisition acquireForSkinType(int type, bool boundary) {
    assert(type == 5 && !boundary);
    ++calls;
    return std::move(next);
  }
};
struct MainMenuScene : Scene {
  MainMenuScene() = default;
  template <class Context> MainMenuScene(Context &, bool) {}
  struct Context {
    struct { bool newcomerTutorialCompleted = true; } applicationUiState;
    std::atomic_bool appInBackground{false};
    struct { int scene = 0, background = 1; } profileSwitchBlockers;
    struct Settings { struct { std::set<int> selectedSkinEntries; } skin; Settings &presentation() { return *this; } } settings;
    Lifecycle *gameplaySkinLifecycle = nullptr;
    SceneManager *sceneManager = nullptr;
  } context;
  std::vector<int> replayIrObservedRevisions;
  std::vector<std::function<bool()>> deferred;
  int scoreClearRanks = 0, scoreBestScores = 0, folderClearData = 0;
  int scoreClearRanksRevision = 0, refreshed = 0;
  bool presentationSkinRefreshPending = false;
  void defer(std::function<bool()> callback, int delay, bool waitFrame) {
    assert(delay == 0 && waitFrame);
    deferred.push_back(std::move(callback));
  }
  ReplayRecordTask replayLoadTask_;
  bool replayResultRecallInProgress = false;
  struct Modal { void setResultRecallInProgress(bool) {} };
  Modal *recordsModal_ = nullptr;
  int resets = 0;
  void resetReplayWatchLoadingUi() { ++resets; }
  void applyReplayLoadCompletion() COMPLETION_BODY
  void applyThemeChange() {}
  std::optional<int> prepareScoreQueryDatabase() { return {}; }
  void reloadProfileSelectionsFromSettings() {}
  std::optional<int> refreshScoreClearRankViews() { return {}; }
  void refreshLongNoteModeClearRankViews() {}
  void refreshLibraryIfNeeded() { ++refreshed; }
  void reselectCurrentChart() {}
  void onResume() RESUME_BODY
  void queueSelectedSkinHandoff() HANDOFF_BODY
};

namespace rendering { int window_width = 800, window_height = 600; }
struct View { void setSize(int, int) {} void applyYogaLayout() {} };
enum class SettingsDestination { Profile };
struct SceneReturnTarget { static int Registered(const char *) { return 0; } };
struct SettingsScene : Scene {
  template <class Context> SettingsScene(Context &, SettingsDestination, int) {}
};
struct IntroScene {
  MainMenuScene::Context &context;
  View *rootLayout_ = nullptr, *startButton_ = nullptr;
  View *settingsButton_ = nullptr, *tutorialButton_ = nullptr;
  int layoutWidth_ = -1, layoutHeight_ = -1;
  bool pendingStart_ = false;
  int navigations = 0;
  void processNavigationInput() { ++navigations; }
  void stopInputListening() {}
  void start() INTRO_START_BODY
  void update(float) INTRO_UPDATE_BODY
  void openSettings() INTRO_SETTINGS_BODY
  void startTutorial() INTRO_TUTORIAL_BODY
  void cleanupScene() INTRO_CLEANUP_BODY
};

int main() {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  {
    MainMenuScene menu;
    SceneManager manager;
    Lifecycle lifecycle;
    menu.context.sceneManager = &manager;
    menu.context.gameplaySkinLifecycle = &lifecycle;
    IntroScene intro{menu.context};
    lifecycle.ready = false;
    intro.start();
    intro.update(0);
    assert(lifecycle.calls == 0 && !manager.current && manager.registered.empty());
    lifecycle.next.disposition = skin::GameplaySkinAcquisitionDisposition::Ready;
    lifecycle.next.request.emplace();
    lifecycle.ready = true;
    intro.update(0);
    assert(lifecycle.calls == 1 && dynamic_cast<MusicSelectScene *>(manager.current.get()));
    intro.update(0);
    assert(lifecycle.calls == 1);
  }
  for (int cancel = 0; cancel < 3; ++cancel) {
    MainMenuScene menu;
    SceneManager manager;
    Lifecycle lifecycle;
    menu.context.sceneManager = &manager;
    menu.context.gameplaySkinLifecycle = &lifecycle;
    IntroScene intro{menu.context};
    lifecycle.ready = false;
    intro.start();
    if (cancel == 0) intro.openSettings();
    if (cancel == 1) intro.startTutorial();
    if (cancel == 2) intro.cleanupScene();
    lifecycle.ready = true;
    intro.update(0);
    assert(lifecycle.calls == 0);
    if (cancel == 0) assert(dynamic_cast<SettingsScene *>(manager.current.get()));
    if (cancel == 1) assert(manager.registered == "MainMenu");
  }
  for (bool cancel : {false, true}) {
    MainMenuScene menu;
    Lifecycle lifecycle;
    menu.context.gameplaySkinLifecycle = &lifecycle;
    lifecycle.ready = false;
    int transitions = 0;
    auto chart = std::make_shared<int>(42);
    std::weak_ptr<int> ownedChart = chart;
    std::atomic_bool published{false};
    menu.replayLoadTask_.start([&, chart](auto) {
      menu.replayLoadTask_.publish([&, chart] { assert(*chart == 42); ++transitions; });
      published = true;
    });
    chart.reset();
    while (!published) std::this_thread::yield();
    menu.applyReplayLoadCompletion();
    assert(transitions == 0 && menu.resets == 0 && !ownedChart.expired());
    assert(menu.replayLoadTask_.active());
    if (cancel) menu.replayLoadTask_.cancelAndWait();
    lifecycle.ready = true;
    menu.applyReplayLoadCompletion();
    assert(transitions == (cancel ? 0 : 1));
    assert(ownedChart.expired());
  }
  {
    MainMenuScene menu;
    SceneManager manager;
    Lifecycle lifecycle;
    menu.context.sceneManager = &manager;
    menu.context.gameplaySkinLifecycle = &lifecycle;
    lifecycle.ready = false;
    menu.onResume();
    menu.deferred.back()();
    assert(lifecycle.calls == 0 && !manager.current && menu.presentationSkinRefreshPending);
    lifecycle.ready = true;
    lifecycle.next.disposition = skin::GameplaySkinAcquisitionDisposition::Ready;
    lifecycle.next.request.emplace();
    menu.queueSelectedSkinHandoff();
    menu.deferred.back()();
    assert(lifecycle.calls == 1 && manager.current && !menu.presentationSkinRefreshPending);
  }
#endif
  for (int scenario = 0; scenario < 5; ++scenario) {
    MainMenuScene menu;
    SceneManager manager;
    Lifecycle lifecycle;
    menu.context.sceneManager = &manager;
    if (scenario < 3) menu.context.gameplaySkinLifecycle = &lifecycle;
    if (scenario == 4) menu.context.settings.presentation().skin.selectedSkinEntries.insert(5);
    menu.onResume();
    assert(menu.refreshed == 1 && !manager.current && lifecycle.calls == 0);
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
    assert(menu.deferred.size() == 1);
    // Resolve the committed selection when the deferred callback runs.
    if (scenario == 1) {
      lifecycle.next.disposition = skin::GameplaySkinAcquisitionDisposition::Ready;
      lifecycle.next.request.emplace();
      lifecycle.next.request->sessionSerial = 42;
    } else if (scenario == 2) {
      lifecycle.next.disposition = skin::GameplaySkinAcquisitionDisposition::Failed;
      lifecycle.next.failure.emplace();
      lifecycle.next.failure->diagnostic.code = "selected-skin-failed";
    }
    const bool keepProcessing = menu.deferred.front()();
    assert(lifecycle.calls == (scenario < 3 ? 1 : 0));
    if (scenario == 1) {
      auto *selected = dynamic_cast<MusicSelectScene *>(manager.current.get());
      assert(selected && selected->request.sessionSerial == 42);
      assert(!keepProcessing);
    } else if (scenario == 2 || scenario == 4) {
      auto *error = dynamic_cast<MusicSelectSkinErrorScene *>(manager.current.get());
      assert(error && error->diagnostics.size() == 1 && !keepProcessing);
      assert(error->diagnostics.front().code == (scenario == 2
          ? "selected-skin-failed" : "skin.music_select.lifecycle_unavailable"));
    } else {
      assert(!manager.current && keepProcessing);
    }
#else
    assert(menu.deferred.empty());
#endif
  }
}
