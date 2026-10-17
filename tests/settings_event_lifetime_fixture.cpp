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
Uint64 SDL_GetTicks() { return 1; }
enum {
  SDL_EVENT_WILL_ENTER_BACKGROUND = 1, SDL_EVENT_DID_ENTER_BACKGROUND,
  SDL_EVENT_WINDOW_FOCUS_LOST, SDL_EVENT_WINDOW_MINIMIZED,
  SDL_EVENT_WINDOW_HIDDEN, SDL_EVENT_MOUSE_BUTTON_UP
};
constexpr int SDL_EVENT_WINDOW_FIRST = SDL_EVENT_WINDOW_FOCUS_LOST;
constexpr int SDL_EVENT_WINDOW_LAST = SDL_EVENT_WINDOW_HIDDEN;
struct SDL_Event { int type = SDL_EVENT_MOUSE_BUTTON_UP; struct { int event = 0; } window; };
class SceneManager;
namespace player_settings { enum class PresentationOrientation { Landscape, Portrait }; }
struct BackgroundTasks { void setGameplayPaused(bool) {} };
struct ApplicationContext {
  Uint64 currentFrame = 0;
  int uiBatchRenderer = 0;
  SceneManager *sceneManager = nullptr;
  std::atomic_bool backgroundTasksPausedForForegroundScene = false;
  std::atomic_bool storageMaintenanceActive = false;
  BackgroundTasks *chartLibraryTasks = nullptr;
  std::function<void()> notifyBackgroundTaskPauseStateChanged;
  std::function<void(bool)> setGameplayOrientationLocked;
  std::function<void(bool)> setResponsiveMenuCanvas;
  int gameplayBgaCompositeState = 0;
  struct {
    int stops = 0;
    bool SetStorageMaintenance(bool active, std::string &) { if (active) ++stops; return true; }
  } musicPlayer;
  struct {
    int stops = 0;
    struct Result { bool success = true; std::string diagnostic; };
    bool fail = false;
    Result discardLoadedMedia() { ++stops; return {!fail, fail ? "backend busy" : ""}; }
  } jukebox;
  struct {
    struct { int video = 0; } audioVideo;
    player_settings::PresentationOrientation orientation = player_settings::PresentationOrientation::Landscape;
    auto activePresentationOrientation() const { return orientation; }
    void setActivePresentationOrientation(player_settings::PresentationOrientation value) { orientation = value; }
  } settings;
};
struct RenderContext {
  explicit RenderContext(int) {}
  struct UiBatchScope { explicit UiBatchScope(RenderContext &) {} };
};
struct View {
  void cancelPointerInput() {}
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
PRODUCTION_RETURN_TARGET
PRODUCTION_RETURN_METHOD

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
  View *appearanceColorPopup = nullptr;
  int displayDraft = 0;
  bool previewActive = false;
  int previewEvents = 0;
  int previewInputCleanups = 0;
  int *destructions = nullptr;
  std::string displayStatus;
  int displayPreviewUpdates = 0;
  ~SettingsScene() override { if (destructions) ++*destructions; }
  void setDisplayStatus(const std::string &status, Color) { displayStatus = status; }
  void updateDisplayPreviewUi() { ++displayPreviewUpdates; }
  void forwardPreviewInputEvent(SDL_Event &) { ++previewEvents; }
  void destroyPreviewInputHandler() { ++previewInputCleanups; }
  void init() override { addView(new View); }
  void update(float) override {}
  void renderScene() override {}
  void cleanupScene() override {}
  EventHandleResult handleEvents(SDL_Event &event) override;
  void openStorage();
};

PRODUCTION_SETTINGS_EVENTS

struct StorageManagementScene : SettingsScene {
  SceneReturnTarget target;
  StorageManagementScene(ApplicationContext &context, SceneReturnTarget value)
      : SettingsScene(context), target(std::move(value)) {}
};
void SettingsScene::openStorage() {
  defer(PRODUCTION_STORAGE_OPEN, 0, true);
}

bool admitMedia(ApplicationContext &context) {
  std::string error;
  bool guard = true;
  PRODUCTION_MEDIA_ADMISSION
}
struct MaintenanceScene : SettingsScene {
  using SettingsScene::SettingsScene;
  bool busy = false;
  int releases = 0;
  bool canPrepareForStorageMaintenance(std::string &error) override {
    if (busy) error = "busy";
    return !busy;
  }
  bool prepareForStorageMaintenance(std::string &error) override {
    if (!canPrepareForStorageMaintenance(error)) return false;
    ++releases;
    return true;
  }
};
void testMaintenanceAdmission() {
  ApplicationContext context;
  SceneManager manager(context);
  auto first = std::make_unique<MaintenanceScene>(context);
  auto second = std::make_unique<MaintenanceScene>(context);
  auto *a = first.get();
  auto *b = second.get();
  manager.changeScene(std::move(first));
  manager.changeScene(std::move(second), true);
  a->busy = true; // retained Settings/library work rejects before any media is stopped
  assert(!admitMedia(context));
  assert(context.musicPlayer.stops == 0 && context.jukebox.stops == 0);
  assert(a->releases == 0 && b->releases == 0);
  // Exercise both traversal orders and direct SceneManager callers too.
  std::string error;
  for (auto *busy : {a, b}) {
    a->busy = busy == a;
    b->busy = busy == b;
    assert(!manager.prepareForStorageMaintenance(error));
    assert(a->releases == 0 && b->releases == 0);
  }
  a->busy = b->busy = false;
  context.jukebox.fail = true;
  assert(!admitMedia(context));
  assert(context.musicPlayer.stops == 0);
  assert(a->releases == 0 && b->releases == 0);
  context.jukebox.fail = false;
  context.jukebox.stops = 0;
  assert(admitMedia(context));
  assert(context.musicPlayer.stops == 1 && context.jukebox.stops == 1);
  assert(a->releases == 1 && b->releases == 1);
}
int main() {
  testMaintenanceAdmission();
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
    event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    manager.handleEvents(event);
    assert(display.focusLosses == 1 && active->displayDraft == 42);
    assert(active->displayStatus == "Display restored");
    assert(active->displayPreviewUpdates == 1);
    assert(active->previewInputCleanups == (previewActive ? 1 : 0));
    assert(active->previewEvents == (previewActive ? 1 : 0));
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    active->views.front()->eventCallback = [&manager] {
      manager.changeScene("Intro");
      return false;
    };
    manager.handleEvents(event);
    assert(manager.currentScene == manager.registeredScenes.at("Intro").get());
  }
  assert(destructions == 2 && "Back deletes each dynamic Settings scene");
  auto settings = std::make_unique<SettingsScene>(context);
  auto *retained = settings.get();
  manager.changeScene(std::move(settings));
  retained->openStorage();
  ++context.currentFrame;
  manager.handleDeferred();
  assert(dynamic_cast<StorageManagementScene *>(manager.currentScene));
  assert(manager.hasBackgroundScene(retained));
  assert(returnToScene(manager, SceneReturnTarget::Retained(retained)));
  manager.handleDeferred();
  assert(manager.currentScene == retained && "Back must not replay the storage-opening callback");
}
