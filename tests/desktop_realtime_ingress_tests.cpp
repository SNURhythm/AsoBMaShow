#include "targets.h"
#include "input/InputTypes.h"
#include "input/InputTimestamp.h"
#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace {
void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
struct Presentation { virtual ~Presentation() = default; };
struct PlayfieldPresentationCoordinator : Presentation {
  bool focused = true;
  bool hasFocusedTextInput() const { return focused; }
};
struct Scene { Presentation *presentation = nullptr; };
// Stand in for device decoding and the downstream router; only the production
// watch decides whether typing may cross that boundary into gameplay.
struct Registry {
  std::optional<std::string> realtimeDisconnectedSdlDevice(const SDL_Event &) { return {}; }
  std::size_t translateRealtimeSdlInputs(const SDL_Event &, std::array<input::PhysicalInputEvent, 4> &out, bool) {
    out[0].control.deviceClass = input::DeviceClass::Keyboard;
    return 1;
  }
};
struct Router {
  int delivered = 0;
  void consume(const input::PhysicalInputEvent &, std::int64_t) { ++delivered; }
  void disconnectDevice(const std::string &, std::int64_t) {}
};
std::int64_t nowMicros() { return 123; }
struct RealtimeGameplaySession {
  std::atomic_bool acceptingNativeInput{true};
  std::atomic_bool keyboardTextFocused{false};
  Scene *scene;
  Registry *inputRegistry;
  Router *physicalInputRouter;
  int interruptions = 0;
  bool fallbackReady = false;
  void interruptInput(const input::InputInterruption &event) {
    if (event.fallbackReady) fallbackReady = true;
    else ++interruptions;
  }
#include "desktop_realtime_ingress_methods.h"
};
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
  SDL_Event event{}; event.type = SDL_EVENT_KEY_DOWN;
  std::thread callback([&] { RealtimeGameplaySession::sdlInputWatch(&session, &event); });
  callback.join();
  require(router.delivered == 0, "typing reached gameplay instead of using published text-focus state");
  session.publishKeyboardTextFocus();
  std::thread next([&] { RealtimeGameplaySession::sdlInputWatch(&session, &event); });
  next.join();
  require(router.delivered == 1, "unfocused keyboard input was suppressed");
  event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
  std::thread focusLoss([&] { RealtimeGameplaySession::sdlInputWatch(&session, &event); });
  focusLoss.join();
  require(session.interruptions == 1 && session.fallbackReady,
          "focus loss during a render stall did not interrupt realtime gameplay safely");
  std::cout << "Desktop realtime ingress tests passed\n";
}
