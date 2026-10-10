#include "input/AndroidRawTouchInput.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;
using namespace input::android;

void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
  struct State {
    std::mutex mutex;
    std::condition_variable changed;
    int calls = 0;
    bool block = false, entered = false, release = false;
    RawTouchEvent event;
  } state;
  const RawTouchEvent event{31, TouchPhase::Down, .25F, .75F, 123456};
  RawTouchRegistration::dispatch(event);
  auto registration = std::make_unique<RawTouchRegistration>(
      [](const RawTouchEvent &sample, void *context) {
        auto &state = *static_cast<State *>(context);
        std::unique_lock lock(state.mutex);
        ++state.calls;
        state.event = sample;
        state.entered = true;
        state.changed.notify_all();
        if (state.block) state.changed.wait(lock, [&] { return state.release; });
      }, &state);
  RawTouchRegistration::dispatch(event);
  require(state.calls == 1 && state.event.pointerId == 31 &&
              state.event.phase == TouchPhase::Down && state.event.x == .25F &&
              state.event.y == .75F && state.event.steadyTimestampMicros == 123456,
          "raw touch reaches the active registration with original sample data");
  state.block = true;
  state.entered = false;
  std::thread producer([&] { RawTouchRegistration::dispatch(event); });
  {
    std::unique_lock lock(state.mutex);
    require(state.changed.wait_for(lock, 2s, [&] { return state.entered; }),
            "producer enters callback");
  }
  std::atomic_bool closing{false}, closed{false};
  std::thread closer([&] {
    closing.store(true);
    registration.reset();
    closed.store(true);
  });
  while (!closing.load()) std::this_thread::yield();
  require(!closed.load(), "detachment cannot finish while callback owns session");
  {
    std::lock_guard lock(state.mutex);
    state.release = true;
    state.changed.notify_all();
  }
  producer.join();
  closer.join();
  RawTouchRegistration::dispatch(event);
  require(closed.load() && state.calls == 2,
          "detached callbacks cannot access the old session");
}
