#include "audio/JukeboxSchedulerWake.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

namespace {

void require(bool result, const char *message) {
  if (!result) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testNotificationsBeforeWaitAreRemembered() {
  JukeboxSchedulerWake wake;
  const auto beforeWork = wake.capture();
  wake.notify();
  wake.notify();
  require(wake.waitFor(beforeWork, 0us, [] { return false; }),
          "notification during scheduling work must prevent sleep");

  const auto afterWork = wake.capture();
  require(!wake.waitFor(afterWork, 0us, [] { return false; }),
          "an unchanged generation must allow sleep");
  require(wake.waitFor(afterWork, 0us, [] { return true; }),
          "already-ready state must prevent sleep without a new notification");
}

void testNotificationDuringPredicateEvaluation() {
  JukeboxSchedulerWake wake;
  const auto generation = wake.capture();
  std::promise<void> predicateEntered;
  auto entered = predicateEntered.get_future();
  std::promise<void> releasePredicate;
  auto release = releasePredicate.get_future();
  std::promise<void> waiterCompleted;
  auto completed = waiterCompleted.get_future();
  bool awakened = false;
  std::thread waiter([&] {
    bool firstEvaluation = true;
    awakened = wake.waitFor(generation, 5s, [&] {
      if (firstEvaluation) {
        firstEvaluation = false;
        predicateEntered.set_value();
        release.wait();
      }
      return false;
    });
    waiterCompleted.set_value();
  });
  require(entered.wait_for(2s) == std::future_status::ready,
          "waiter must enter its predicate");
  std::promise<void> notifierStarted;
  auto started = notifierStarted.get_future();
  std::thread notifier([&] {
    notifierStarted.set_value();
    wake.notify();
  });
  require(started.wait_for(2s) == std::future_status::ready,
          "notifier must start");
  releasePredicate.set_value();
  notifier.join();
  const bool completedPromptly =
      completed.wait_for(1s) == std::future_status::ready;
  if (!completedPromptly) {
    wake.notify();
  }
  waiter.join();
  require(completedPromptly,
          "notification must finish the wait before its fallback timeout");
  require(awakened, "notification at the predicate/wait boundary must survive");
}

}

int main() {
  testNotificationsBeforeWaitAreRemembered();
  testNotificationDuringPredicateEvaluation();
  return 0;
}
