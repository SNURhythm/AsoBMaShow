#include "targets.h"
#include "input/InputTypes.h"
#include "input/InputTimestamp.h"
#include "input/SDLTouchInputSource.h"
#include "scene/play/RealtimeGameplayInputRegistration.h"
#include <limits>
#include <memory>
#include <mutex>
#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace rendering {
int render_width = 1000, render_height = 500;
int window_width = 1000, window_height = 500;
float ui_scale_x = 1, ui_scale_y = 1;
int ui_offset_x = 0, ui_offset_y = 0;
float widthScale = 1, heightScale = 1;
}
namespace {
void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
struct Presentation { virtual ~Presentation() = default; };
struct PlayfieldPresentationCoordinator : Presentation {
  bool focused = true;
  bool hasFocusedTextInput() const { return focused; }
};
struct Scene { Presentation *presentation = nullptr; struct { std::atomic_bool appInBackground{false}; } context; };
// Record delivery at the router boundary using the production registration.
struct Registry {
  std::optional<std::string> realtimeDisconnectedSdlDevice(const SDL_Event &) { return {}; }
  std::size_t translateRealtimeSdlInputs(const SDL_Event &, std::array<input::PhysicalInputEvent, 4> &out, bool) {
    out[0].control.deviceClass = input::DeviceClass::Keyboard;
    return 1;
  }
};
struct Router {
  int delivered = 0, disconnects = 0;
  void consume(const input::PhysicalInputEvent &, std::int64_t) { ++delivered; }
  void disconnectDevice(const std::string &, std::int64_t) { ++disconnects; }
};
std::int64_t nowMicros() { return 123; }
struct RealtimeGameplaySession {
  std::atomic_bool acceptingNativeInput{true};
  std::atomic_bool keyboardTextFocused{false};
  Scene *scene;
  Registry *inputRegistry;
  Router *physicalInputRouter;
  std::mutex inputInterruptionMutex;
  bool registryRealtimeEnabled(input::DeviceClass) const { return true; }
  int interruptions = 0;
  bool fallbackReady = false;
  void interruptInput(const input::InputInterruption &event) {
    if (event.fallbackReady) fallbackReady = true;
    else ++interruptions;
  }
#define INGRESS_METHODS
#include "desktop_realtime_ingress_methods.h"
#undef INGRESS_METHODS
};
struct InputHandlerStub {
  void setRegistryDeviceClassEnabled(input::DeviceClass, bool) {}
};
struct RegistrationFixture {
  RealtimeGameplaySession &activeSession;
  InputHandlerStub *inputHandler = nullptr;
  gameplay::RealtimeGameplayInputRegistration::Configuration configuration() {
    gameplay::RealtimeGameplayInputRegistration::DeviceClasses claimedClasses{};
#define INGRESS_CONFIGURATION
#include "desktop_realtime_ingress_methods.h"
#undef INGRESS_CONFIGURATION
  }
};
struct RhythmInputHandler : IInputHandler {
  std::unique_ptr<SDLTouchInputSource> touchInputSource;
  SDLTouchInputSource::RawEventCallback touchIngressCallback;
  std::vector<std::thread::id> deliveries;
  bool startListenTouch();
  void pumpPendingTouchEvents() { touchInputSource->pumpPendingEvents(); }
  void onKeyDown(int, KeySource) override {}
  void onKeyUp(int, KeySource) override {}
  void onFingerDown(SDL_FingerID, Vector3) override { deliveries.push_back(std::this_thread::get_id()); }
  void onFingerUp(SDL_FingerID, Vector3) override { deliveries.push_back(std::this_thread::get_id()); }
  void onFingerMove(SDL_FingerID, Vector3) override {}
};
#define POINTER_METHODS
#include "desktop_realtime_ingress_methods.h"
#undef POINTER_METHODS
void drainPointer(RhythmInputHandler *inputHandler, bool realtimeAtFrameStart) {
#define POINTER_DRAIN
#include "desktop_realtime_ingress_methods.h"
#undef POINTER_DRAIN
}
}
int main() {
  PlayfieldPresentationCoordinator presentation;
  Scene scene{&presentation};
  Registry registry;
  Router router;
  RealtimeGameplaySession session{true, false, &scene, &registry, &router};
  session.publishKeyboardTextFocus();
  // The native callback must use its published snapshot even if presentation
  // has since changed on the scene owner. It must never dereference scene here.
  scene.presentation = nullptr;
  auto configuration = RegistrationFixture{session}.configuration();
  input::PhysicalInputEvent key{};
  key.control.deviceClass = input::DeviceClass::Keyboard;
  std::thread callback([&] { configuration.onInput(key); });
  callback.join();
  require(router.delivered == 0, "registered keyboard ingress ignored published text focus");
  session.publishKeyboardTextFocus();
  std::thread next([&] { configuration.onInput(key); });
  next.join();
  require(router.delivered == 1, "registered unfocused keyboard input was suppressed");
  const int previousDisconnects = router.disconnects;
  scene.presentation = &presentation;
  session.publishKeyboardTextFocus();
  require(router.disconnects == previousDisconnects + 1,
          "entering text editing retained a held gameplay keyboard binding");
  scene.presentation = nullptr;
  session.publishKeyboardTextFocus();
  require(configuration.sdlWatch != nullptr, "desktop lifecycle watcher was not registered");
  SDL_Event event{}; event.type = SDL_EVENT_KEY_DOWN;
  configuration.sdlWatch(configuration.sdlWatchContext, &event);
  require(router.delivered == 1, "desktop watcher duplicated registry keyboard delivery");
  event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
  std::thread focusLoss([&] { configuration.sdlWatch(configuration.sdlWatchContext, &event); });
  focusLoss.join();
  require(session.interruptions == 1 && session.fallbackReady,
          "registered focus loss did not interrupt gameplay during owner stall");
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
  {
    RhythmInputHandler pointer;
    require(pointer.startListenTouch(), "pointer source failed to register");
    std::thread producer([&] {
      SDL_Event mouse{};
      mouse.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
      mouse.button.button = SDL_BUTTON_LEFT;
      mouse.button.x = 250; mouse.button.y = 250;
      SDL_PushEvent(&mouse);
      mouse.type = SDL_EVENT_MOUSE_BUTTON_UP;
      SDL_PushEvent(&mouse);
    });
    producer.join();
    require(pointer.deliveries.empty(), "desktop pointer callback entered scene on event producer");
    drainPointer(&pointer, true);
    require(pointer.deliveries == std::vector<std::thread::id>(2, std::this_thread::get_id()),
            "desktop realtime gameplay failed to drain pointer events on application owner");
  }
  SDL_Quit();
  std::cout << "Desktop realtime ingress tests passed\n";
}
