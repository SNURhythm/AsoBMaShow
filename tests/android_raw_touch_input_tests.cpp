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

void testUiForwardingPreservesEpochAndOrder() {
  int calls = 0;
  auto registration = std::make_unique<RawTouchRegistration>(
      [](const RawTouchEvent &, void *context) { ++*static_cast<int *>(context); }, &calls);
  const auto epoch = registration->epoch();
  require(epoch != 0 && RawTouchRegistration::activeEpoch() == epoch,
          "registration publishes a nonzero gesture ownership epoch");
  RawTouchEvent touch{1, TouchPhase::Down, .25F, .75F, 123456};
  RawTouchRegistration::dispatch(epoch, touch);
  UiTouchEvent ui;
  require(calls == 1 && RawTouchRegistration::pollUiEvent(ui) && ui.epoch == epoch &&
              ui.touch.pointerId == 1 && ui.touch.phase == TouchPhase::Down &&
              ui.touch.x == .25F && ui.touch.steadyTimestampMicros == 123456,
          "native gameplay and deferred UI receive the original touch independently");
  touch.phase = TouchPhase::Move;
  touch.x = .50F;
  RawTouchRegistration::dispatch(epoch, touch);
  touch.phase = TouchPhase::Up;
  RawTouchRegistration::dispatch(epoch, touch);
  require(RawTouchRegistration::pollUiEvent(ui) && ui.touch.phase == TouchPhase::Move &&
              ui.touch.x == .50F && RawTouchRegistration::pollUiEvent(ui) &&
              ui.touch.phase == TouchPhase::Up && !RawTouchRegistration::pollUiEvent(ui),
          "deferred UI retains movement before its terminal edge");
  touch.phase = TouchPhase::Down;
  RawTouchRegistration::dispatch(epoch, touch);
  registration.reset();
  require(!RawTouchRegistration::isCurrentEpoch(epoch) && !RawTouchRegistration::pollUiEvent(ui),
          "detachment drops queued UI before its scene disappears");
  RawTouchRegistration replacement(
      [](const RawTouchEvent &, void *context) { ++*static_cast<int *>(context); }, &calls);
  const int previousCalls = calls;
  require(replacement.epoch() != epoch, "a retry receives a distinct gesture epoch");
  RawTouchRegistration::dispatch(epoch, touch);
  touch.phase = TouchPhase::Up;
  RawTouchRegistration::dispatch(epoch, touch);
  require(calls == previousCalls && !RawTouchRegistration::pollUiEvent(ui),
          "old held gestures cannot reach a replacement gameplay or UI session");
}

void testUiOverflowCancelsAndRequiresFreshContact() {
  int calls = 0;
  RawTouchRegistration registration(
      [](const RawTouchEvent &, void *context) { ++*static_cast<int *>(context); }, &calls);
  const auto epoch = registration.epoch();
  RawTouchEvent touch{2, TouchPhase::Down, .25F, .75F, 1000};
  RawTouchRegistration::dispatch(epoch, touch);
  UiTouchEvent ui;
  require(RawTouchRegistration::pollUiEvent(ui), "overflow fixture delivers its initial UI down");
  touch.phase = TouchPhase::Move;
  for (std::size_t i = 0; i <= kUiTouchQueueCapacity; ++i) {
    touch.steadyTimestampMicros++;
    RawTouchRegistration::dispatch(epoch, touch);
  }
  require(calls == static_cast<int>(kUiTouchQueueCapacity + 2),
          "UI saturation never delays or drops native gameplay callbacks");
  require(RawTouchRegistration::pollUiEvent(ui) && ui.epoch == epoch &&
              ui.touch.pointerId == 2 && ui.touch.phase == TouchPhase::Cancel &&
              !RawTouchRegistration::pollUiEvent(ui),
          "overflow replaces the incomplete UI backlog with terminal cancellation");
  RawTouchRegistration::dispatch(epoch, touch);
  touch.phase = TouchPhase::Up;
  RawTouchRegistration::dispatch(epoch, touch);
  require(!RawTouchRegistration::pollUiEvent(ui),
          "a cancelled UI gesture stays suppressed through its physical lift");
  touch.phase = TouchPhase::Down;
  RawTouchRegistration::dispatch(epoch, touch);
  require(RawTouchRegistration::pollUiEvent(ui) && ui.touch.phase == TouchPhase::Down,
          "a fresh contact works after UI overflow recovery");
  const auto cancelled = registration.cancelUiTouches();
  require(cancelled.size == 1 && cancelled.events[0].pointerId == 2 &&
              cancelled.events[0].phase == TouchPhase::Cancel &&
              registration.cancelUiTouches().size == 0,
          "teardown retrieves each delivered UI capture once without dispatching callbacks");
  touch.pointerId = 3;
  RawTouchRegistration::dispatch(epoch, touch);
  require(RawTouchRegistration::pollUiEvent(ui) && ui.touch.pointerId == 3 &&
              ui.touch.phase == TouchPhase::Down,
          "fresh paused-menu touches work while an older contact remains suppressed");
  touch.pointerId = 2;
  touch.phase = TouchPhase::Up;
  RawTouchRegistration::dispatch(epoch, touch);
  require(!RawTouchRegistration::pollUiEvent(ui),
          "lifting the pre-pause contact cannot click the paused menu");
}

