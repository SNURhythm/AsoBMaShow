#include "input/SDLTouchInputSource.h"

#include <iostream>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

namespace rendering {
int render_width = 1000, render_height = 500;
int window_width = 800, window_height = 400;
float ui_scale_x = 1, ui_scale_y = 1;
int ui_offset_x = 100, ui_offset_y = 50;
float widthScale = 1, heightScale = 1;
}

// Event registration belongs to SDL; invoke the real source callback from a
// producer thread without starting a video subsystem in this unit test.
extern "C" void SDLCALL SDL_AddEventWatch(SDL_EventFilter, void *) {}
extern "C" void SDLCALL SDL_DelEventWatch(SDL_EventFilter, void *) {}

struct RecordingHandler : IInputHandler {
  std::vector<int> phases;
  std::vector<SDL_FingerID> fingers;
  std::vector<std::uint64_t> timestamps;
  std::vector<Vector3> locations;
  std::vector<std::thread::id> threads;
  std::function<void()> afterDown;
  void onKeyDown(int, KeySource) override {}
  void onKeyUp(int, KeySource) override {}
  void record(int phase, Vector3 point, SDL_FingerID finger) {
    fingers.push_back(finger);
    timestamps.push_back(touchEventTimestampMicros());
    phases.push_back(phase);
    locations.push_back(point);
    threads.push_back(std::this_thread::get_id());
  }
  void onFingerDown(SDL_FingerID finger, Vector3 point) override {
    record(0, point, finger);
    if (afterDown) afterDown();
  }
  void onFingerMove(SDL_FingerID finger, Vector3 point) override { record(1, point, finger); }
  void onFingerUp(SDL_FingerID finger, Vector3 point) override { record(2, point, finger); }
};

void testSyntheticPointerFiltering() {
  for (bool raw : {false, true}) {
    SDLTouchInputSource source(true);
    RecordingHandler handler;
    source.setHandler(&handler);
    int rawCallbacks = 0;
    if (raw) source.setRawEventCallback([&](const SDL_Event &, std::uint64_t) { ++rawCallbacks; });
    for (const auto type : {SDL_MOUSEBUTTONDOWN, SDL_MOUSEMOTION, SDL_MOUSEBUTTONUP,
                            SDL_FINGERDOWN, SDL_FINGERMOTION, SDL_FINGERUP}) {
      SDL_Event event{};
      event.type = type;
      if (type == SDL_MOUSEMOTION) event.motion.which = SDL_TOUCH_MOUSEID;
      else if (type == SDL_MOUSEBUTTONDOWN || type == SDL_MOUSEBUTTONUP)
        event.button.which = SDL_TOUCH_MOUSEID;
      else event.tfinger.touchId = SDL_MOUSE_TOUCHID;
      SDLTouchInputSource::EventHandler(&source, &event);
    }
    source.pumpPendingEvents();
    if (!handler.phases.empty() || rawCallbacks != 0) {
      throw "synthetic touch/mouse duplicates must reach neither legacy nor raw gameplay";
    }
    source.setRawEventCallback({});
    SDL_Event finger{};
    finger.type = SDL_FINGERDOWN;
    finger.tfinger.fingerId = 0;
    SDLTouchInputSource::EventHandler(&source, &finger);
    SDL_Event mouse{};
    mouse.type = SDL_MOUSEBUTTONDOWN;
    SDLTouchInputSource::EventHandler(&source, &mouse);
    source.pumpPendingEvents();
    if (handler.fingers.size() != 2 || handler.fingers[0] == handler.fingers[1]) {
      throw "real mouse and Android pointer zero must retain separate ownership";
    }
  }
}

void run() {
  SDLTouchInputSource source(true);
  RecordingHandler handler;
  source.setHandler(&handler);
  const auto nowMicros = [] {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  };
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ingressWindows;
  std::thread producer([&] {
    for (const auto type : {SDL_FINGERDOWN, SDL_FINGERMOTION, SDL_FINGERUP}) {
      SDL_Event event{};
      event.type = type;
      event.tfinger.fingerId = 42;
      event.tfinger.x = .25F;
      event.tfinger.y = .5F;
      const auto before = nowMicros();
      SDLTouchInputSource::EventHandler(&source, &event);
      ingressWindows.emplace_back(before, nowMicros());
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  producer.join();
  if (!handler.phases.empty()) {
    throw "Android touch callback must not enter gameplay on producer thread";
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  source.pumpPendingEvents();
  for (std::size_t i = 0; i < ingressWindows.size(); ++i) {
    if (handler.timestamps.at(i) < ingressWindows[i].first ||
        handler.timestamps.at(i) > ingressWindows[i].second) {
      throw "delayed drain must retain each touch's steady-clock ingress time";
    }
  }
  if (handler.touchEventTimestampMicros() != 0) {
    throw "timestamp scope must end after the callback";
  }
  if (handler.phases != std::vector<int>({0, 1, 2}) ||
      handler.threads != std::vector<std::thread::id>(3, std::this_thread::get_id()) ||
      handler.locations.front().x != .1875F || handler.locations.front().y != .5F) {
    throw "main-thread drain must preserve touch order and UI coordinate conversion";
  }
  const auto enqueue = [&](Uint32 type) {
    SDL_Event event{};
    event.type = type;
    event.tfinger.fingerId = 42;
    event.tfinger.x = .25F;
    event.tfinger.y = .5F;
    SDLTouchInputSource::EventHandler(&source, &event);
  };
  handler.phases.clear();
  enqueue(SDL_FINGERDOWN);
  source.discardPendingEvents();
  enqueue(SDL_FINGERMOTION);
  enqueue(SDL_FINGERUP);
  source.pumpPendingEvents();
  if (!handler.phases.empty()) {
    throw "discarded background touch must not reactivate on a stale move";
  }
  source.startListen();
  enqueue(SDL_FINGERDOWN);
  source.stopListen();
  source.pumpPendingEvents();
  if (!handler.phases.empty()) {
    throw "stopListen must discard queued Down before resuming gameplay";
  }
  enqueue(SDL_FINGERDOWN);
  source.pumpPendingEvents();
  for (int index = 0; index < 5000; ++index) enqueue(SDL_FINGERMOTION);
  source.pumpPendingEvents();
  enqueue(SDL_FINGERMOTION);
  enqueue(SDL_FINGERUP);
  source.pumpPendingEvents();
  if (handler.phases != std::vector<int>({0, 2})) {
    throw "overflow must release held input and suppress its orphan motion";
  }
  enqueue(SDL_FINGERDOWN);
  enqueue(SDL_FINGERUP);
  source.pumpPendingEvents();
  if (handler.phases != std::vector<int>({0, 2, 0, 2})) {
    throw "fresh touch must recover after overflow";
  }
  SDLTouchInputSource immediate;
  handler.phases.clear();
  immediate.setHandler(&handler);
  SDL_Event event{};
  event.type = SDL_FINGERDOWN;
  SDLTouchInputSource::EventHandler(&immediate, &event);
  if (handler.phases != std::vector<int>{0}) {
    throw "desktop synchronous SDL input must retain its existing behavior";
  }
  handler.phases.clear();
  handler.afterDown = [&] { source.discardPendingEvents(); };
  enqueue(SDL_FINGERDOWN);
  enqueue(SDL_FINGERUP);
  enqueue(SDL_FINGERDOWN);
  source.pumpPendingEvents();
  if (handler.phases != std::vector<int>{0}) {
    throw "discard during a callback must invalidate the remaining drained batch";
  }
}

int main() {
  try { testSyntheticPointerFiltering(); run(); }
  catch (const char *message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }
  return 0;
}
