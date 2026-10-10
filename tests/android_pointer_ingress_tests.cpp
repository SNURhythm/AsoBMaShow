#include "input/AndroidInputHints.h"

#include <SDL3/SDL.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

// Exercise SDL's platform ingress, not SDL_PushEvent: synthesis happens before
// the original event reaches the event watch. Link the static SDL test target.
extern "C" int SDL_AddTouch(SDL_TouchID, SDL_TouchDeviceType, const char *);
extern "C" void SDL_SendTouch(Uint64, SDL_TouchID, SDL_FingerID, SDL_Window *,
                              SDL_EventType, float, float, float);
extern "C" void SDL_SendTouchMotion(Uint64, SDL_TouchID, SDL_FingerID,
                                    SDL_Window *, float, float, float);
extern "C" void SDL_SendMouseMotion(Uint64, SDL_Window *, SDL_MouseID, bool,
                                    float, float);
extern "C" void SDL_SendMouseButton(Uint64, SDL_Window *, SDL_MouseID, Uint8, bool);

namespace {
using namespace std::chrono_literals;

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

struct QueueHold {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool release = false;
  SDL_EventType expected;
  SDL_Event observed{};
  int realEvents = 0;
  int syntheticEvents = 0;
};

bool SDLCALL holdQueuedEvent(void *context, SDL_Event *event) {
  if (event->type == SDL_EVENT_USER) {
    auto &state = *static_cast<QueueHold *>(context);
    std::unique_lock lock(state.mutex);
    state.entered = true;
    state.changed.notify_all();
    state.changed.wait(lock, [&] { return state.release; });
  }
  return true;
}

bool SDLCALL observePointer(void *context, SDL_Event *event) {
  auto &state = *static_cast<QueueHold *>(context);
  const bool touch = event->type == SDL_EVENT_FINGER_DOWN ||
      event->type == SDL_EVENT_FINGER_MOTION || event->type == SDL_EVENT_FINGER_UP;
  const bool mouse = event->type == SDL_EVENT_MOUSE_MOTION ||
      event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP;
  if (!touch && !mouse) return true;
  const bool synthetic = touch ? event->tfinger.touchID == SDL_MOUSE_TOUCHID
      : event->type == SDL_EVENT_MOUSE_MOTION ? event->motion.which == SDL_TOUCH_MOUSEID
                                            : event->button.which == SDL_TOUCH_MOUSEID;
  std::lock_guard lock(state.mutex);
  if (synthetic) {
    ++state.syntheticEvents;
  } else if (event->type == state.expected) {
    state.observed = *event;
    ++state.realEvents;
  }
  state.changed.notify_all();
  return true;
}

SDL_Event deliverWhileQueueHeld(SDL_EventType type, const std::function<void()> &deliver) {
  SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
  SDL_Event seed{};
  seed.type = SDL_EVENT_USER;
  require(SDL_PushEvent(&seed), "the queue-lock fixture is enqueued");
  QueueHold state{.expected = type};
  require(SDL_AddEventWatch(observePointer, &state), "the real input watcher registers");
  std::thread queueOwner([&] { SDL_FilterEvents(holdQueuedEvent, &state); });
  {
    std::unique_lock lock(state.mutex);
    state.changed.wait(lock, [&] { return state.entered; });
  }
  std::thread nativeProducer(deliver);
  bool observedBeforeQueueRelease;
  {
    std::unique_lock lock(state.mutex);
    // The condition proves the original watch runs while the unrelated queue
    // owner is blocked; this is not a latency threshold or a render-loop test.
    observedBeforeQueueRelease = state.changed.wait_for(lock, 2s, [&] {
      return state.realEvents != 0;
    });
    state.release = true;
    state.changed.notify_all();
  }
  queueOwner.join();
  nativeProducer.join();
  SDL_RemoveEventWatch(observePointer, &state);
  require(observedBeforeQueueRelease,
          "real pointer ingress must reach its watch before an unrelated event queue unlocks");
  require(state.realEvents == 1 && state.syntheticEvents == 0,
          "one original pointer sample reaches the watch without synthetic duplicates");
  return state.observed;
}

void testTouchAndMouseRemainIndependent(SDL_Window *window) {
  require(SDL_AddTouch(123, SDL_TOUCH_DEVICE_DIRECT, "Android touch ingress test") >= 0,
          "a real touch device registers");
  // Establish focus and a mouse input source before blocking the event queue.
  SDL_SendMouseMotion(1, window, 77, false, 10, 10);
  SDL_SendMouseButton(2, window, 77, SDL_BUTTON_LEFT, true);
  SDL_SendMouseButton(3, window, 77, SDL_BUTTON_LEFT, false);

  const auto down = deliverWhileQueueHeld(SDL_EVENT_FINGER_DOWN, [&] {
    SDL_SendTouch(1'000'000, 123, 42, window, SDL_EVENT_FINGER_DOWN, .25F, .25F, .5F);
  });
  const auto move = deliverWhileQueueHeld(SDL_EVENT_FINGER_MOTION, [&] {
    SDL_SendTouchMotion(2'000'000, 123, 42, window, .75F, .5F, .5F);
  });
  const auto up = deliverWhileQueueHeld(SDL_EVENT_FINGER_UP, [&] {
    SDL_SendTouch(3'000'000, 123, 42, window, SDL_EVENT_FINGER_UP, .75F, .5F, 0);
  });
  require(down.tfinger.touchID == 123 && move.tfinger.touchID == 123 && up.tfinger.touchID == 123 &&
              down.tfinger.fingerID == 42 && move.tfinger.fingerID == 42 && up.tfinger.fingerID == 42,
          "balanced touch transitions retain their physical contact identity");
  require(down.common.timestamp == 1'000'000 && move.common.timestamp == 2'000'000 &&
              up.common.timestamp == 3'000'000 && down.tfinger.x == .25F &&
              move.tfinger.x == .75F && up.tfinger.x == .75F,
          "touch timing and final coordinates survive native ingress");

  const auto mouseDown = deliverWhileQueueHeld(SDL_EVENT_MOUSE_BUTTON_DOWN, [&] {
    SDL_SendMouseButton(4'000'000, window, 77, SDL_BUTTON_LEFT, true);
  });
  const auto mouseMove = deliverWhileQueueHeld(SDL_EVENT_MOUSE_MOTION, [&] {
    SDL_SendMouseMotion(5'000'000, window, 77, false, 30, 40);
  });
  const auto mouseUp = deliverWhileQueueHeld(SDL_EVENT_MOUSE_BUTTON_UP, [&] {
    SDL_SendMouseButton(6'000'000, window, 77, SDL_BUTTON_LEFT, false);
  });
  // SDL absolute mouse input intentionally uses global mouse ID zero.
  require(mouseDown.button.which == 0 && mouseMove.motion.which == 0 && mouseUp.button.which == 0 &&
              mouseDown.button.down && !mouseUp.button.down &&
              mouseDown.button.button == SDL_BUTTON_LEFT && mouseUp.button.button == SDL_BUTTON_LEFT,
          "real mouse press/move/release retain SDL's mouse source and balanced button state");
  require(mouseDown.common.timestamp == 4'000'000 && mouseMove.common.timestamp == 5'000'000 &&
              mouseUp.common.timestamp == 6'000'000 && mouseMove.motion.x == 30 && mouseMove.motion.y == 40,
          "real mouse timing and coordinates remain available without synthesized touch");
}
} // namespace

int main() {
  try {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    // Reproduce Android's synthesis defaults on every desktop test platform.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");
    input::android::configurePointerHints();
    require(SDL_Init(SDL_INIT_VIDEO), "SDL dummy video initializes");
    SDL_Window *window = SDL_CreateWindow("Android pointer ingress", 100, 100, SDL_WINDOW_HIDDEN);
    require(window != nullptr, "SDL dummy window initializes");
    testTouchAndMouseRemainIndependent(window);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    SDL_Quit();
    return 1;
  }
}