void testUiBoundaryDiscardsUndeliveredContacts() {
  RawTouchRegistration registration(nullptr, nullptr);
  const auto epoch = registration.epoch();
  require(registration.cancelUiTouches().size == 0, "background entry begins without captures");
  RawTouchEvent touch{1, TouchPhase::Down, .25F, .75F, 1000};
  RawTouchRegistration::dispatch(epoch, touch);
  require(registration.cancelUiTouches().size == 0,
          "foreground cancellation drops a background Down before it acquired any UI view");
  UiTouchEvent ui;
  require(!RawTouchRegistration::pollUiEvent(ui),
          "a background Down cannot appear on the resumed UI");
  touch.phase = TouchPhase::Up;
  RawTouchRegistration::dispatch(epoch, touch);
  require(!RawTouchRegistration::pollUiEvent(ui),
          "the background contact remains suppressed through its release");
  touch.phase = TouchPhase::Down;
  RawTouchRegistration::dispatch(epoch, touch);
  require(RawTouchRegistration::pollUiEvent(ui) && ui.touch.phase == TouchPhase::Down,
          "a fresh foreground touch retains the current registration epoch");
}

void testUiOverflowCannotTurnDroppedReleaseIntoClick() {
  RawTouchRegistration registration(nullptr, nullptr);
  const auto epoch = registration.epoch();
  RawTouchEvent touch{1, TouchPhase::Down, .25F, .75F, 1000};
  RawTouchRegistration::dispatch(epoch, touch);
  UiTouchEvent ui;
  require(RawTouchRegistration::pollUiEvent(ui),
          "UI forwarding remains active even without a gameplay callback");
  touch.phase = TouchPhase::Move;
  for (std::size_t i = 0; i < kUiTouchQueueCapacity - 1; ++i) {
    RawTouchRegistration::dispatch(epoch, touch);
  }
  touch.phase = TouchPhase::Up;
  RawTouchRegistration::dispatch(epoch, touch);
  touch.pointerId = 2;
  touch.phase = TouchPhase::Down;
  RawTouchRegistration::dispatch(epoch, touch);
  touch.pointerId = 3;
  RawTouchRegistration::dispatch(epoch, touch);
  require(RawTouchRegistration::pollUiEvent(ui) && ui.touch.pointerId == 1 &&
              ui.touch.phase == TouchPhase::Cancel && !RawTouchRegistration::pollUiEvent(ui),
          "overflow cancels a delivered press even when its physical release was already queued");
  for (const int pointer : {2, 3}) {
    touch.pointerId = pointer;
    touch.phase = TouchPhase::Move;
    RawTouchRegistration::dispatch(epoch, touch);
    touch.phase = TouchPhase::Up;
    RawTouchRegistration::dispatch(epoch, touch);
  }
  require(!RawTouchRegistration::pollUiEvent(ui),
          "contacts beginning during overflow cannot introduce orphan movement or releases");
  touch.phase = TouchPhase::Down;
  RawTouchRegistration::dispatch(epoch, touch);
  require(RawTouchRegistration::pollUiEvent(ui) && ui.touch.phase == TouchPhase::Down,
          "fresh UI contact admission resumes after all discarded gestures lift");
  const auto stale = ui;
  RawTouchRegistration replacement(nullptr, nullptr);
  require(!RawTouchRegistration::isCurrentEpoch(stale.epoch),
          "main-thread dispatch can reject an already-popped event after a Retry changes epoch");
}

int main() {
  testUiForwardingPreservesEpochAndOrder();
  testUiOverflowCancelsAndRequiresFreshContact();
  testUiBoundaryDiscardsUndeliveredContacts();
  testUiOverflowCannotTurnDroppedReleaseIntoClick();
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
