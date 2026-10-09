#include "REPOSITORY_ROOT/src/scene/SceneReturnTarget.h"
#include "REPOSITORY_ROOT/src/skin/SkinPresentationTypes.h"

#include <cassert>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

enum {
  SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP, SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_EVENT_GAMEPAD_BUTTON_UP,
  SDL_EVENT_MOUSE_BUTTON_UP, SDLK_RETURN, SDLK_KP_ENTER, SDLK_6, SDLK_ESCAPE, SDLK_DOWN,
  SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_START,
  SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
  SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_MOTION,
  SDL_TOUCH_MOUSEID, SDL_MOUSE_TOUCHID
};
struct SDL_Event {
  int type = SDL_EVENT_KEY_DOWN;
  struct { int repeat = 0; int key = 0; } key;
  struct { int button = 0; } gbutton;
  struct { int which = 0, x = 0, y = 0; } motion, button;
  struct { int touchID = 0; float x = 0, y = 0; } tfinger;
};
namespace rendering {
constexpr float widthScale = 1.0F, heightScale = 1.0F;
void screenToUi(float x, float y, float &outX, float &outY) {
  outX = x;
  outY = y;
}
void normalizedToUi(float x, float y, float &outX, float &outY) {
  screenToUi(x * 1920.0F, y * 1080.0F, outX, outY);
}
}
struct EventHandleResult { bool quit = false; };
struct View {
  bool visible = true;
  bool editing = true;
  int events = 0;
  void setVisible(bool value) { visible = value; }
  void endEditing() { editing = false; }
  bool handleEvents(SDL_Event &) { ++events; return false; }
};
class Scene {
public:
  View staleModal;
  EventHandleResult handleEvents(SDL_Event &event) {
    staleModal.handleEvents(event);
    return {};
  }
};
namespace skin {
struct SkinDiagnostic { std::string code, message; };
}
enum class SettingsDestination { Profile };
struct SettingsScene {
  SceneReturnTarget target;
  SettingsScene(auto &, SettingsDestination, SceneReturnTarget value)
      : target(std::move(value)) {}
};
struct SceneManager {
  std::thread::id uiThread = std::this_thread::get_id();
  int transitions = 0;
  bool retained = false;
  std::string destination;
  SceneReturnTarget settingsReturn;
  void changeScene(const std::string &name) {
    assert(std::this_thread::get_id() == uiThread);
    ++transitions;
    destination = name;
  }
  void changeScene(std::unique_ptr<SettingsScene> settings, bool retain = false) {
    assert(std::this_thread::get_id() == uiThread);
    ++transitions;
    destination = "Settings";
    retained = retain;
    settingsReturn = settings->target;
  }
};
struct PreviewAudio {
  bool silenced = false;
  void silence() { silenced = true; }
};
struct MusicSelectScene : Scene {
  void closeRankings() {}

  struct RevealMenu { void dismiss() {} };
  std::unique_ptr<RevealMenu> revealContextMenu_;

  struct UnzipModal {
    bool cancelled = false;
    void cancelAndWait() { cancelled = true; }
  } unzipModal;
  UnzipModal *archiveUnzipModal_ = &unzipModal;
  UnzipModal *findBmsModal_ = nullptr;
  SceneManager manager;
  struct { SceneManager *sceneManager; } context{&manager};
  bool failed_ = false;
  bool reactivateSkinOnResume_ = false;
  bool listening = true;
  bool directoryPending = true;
  int normalEvents = 0;
  int errorViews = 0;
  View searchInput, searchOverlay, toolbar, errorRoot, skinTextInput, skinLoading;
  View *searchInput_ = &searchInput;
  View *searchOverlay_ = &searchOverlay;
  View *toolbar_ = &toolbar;
  View *errorView_ = nullptr;
  View *skinTextInput_ = &skinTextInput;
  View *skinLoadingView_ = &skinLoading;
  struct { void cancel() {} } skinTouchGesture_;
  struct SkinSession {
    std::optional<UiLogicalPoint> pointer;
    void setPointerPosition(UiLogicalPoint point) { pointer = point; }
  };
  std::unique_ptr<SkinSession> skinSession_ = std::make_unique<SkinSession>();
  std::optional<UiLogicalPoint> skinPointerUiPosition_;
  std::unique_ptr<int> previewController_ = std::make_unique<int>(1);
  std::unique_ptr<PreviewAudio> previewAudio_ = std::make_unique<PreviewAudio>();
  std::vector<skin::SkinDiagnostic> diagnostics_;
  void cancelDirectoryLoad() { directoryPending = false; }
  void stopInputListening() { listening = false; }
  void buildErrorView() { ++errorViews; errorView_ = &errorRoot; }
  void enterError(std::vector<skin::SkinDiagnostic> diagnostics);
  void openSettings();
  void refreshToolbarAvailability() {}
  EventHandleResult handleEvents(SDL_Event &event) {
    ERROR_EVENT_PREFIX
    ++normalEvents;
    return {};
  }
};

