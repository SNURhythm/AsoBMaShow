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

#ifndef FIXTURE_IOS
#define FIXTURE_IOS 0
#endif
#if FIXTURE_IOS
#undef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 1
#endif

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
bool iosActive = true;
bool IOSApplicationActive() { return iosActive; }
struct Registry {
  std::optional<std::string> realtimeDisconnectedSdlDevice(const SDL_Event &) { return {}; }
  bool consumedOnce = false;
  bool deviceKnown = true;
  std::size_t translateRealtimeSdlInputs(const SDL_Event &event, std::array<input::PhysicalInputEvent, 4> &out, bool consumeOnce) {
    consumedOnce = consumeOnce;
    if (!deviceKnown) return 0;
    out[0].control.deviceClass = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN
        ? input::DeviceClass::GameController : event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN
        ? input::DeviceClass::Joystick : input::DeviceClass::Keyboard;
    return 1;
  }
};
struct Router {
  int delivered = 0, disconnects = 0, releases = 0;
  bool held = false, enabled = true;
  void consume(const input::PhysicalInputEvent &, std::int64_t) { ++delivered; held = true; }
  void cancelInputs(std::int64_t) { if (held && enabled) ++releases; held = false; }
  void setGameplayEnabled(bool value, std::int64_t) { enabled = value; }
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
  std::array<bool, 6> registryRealtimeClasses{};
  bool registryRealtimeEnabled(input::DeviceClass value) const { return registryRealtimeClasses[static_cast<std::size_t>(value)]; }
  std::uint64_t inputInterruptionGeneration = 0;
  std::atomic_bool inputInterrupted{false};
  std::atomic_bool inputFallbackReady{false};
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
  for (const auto deviceClass : {input::DeviceClass::Keyboard, input::DeviceClass::GameController, input::DeviceClass::Joystick})
    require(session.registryRealtimeEnabled(deviceClass) && configuration.claimedClasses[static_cast<std::size_t>(deviceClass)],
            "claimed SDL physical input must retain deferred registry fallback");
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
  require(router.delivered == 2 && registry.consumedOnce,
          "desktop SDL keyboard fallback did not deliver immediately with acknowledgement");
  for (const auto type : {SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_EVENT_JOYSTICK_BUTTON_DOWN}) {
    event.type = type;
    std::thread producer([&] { configuration.sdlWatch(configuration.sdlWatchContext, &event); });
    producer.join();
  }
  require(router.delivered == 4, "desktop SDL controller fallback waited for the owner");
  session.keyboardTextFocused.store(true);
  event.type = SDL_EVENT_KEY_DOWN;
  configuration.sdlWatch(configuration.sdlWatchContext, &event);
  require(router.delivered == 4, "desktop SDL keyboard ignored published text focus");
  event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
  std::thread focusLoss([&] { configuration.sdlWatch(configuration.sdlWatchContext, &event); });
  focusLoss.join();
  require(session.inputInterruptionGeneration == 1 && session.inputFallbackReady && !router.held && router.releases == 1 && !router.enabled,
          "registered focus loss did not interrupt gameplay during owner stall");
#if FIXTURE_IOS
  require(registry.consumedOnce, "iOS producer input must be acknowledged before owner delivery");
  iosActive = false;
  router.setGameplayEnabled(true, 0);
  router.consume(key, 0);
  const int releasesBeforeBackground = router.releases;
  const auto interruptedBeforeBackground = session.inputInterruptionGeneration;
  event.type = SDL_EVENT_WILL_ENTER_BACKGROUND;
  configuration.sdlWatch(configuration.sdlWatchContext, &event);
  require(session.inputInterruptionGeneration == interruptedBeforeBackground + 1 &&
              router.releases == releasesBeforeBackground + 1 && !router.held && !router.enabled,
          "iOS background lifecycle must cancel physical ownership even after native inactivity");
  const int inactiveDelivered = router.delivered;
  session.keyboardTextFocused.store(false);
  for (const auto deviceClass : {input::DeviceClass::Keyboard, input::DeviceClass::GameController,
                                input::DeviceClass::Joystick, input::DeviceClass::Midi,
                                input::DeviceClass::Gyroscope}) {
    input::PhysicalInputEvent inactive{};
    inactive.control.deviceClass = deviceClass;
    configuration.onInput(inactive);
  }
  require(router.delivered == inactiveDelivered, "inactive iOS registry ingress must gate every claimed class");
  iosActive = true;
#endif
  session.keyboardTextFocused.store(false);
  session.inputInterrupted.store(false);
  router.setGameplayEnabled(true, 0);
  registry.deviceKnown = false;
  event.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
  const int beforeHotplug = router.delivered;
  configuration.sdlWatch(configuration.sdlWatchContext, &event);
  require(router.delivered == beforeHotplug, "unknown controller cannot route before owner opens it");
  registry.deviceKnown = true;
  input::PhysicalInputEvent pad{};
  pad.control.deviceClass = input::DeviceClass::GameController;
  configuration.onInput(pad);
  require(router.delivered == beforeHotplug + 1, "deferred registry delivery lost hotplug input");
#if !FIXTURE_IOS
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
#endif
  std::cout << (FIXTURE_IOS ? "iOS" : "Desktop") << " realtime ingress tests passed\n";
}
