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
  void changeScene(std::unique_ptr<Scene> scene) { current = std::move(scene); }
};
struct Lifecycle {
  int calls = 0;
  skin::GameplaySkinAcquisition next;
  skin::GameplaySkinAcquisition acquireForSkinType(int type, bool boundary) {
    assert(type == 5 && !boundary);
    ++calls;
    return std::move(next);
  }
};
struct MainMenuScene {
  struct Context {
    struct { int scene = 0, background = 1; } profileSwitchBlockers;
    struct { struct { std::set<int> selectedSkinEntries; } skin; } settings;
    Lifecycle *gameplaySkinLifecycle = nullptr;
    SceneManager *sceneManager = nullptr;
  } context;
  std::vector<int> replayIrObservedRevisions;
  std::vector<std::function<bool()>> deferred;
  int scoreClearRanks = 0, scoreBestScores = 0, folderClearData = 0;
  int scoreClearRanksRevision = 0, refreshed = 0;
  void defer(std::function<bool()> callback, int delay, bool waitFrame) {
    assert(delay == 0 && waitFrame);
    deferred.push_back(std::move(callback));
  }
  void applyThemeChange() {}
  std::optional<int> prepareScoreQueryDatabase() { return {}; }
  void reloadProfileSelectionsFromSettings() {}
  std::optional<int> refreshScoreClearRankViews() { return {}; }
  void refreshLongNoteModeClearRankViews() {}
  void refreshLibraryIfNeeded() { ++refreshed; }
  void reselectCurrentChart() {}
  void onResume() RESUME_BODY
};

int main() {
  for (int scenario = 0; scenario < 5; ++scenario) {
    MainMenuScene menu;
    SceneManager manager;
    Lifecycle lifecycle;
    menu.context.sceneManager = &manager;
    if (scenario < 3) menu.context.gameplaySkinLifecycle = &lifecycle;
    if (scenario == 4) menu.context.settings.skin.selectedSkinEntries.insert(5);
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
