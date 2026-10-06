#include "input/SDLTouchInputSource.h"

#include <iostream>
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
  std::vector<Vector3> locations;
  std::vector<std::thread::id> threads;
  std::function<void()> afterDown;
  void onKeyDown(int, KeySource) override {}
  void onKeyUp(int, KeySource) override {}
  void record(int phase, Vector3 point) {
    phases.push_back(phase);
    locations.push_back(point);
    threads.push_back(std::this_thread::get_id());
  }
  void onFingerDown(SDL_FingerID, Vector3 point) override {
    record(0, point);
    if (afterDown) afterDown();
  }
  void onFingerMove(SDL_FingerID, Vector3 point) override { record(1, point); }
  void onFingerUp(SDL_FingerID, Vector3 point) override { record(2, point); }
};

void run() {
  SDLTouchInputSource source(true);
  RecordingHandler handler;
  source.setHandler(&handler);
  std::thread producer([&] {
    for (const auto type : {SDL_FINGERDOWN, SDL_FINGERMOTION, SDL_FINGERUP}) {
      SDL_Event event{};
      event.type = type;
      event.tfinger.fingerId = 42;
      event.tfinger.x = .25F;
      event.tfinger.y = .5F;
      SDLTouchInputSource::EventHandler(&source, &event);
    }
  });
  producer.join();
  if (!handler.phases.empty()) {
    throw "Android touch callback must not enter gameplay on producer thread";
  }
  source.pumpPendingEvents();
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
  try { run(); }
  catch (const char *message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }
  return 0;
}
