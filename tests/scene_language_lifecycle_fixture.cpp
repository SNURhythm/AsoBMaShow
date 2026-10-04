// Keep production scene ownership/event ordering; substitute only SDL, UI, and
// application services so the regression needs neither graphics nor audio.
#include "i18n/Localization.h"

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

using Uint64 = std::uint64_t;
Uint64 SDL_GetTicks64() { return 1; }
struct SDL_Event {};
class SceneManager;
struct BackgroundTasks {
  bool paused = false;
  void setGameplayPaused(bool value) { paused = value; }
};
class ApplicationContext {
public:
  Uint64 currentFrame = 0;
  int uiBatchRenderer = 0;
  SceneManager *sceneManager = nullptr;
  std::atomic_bool backgroundTasksPausedForForegroundScene = false;
  BackgroundTasks *chartLibraryTasks = nullptr;
  std::function<void()> notifyBackgroundTaskPauseStateChanged;
  std::function<void(bool)> setGameplayOrientationLocked;
  int gameplayBgaCompositeState = 0;
};
struct RenderContext {
  explicit RenderContext(int) {}
  struct UiBatchScope { explicit UiBatchScope(RenderContext &) {} };
};
struct View {
  i18n::Text label = i18n::message("menu.settings.label");
  std::string displayed = label.resolve();
  int languageChanges = 0;
  bool insideCallback = false;
  static inline std::vector<std::function<void()>> deferredCallbacks;
  virtual ~View() = default;
  bool handleEvents(SDL_Event &) { return true; }
  void render(RenderContext &) {}
  void propagateLanguageChange() {
    assert(!insideCallback && "language refresh must not run inside a UI callback");
    ++languageChanges;
    displayed = label.resolve();
  }
  static void dispatchTemporaryEventListeners(SDL_Event &) {}
  static void dispatchDeferredEventCallbacks() {
    auto pending = std::exchange(deferredCallbacks, {});
    for (auto &callback : pending) callback();
  }
};

PRODUCTION_SCENE_HEADER
PRODUCTION_MANAGER_HEADER
PRODUCTION_MANAGER_METHODS

struct ObservedScene final : Scene {
  explicit ObservedScene(ApplicationContext &context, bool tutorial = false)
      : Scene(context), tutorial(tutorial) {}
  bool tutorial = false;
  int initializations = 0;
  int cleanups = 0;
  int resumes = 0;
  int pauses = 0;
  View *label = nullptr;
  std::string unsavedInput;
  float scrollOffset = 0;
  bool playing = false;
  int activeTask = 0;
  std::function<void()> eventCallback;

  void init() override {
    ++initializations;
    unsavedInput.clear();
    scrollOffset = 0;
    playing = false;
    activeTask = 0;
    label = new View;
    addView(label);
  }
  void onPause() override { ++pauses; }
  void onResume() override {
    ++resumes;
    assert(label->displayed == i18n::tr("menu.settings.label") &&
           "retained scene must be translated before onResume observes its UI");
  }
  EventHandleResult handleEvents(SDL_Event &) override {
    label->insideCallback = true;
    if (eventCallback) eventCallback();
    label->insideCallback = false;
    return {};
  }
  void update(float) override {}
  void renderScene() override {}
  void cleanupScene() override {
    ++cleanups;
    unsavedInput.clear();
    scrollOffset = 0;
    playing = false;
    activeTask = 0;
  }
};

void assertRetainedState(const ObservedScene &scene, const View *originalLabel) {
  assert(scene.initializations == 1 && scene.cleanups == 0);
  assert(scene.label == originalLabel);
  assert(scene.unsavedInput == "Settings" && "user text remains raw catalog-like data");
  assert(scene.scrollOffset == 73.5f);
  assert(scene.playing && scene.activeTask == 42);
}

using MainMenuScene = ObservedScene;
struct IntroScene {
  ApplicationContext &context;
  void startTutorial();
};
PRODUCTION_TUTORIAL_LAUNCH

void testTutorialMenuReturnsWithState() {
  ApplicationContext context;
  SceneManager manager(context);
  manager.registerScene("MainMenu", std::make_unique<MainMenuScene>(context));
  IntroScene intro{context};
  for (int visit = 0; visit < 2; ++visit) {
    intro.startTutorial();
    auto *menu = static_cast<MainMenuScene *>(manager.currentScene);
    assert(menu->tutorial);
    menu->unsavedInput = "Settings";
    menu->scrollOffset = 73.5f;
    menu->playing = true;
    menu->activeTask = 42;
    auto *label = menu->label;
    manager.changeScene(std::make_unique<ObservedScene>(context), true);
    assert(manager.hasBackgroundScene(menu));
    manager.changeScene("MainMenu");
    assert(manager.currentScene == menu && "gameplay must return to the tutorial menu instance");
    assertRetainedState(*menu, label);
    assert(menu->resumes == 1);
    // Returning to Intro releases the menu before replaying the tutorial.
    manager.changeScene(std::make_unique<ObservedScene>(context));
  }
}

