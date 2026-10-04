// Real scene ownership and Settings event dispatch; graphics and display
// services are substituted. Run with ASan to detect reads after Back deletes
// the dynamically allocated Settings scene, as Intro and selectors do.
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace i18n {
std::uint64_t revision() { return 0; }
std::string tr(const char *key) { return key; }
}
using Uint64 = std::uint64_t;
Uint64 SDL_GetTicks64() { return 1; }
enum {
  SDL_APP_WILLENTERBACKGROUND = 1, SDL_APP_DIDENTERBACKGROUND,
  SDL_WINDOWEVENT, SDL_WINDOWEVENT_FOCUS_LOST, SDL_WINDOWEVENT_MINIMIZED,
  SDL_WINDOWEVENT_HIDDEN, SDL_MOUSEBUTTONUP
};
struct SDL_Event { int type = SDL_MOUSEBUTTONUP; struct { int event = 0; } window; };
class SceneManager;
struct BackgroundTasks { void setGameplayPaused(bool) {} };
struct ApplicationContext {
  Uint64 currentFrame = 0;
  int uiBatchRenderer = 0;
  SceneManager *sceneManager = nullptr;
  std::atomic_bool backgroundTasksPausedForForegroundScene = false;
  BackgroundTasks *chartLibraryTasks = nullptr;
  std::function<void()> notifyBackgroundTaskPauseStateChanged;
  std::function<void(bool)> setGameplayOrientationLocked;
  int gameplayBgaCompositeState = 0;
  struct { struct { int video = 0; } audioVideo; } settings;
};
struct RenderContext {
  explicit RenderContext(int) {}
  struct UiBatchScope { explicit UiBatchScope(RenderContext &) {} };
};
struct View {
  std::function<bool()> eventCallback;
  virtual ~View() = default;
  bool handleEvents(SDL_Event &) {
    auto callback = eventCallback;
    return callback ? callback() : true;
  }
  void propagateLanguageChange() {}
  void render(RenderContext &) {}
  static void dispatchTemporaryEventListeners(SDL_Event &) {}
  static void dispatchDeferredEventCallbacks() {}
};

PRODUCTION_SCENE_HEADER
PRODUCTION_MANAGER_HEADER
PRODUCTION_MANAGER_METHODS

struct DisplaySession {
  struct Result { std::string message; };
  int focusLosses = 0;
  bool hasDisplayPreview() const { return true; }
  std::optional<Result> onFocusLost() {
    ++focusLosses;
    return Result{"Display restored"};
  }
};
struct Color { int r, g, b, a; };
struct SettingsScene : Scene {
  using Scene::Scene;
  DisplaySession *audioVideoSession = nullptr;
  int displayDraft = 0;
  bool previewActive = false;
  int previewEvents = 0;
  int *destructions = nullptr;
  std::string displayStatus;
  int displayPreviewUpdates = 0;
  ~SettingsScene() override { if (destructions) ++*destructions; }
  void setDisplayStatus(const std::string &status, Color) { displayStatus = status; }
  void updateDisplayPreviewUi() { ++displayPreviewUpdates; }
  void forwardPreviewInputEvent(SDL_Event &) { ++previewEvents; }
  void init() override { addView(new View); }
  void update(float) override {}
  void renderScene() override {}
  void cleanupScene() override {}
  EventHandleResult handleEvents(SDL_Event &event) override;
};

PRODUCTION_SETTINGS_EVENTS

int main() {
  ApplicationContext context;
  int destructions = 0;
  SceneManager manager(context);
  manager.registerScene("Intro", std::make_unique<SettingsScene>(context));
  for (bool previewActive : {false, true}) {
    auto settings = std::make_unique<SettingsScene>(context);
    auto *active = settings.get();
    active->destructions = &destructions;
    active->previewActive = previewActive;
    manager.changeScene(std::move(settings));
    SDL_Event event;
    manager.handleEvents(event);
    assert(active->previewEvents == (previewActive ? 1 : 0));
    active->views.front()->eventCallback = [] { return false; };
    manager.handleEvents(event);
    assert(active->previewEvents == (previewActive ? 1 : 0) &&
           "consumed events do not reach the gameplay preview");
    DisplaySession display;
    active->audioVideoSession = &display;
    context.settings.audioVideo.video = 42;
    event.type = SDL_WINDOWEVENT;
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    manager.handleEvents(event);
    assert(display.focusLosses == 1 && active->displayDraft == 42);
    assert(active->displayStatus == "Display restored");
    assert(active->displayPreviewUpdates == 1);
    assert(active->previewEvents == (previewActive ? 1 : 0));
    event.type = SDL_MOUSEBUTTONUP;
    active->views.front()->eventCallback = [&manager] {
      manager.changeScene("Intro");
      return false;
    };
    manager.handleEvents(event);
    assert(manager.currentScene == manager.registeredScenes.at("Intro").get());
  }
  assert(destructions == 2 && "Back deletes each dynamic Settings scene");
}
