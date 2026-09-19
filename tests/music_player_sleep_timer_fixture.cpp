#include "ThreadCompat.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;
void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

// This is the normal predicate-wait algorithm, with a scheduling pause after
// one false predicate and before wait atomically releases its real mutex.
class ObservedConditionVariable {
public:
  bool observeShutdown = false;
  std::promise<void> initialWait, predicateGap, releaseGap, finalNotification;
  std::shared_future<void> released = releaseGap.get_future().share();
  void notify_all() {
    if (observeShutdown) {
      const int notification = ++notifications;
      if (notification == 1) pauseNextWait = true;
      if (notification == 2) finalNotification.set_value();
    }
    cv.notify_all();
  }
  template <typename Predicate>
  void wait(std::unique_lock<std::mutex> &lock, Predicate predicate) {
    while (!predicate()) {
      if (!announcedWait.exchange(true)) initialWait.set_value();
      if (pauseNextWait.exchange(false)) {
        predicateGap.set_value();
        released.wait(); // Keep the production wait mutex locked in this gap.
      }
      cv.wait(lock);
    }
  }
  template <typename Predicate>
  void wait_until(std::unique_lock<std::mutex> &lock,
                  std::chrono::steady_clock::time_point deadline,
                  Predicate predicate) {
    cv.wait_until(lock, deadline, predicate);
  }
  void rescueLostWake() { cv.notify_all(); }
private:
  std::condition_variable cv;
  std::atomic_bool announcedWait{false}, pauseNextWait{false};
  std::atomic_int notifications{0};
};

class MusicPlayerService {
public:
  ~MusicPlayerService() { StopSleepTimerWorker(); }
  bool SetSleepTimer(long long durationMicros, std::string &statusMessage);
  void ClearSleepTimer();
  long long SleepTimerRemainingMicros() const;
  void EnsureSleepTimerWorker();
  void StopSleepTimerWorker();
  void SleepTimerWorker(const std::stop_token &stopToken);
  bool StopPlaybackInternal(std::string &message) { return onExpiry(message); }
  void PublishNativeControlStatus(const std::string &message) {
    std::lock_guard lock(statusMutex);
    statuses.push_back(message);
    statusCv.notify_all();
  }
  std::string waitForStatus(std::size_t count) {
    std::unique_lock lock(statusMutex);
    require(statusCv.wait_for(lock, 3s, [&] { return statuses.size() >= count; }),
            "timer expiry should publish status");
    return statuses.back();
  }
  std::function<bool(std::string &)> onExpiry = [](std::string &) { return true; };
  mutable std::mutex sleepTimerMutex;
  ObservedConditionVariable sleepTimerCv;
  std::mutex sleepTimerThreadMutex;
  std::jthread sleepTimerThread;
  std::optional<std::chrono::steady_clock::time_point> sleepTimerDeadline;
  std::mutex statusMutex;
  std::condition_variable statusCv;
  std::vector<std::string> statuses;
};
TIMER_METHODS

void testShutdownCannotNotifyBeforeWaitRegistration() {
  MusicPlayerService service;
  auto waiting = service.sleepTimerCv.initialWait.get_future();
  auto gap = service.sleepTimerCv.predicateGap.get_future();
  auto notified = service.sleepTimerCv.finalNotification.get_future();
  service.EnsureSleepTimerWorker();
  require(waiting.wait_for(3s) == std::future_status::ready, "worker should reach its idle wait");
  // Keep shutdown between its deadline reset/first notification and its stop
  // request until the worker holds the wait mutex in the predicate gap.
  std::unique_lock launchLock(service.sleepTimerThreadMutex);
  service.sleepTimerCv.observeShutdown = true;
  auto stopped = std::async(std::launch::async, [&] { service.StopSleepTimerWorker(); });
  require(gap.wait_for(3s) == std::future_status::ready, "worker should enter controlled predicate gap");
  launchLock.unlock();
  const bool notifiedTooEarly = notified.wait_for(30ms) == std::future_status::ready;
  service.sleepTimerCv.releaseGap.set_value();
  if (notifiedTooEarly) {
    // Rescue the old implementation only so its deterministic failure exits
    // instead of hanging the test runner after losing the final notification.
    { std::lock_guard lock(service.sleepTimerMutex); }
    service.sleepTimerCv.rescueLostWake();
  }
  require(stopped.wait_for(3s) == std::future_status::ready, "shutdown should join the idle worker");
  stopped.get();
  require(!notifiedTooEarly, "shutdown notified stop before the worker registered its wait");
  require(!service.sleepTimerThread.joinable(), "shutdown should release worker ownership");
}

void testReplacementClearAndExpiryStatus() {
  MusicPlayerService service;
  std::string status;
  require(service.SetSleepTimer(0, status) && status == "Sleep timer off.", "zero duration clears timer");
  require(!service.sleepTimerThread.joinable(), "clearing unused timer does not start worker");
  for (int result = 0; result < 3; ++result) {
    service.onExpiry = [result](std::string &message) {
      if (result == 2) message = "Device could not stop.";
      return result == 0;
    };
    require(service.SetSleepTimer(3'600'000'000LL, status), "long timer starts");
    const auto remaining = service.SleepTimerRemainingMicros();
    require(remaining > 0 && remaining <= 3'600'000'000LL, "remaining time reflects deadline");
    require(service.SetSleepTimer(10'000, status) && status == "Sleep timer set.", "replacement timer starts");
    const auto published = service.waitForStatus(static_cast<std::size_t>(result + 1));
    require(published == (result == 0 ? "Sleep timer stopped playback." :
                          result == 1 ? "Sleep timer expired." : "Device could not stop."),
            "expiry preserves success and failure messages");
    require(service.SleepTimerRemainingMicros() == 0, "expiry clears deadline");
    service.StopSleepTimerWorker();
  }
  service.SetSleepTimer(3'600'000'000LL, status);
  service.ClearSleepTimer();
  require(service.SleepTimerRemainingMicros() == 0, "clear removes pending deadline");
  service.StopSleepTimerWorker();
  service.StopSleepTimerWorker();
  require(service.statuses.size() == 3, "clear and repeated shutdown do not invoke expiry");
}

void testShutdownWaitsForUnlockedExpiryCallback() {
  MusicPlayerService service;
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  service.onExpiry = [&](std::string &) {
    entered.set_value();
    released.wait();
    return true;
  };
  std::string status;
  service.SetSleepTimer(10'000, status);
  require(entered.get_future().wait_for(3s) == std::future_status::ready, "expiry callback should start");
  auto cleared = std::async(std::launch::async, [&] { service.ClearSleepTimer(); });
  const bool callbackUnlocked = cleared.wait_for(3s) == std::future_status::ready;
  if (!callbackUnlocked) release.set_value();
  require(callbackUnlocked, "expiry callback must not hold the timer mutex");
  cleared.get();
  auto stopped = std::async(std::launch::async, [&] { service.StopSleepTimerWorker(); });
  const bool waitsForCallback = stopped.wait_for(30ms) == std::future_status::timeout;
  release.set_value();
  require(stopped.wait_for(3s) == std::future_status::ready, "shutdown should finish after callback");
  stopped.get();
  require(waitsForCallback, "shutdown must join the running expiry callback");
  require(service.waitForStatus(1) == "Sleep timer stopped playback.", "started expiry keeps its outcome");
}
int main() {
  testShutdownCannotNotifyBeforeWaitRegistration();
  testReplacementClearAndExpiryStatus();
  testShutdownWaitsForUnlockedExpiryCallback();
}