void testGameplayOrientationFollowsSceneLifetime() {
  ApplicationContext context;
  bool locked = false;
  std::vector<bool> requests;
  context.setGameplayOrientationLocked = [&](bool value) {
    locked = value;
    requests.push_back(value);
  };
  SceneManager manager(context);
  struct Gameplay final : Scene {
    bool &locked;
    bool fail;
    Gameplay(ApplicationContext &context, bool &locked, bool fail = false)
        : Scene(context), locked(locked), fail(fail) {}
    bool locksOrientation() const override { return true; }
    void init() override {
      assert(locked && "lock the current orientation before gameplay loading");
      if (fail) throw 1;
    }
    void update(float) override { assert(locked); }
    void renderScene() override {}
    void cleanupScene() override {}
  };
  manager.changeScene(std::make_unique<ObservedScene>(context));
  manager.changeScene(std::make_unique<Gameplay>(context, locked), true);
  manager.currentScene->onApplicationBackgroundChanged(true);
  manager.currentScene->onApplicationBackgroundChanged(false);
  manager.update(0);
  manager.changeScene(std::make_unique<Gameplay>(context, locked));
  assert(requests == std::vector<bool>{true} && "retry must not recapture device orientation");
  manager.changeScene(std::make_unique<ObservedScene>(context));
  assert(!locked && "results and menus restore the selected orientation mode");
  try {
    manager.changeScene(std::make_unique<Gameplay>(context, locked, true));
    assert(false);
  } catch (int) {}
  assert(!locked && "failed gameplay initialization must release the lock");
  manager.changeScene(std::make_unique<Gameplay>(context, locked));
  manager.cleanup();
  assert(!locked && "shutdown must release the lock");
}

int main() {
  testGameplayOrientationFollowsSceneLifetime();
  testTutorialMenuReturnsWithState();
  i18n::setLanguage(i18n::Language::English);
  ApplicationContext context;
  BackgroundTasks tasks;
  context.chartLibraryTasks = &tasks;
  SceneManager manager(context);
  auto selector = std::make_unique<ObservedScene>(context);
  auto *retained = selector.get();
  manager.registerScene("selector", std::move(selector));
  manager.changeScene("selector");
  retained->unsavedInput = "Settings";
  retained->scrollOffset = 73.5f;
  retained->playing = true;
  retained->activeTask = 42;
  auto *originalLabel = retained->label;
  const auto english = originalLabel->displayed;
  bool deferredRan = false;
  retained->eventCallback = [&] {
    i18n::setLanguage(i18n::Language::Korean);
    assert(originalLabel->displayed == english);
    assert(originalLabel->languageChanges == 0);
    View::deferredCallbacks.push_back([&] {
      assert(!originalLabel->insideCallback);
      assert(originalLabel->displayed == english &&
             "refresh waits for deferred event callbacks to finish");
      deferredRan = true;
    });
  };
  SDL_Event event;
  manager.handleEvents(event);
  retained->eventCallback = {};
  assert(deferredRan);
  assert(originalLabel->languageChanges == 1);
  assert(originalLabel->displayed == i18n::tr("menu.settings.label"));
  assert(originalLabel->displayed != english);
  assertRetainedState(*retained, originalLabel);
  manager.update(0);
  manager.handleEvents(event);
  assert(originalLabel->languageChanges == 1 && "unchanged revision refreshes once");

  auto settings = std::make_unique<ObservedScene>(context);
  auto *covering = settings.get();
  manager.changeScene(std::move(settings), true);
  assert(manager.hasBackgroundScene(retained));
  covering->eventCallback = [&] { i18n::setLanguage(i18n::Language::Japanese); };
  manager.handleEvents(event);
  assert(covering->label->languageChanges == 1);
  assert(originalLabel->languageChanges == 1 && "background UI refresh is deferred until return");
  assertRetainedState(*retained, originalLabel);

  manager.changeScene("selector");
  assert(retained->resumes == 1 && retained->pauses == 1);
  assert(originalLabel->languageChanges == 2);
  assert(originalLabel->displayed == i18n::tr("menu.settings.label"));
  assertRetainedState(*retained, originalLabel);
  assert(!tasks.paused);
  manager.update(0);
  manager.handleEvents(event);
  assert(originalLabel->languageChanges == 2);

  // Language changes outside event delivery are picked up at the next frame.
  i18n::setLanguage(i18n::Language::English);
  manager.update(0);
  assert(originalLabel->languageChanges == 3 && originalLabel->displayed == english);
  assertRetainedState(*retained, originalLabel);
}