SCENE_METHODS

SDL_Event keyEvent(int key, int type = SDL_EVENT_KEY_DOWN, int repeat = 0) {
  SDL_Event event;
  event.type = type;
  event.key.key = key;
  event.key.repeat = repeat;
  return event;
}
SDL_Event controllerEvent(int button, int type = SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
  SDL_Event event;
  event.type = type;
  event.gbutton.button = button;
  return event;
}

void enterFailure(MusicSelectScene &scene) {
  scene.enterError({{.code = "fixture.failure", .message = "Skin failed"}});
  assert(scene.failed_ && !scene.listening && !scene.directoryPending);
  assert(scene.unzipModal.cancelled && "skin error must stop archive work before hiding its controls");
  assert(scene.previewAudio_->silenced && !scene.previewController_);
  assert(!scene.searchInput.editing && !scene.searchOverlay.visible);
  assert(!scene.toolbar.visible && scene.errorView_ != nullptr);
  scene.enterError({});
  assert(scene.errorViews == 1 && scene.diagnostics_.front().code == "fixture.failure");
}

void testSettingsRecovery() {
  for (auto event : {keyEvent(SDLK_RETURN), keyEvent(SDLK_KP_ENTER),
                     keyEvent(SDLK_6), controllerEvent(SDL_GAMEPAD_BUTTON_SOUTH),
                     controllerEvent(SDL_GAMEPAD_BUTTON_START)}) {
    MusicSelectScene scene;
    enterFailure(scene);
    scene.handleEvents(event);
    assert(scene.manager.destination == "Settings" &&
           "runtime skin errors must allow keyboard/controller Settings recovery");
    assert(scene.manager.transitions == 1 && !scene.manager.retained);
    assert(scene.manager.settingsReturn.kind == SceneReturnTarget::Kind::Registered);
    assert(scene.manager.settingsReturn.registeredName == "Intro" &&
           "error recovery must return to Intro, not retain the failed selector");
    assert(!scene.reactivateSkinOnResume_ && scene.normalEvents == 0);
    assert(!scene.listening && scene.previewAudio_->silenced);
  }
}

void testBackRecovery() {
  for (auto event : {keyEvent(SDLK_ESCAPE), controllerEvent(SDL_GAMEPAD_BUTTON_EAST),
                     controllerEvent(SDL_GAMEPAD_BUTTON_BACK)}) {
    MusicSelectScene scene;
    enterFailure(scene);
    scene.handleEvents(event);
    assert(scene.manager.destination == "Intro" && scene.manager.transitions == 1 &&
           "Escape/controller cancel must leave the failed selector for Intro");
    assert(scene.normalEvents == 0 && !scene.listening);
  }
}

void testErrorModalIsolation() {
  MusicSelectScene scene;
  enterFailure(scene);
  for (auto event : {keyEvent(SDLK_RETURN, SDL_EVENT_KEY_DOWN, 1),
                     keyEvent(SDLK_ESCAPE, SDL_EVENT_KEY_DOWN, 1),
                     keyEvent(SDLK_6, SDL_EVENT_KEY_UP), keyEvent(SDLK_DOWN),
                     controllerEvent(SDL_GAMEPAD_BUTTON_SOUTH, SDL_EVENT_GAMEPAD_BUTTON_UP),
                     controllerEvent(SDL_GAMEPAD_BUTTON_DPAD_DOWN)}) {
    scene.handleEvents(event);
  }
  assert(scene.manager.transitions == 0 && scene.normalEvents == 0);
  SDL_Event pointer;
  pointer.type = SDL_EVENT_MOUSE_BUTTON_UP;
  const auto before = scene.errorRoot.events;
  scene.handleEvents(pointer);
  assert(scene.errorRoot.events == before + 1 && scene.staleModal.events == 0 &&
         "error pointer recovery must bypass stale selector modals");
}

void testHealthySettingsRetainsSelector() {
  MusicSelectScene scene;
  scene.openSettings();
  assert(scene.manager.destination == "Settings" && scene.manager.retained);
  assert(scene.manager.settingsReturn.kind == SceneReturnTarget::Kind::Retained);
  assert(scene.manager.settingsReturn.retained == &scene && scene.reactivateSkinOnResume_);
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::string test = argv[1];
  if (test == "testSettingsRecovery") testSettingsRecovery();
  else if (test == "testBackRecovery") testBackRecovery();
  else if (test == "testErrorModalIsolation") testErrorModalIsolation();
  else if (test == "testHealthySettingsRetainsSelector") testHealthySettingsRetainsSelector();
  else return 2;
}
