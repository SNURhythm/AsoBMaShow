#include "../src/ChartScanWorkScheduler.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void testLaterEntitiesUseWorkersWhileArchiveIsActive() {
  chart_scan::WorkScheduler scheduler(4);
  std::mutex mutex;
  std::condition_variable cv;
  bool archiveStarted = false;
  bool releaseArchive = false;
  int ordinaryStarted = 0;

  assert(scheduler.enqueue([&] {
    std::unique_lock lock(mutex);
    archiveStarted = true;
    cv.notify_all();
    cv.wait(lock, [&] { return releaseArchive; });
  }));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, 2s, [&] { return archiveStarted; }));
  }

  for (int i = 0; i < 3; ++i) {
    assert(scheduler.enqueue([&] {
      std::lock_guard lock(mutex);
      ++ordinaryStarted;
      cv.notify_all();
    }));
  }
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, 2s, [&] { return ordinaryStarted == 3; }));
    releaseArchive = true;
  }
  cv.notify_all();

  scheduler.finish();
  assert(scheduler.takeExceptions().empty());
}

void testArchiveAdmissionLeavesWorkersForCpuTasks() {
  chart_scan::WorkScheduler scheduler(4, 2);
  std::mutex mutex;
  std::condition_variable cv;
  int activeArchives = 0;
  int maximumActiveArchives = 0;
  int startedArchives = 0;
  int startedCpuTasks = 0;
  bool releaseArchives = false;

  for (int index = 0; index < 4; ++index) {
    assert(scheduler.enqueue(
        [&] {
          std::unique_lock lock(mutex);
          ++activeArchives;
          ++startedArchives;
          maximumActiveArchives =
              std::max(maximumActiveArchives, activeArchives);
          cv.notify_all();
          cv.wait(lock, [&] { return releaseArchives; });
          --activeArchives;
          cv.notify_all();
        },
        chart_scan::WorkClass::ArchiveIo));
  }

  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, 2s, [&] { return startedArchives == 2; }));
  }
  for (int index = 0; index < 2; ++index) {
    assert(scheduler.enqueue([&] {
      std::lock_guard lock(mutex);
      ++startedCpuTasks;
      cv.notify_all();
    }));
  }
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, 2s, [&] { return startedCpuTasks == 2; }));
    assert(startedArchives == 2);
    assert(maximumActiveArchives == 2);
    releaseArchives = true;
  }
  cv.notify_all();

  scheduler.finish();
  assert(startedArchives == 4);
  assert(maximumActiveArchives == 2);
  assert(scheduler.takeExceptions().empty());
}

void testFinishDrainsWorkSpawnedByActiveTask() {
  chart_scan::WorkScheduler scheduler(1);
  std::mutex mutex;
  std::condition_variable cv;
  bool rootStarted = false;
  bool releaseRoot = false;
  std::atomic_bool finishStarted{false};
  std::atomic_bool childAccepted{false};
  std::atomic_bool childRan{false};

  assert(scheduler.enqueue([&] {
    {
      std::unique_lock lock(mutex);
      rootStarted = true;
      cv.notify_all();
      cv.wait(lock, [&] { return releaseRoot; });
    }
    childAccepted.store(
        scheduler.enqueue(
            [&] { childRan.store(true, std::memory_order_release); }),
        std::memory_order_release);
  }));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, 2s, [&] { return rootStarted; }));
  }

  std::thread finisher([&] {
    finishStarted.store(true, std::memory_order_release);
    scheduler.finish();
  });
  while (!finishStarted.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(20ms);
  {
    std::lock_guard lock(mutex);
    releaseRoot = true;
  }
  cv.notify_all();
  finisher.join();

  assert(childAccepted.load(std::memory_order_acquire));
  assert(childRan.load(std::memory_order_acquire));
  assert(!scheduler.enqueue([] {}));
}

void testSingleWorkerPreservesFifoOrder() {
  chart_scan::WorkScheduler scheduler(1);
  std::vector<int> order;
  for (int value = 0; value < 5; ++value) {
    assert(scheduler.enqueue([&, value] { order.push_back(value); }));
  }

  scheduler.finish();
  assert((order == std::vector<int>{0, 1, 2, 3, 4}));
}

void testTaskExceptionDoesNotStopWorker() {
  chart_scan::WorkScheduler scheduler(1);
  std::atomic_bool laterTaskRan{false};
  assert(scheduler.enqueue([] { throw std::runtime_error("expected"); }));
  assert(scheduler.enqueue(
      [&] { laterTaskRan.store(true, std::memory_order_release); }));

  scheduler.finish();
  scheduler.finish();
  assert(laterTaskRan.load(std::memory_order_acquire));
  assert(scheduler.takeExceptions().size() == 1);
  assert(scheduler.takeExceptions().empty());
  assert(!scheduler.enqueue([] {}));
}

void testCancelDiscardsQueuedWorkAndJoins() {
  chart_scan::WorkScheduler scheduler(1);
  std::mutex mutex;
  std::condition_variable cv;
  bool activeStarted = false;
  bool releaseActive = false;
  std::atomic_bool queuedTaskRan{false};

  assert(scheduler.enqueue([&] {
    std::unique_lock lock(mutex);
    activeStarted = true;
    cv.notify_all();
    cv.wait(lock, [&] { return releaseActive; });
  }));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, 2s, [&] { return activeStarted; }));
  }
  assert(scheduler.enqueue(
      [&] { queuedTaskRan.store(true, std::memory_order_release); }));

  std::thread canceller([&] { scheduler.cancel(); });
  while (scheduler.enqueue([] {})) {
    std::this_thread::yield();
  }
  {
    std::lock_guard lock(mutex);
    releaseActive = true;
  }
  cv.notify_all();
  canceller.join();

  assert(!queuedTaskRan.load(std::memory_order_acquire));
  assert(!scheduler.enqueue([] {}));
  scheduler.cancel();
}

} // namespace

int main() {
  testLaterEntitiesUseWorkersWhileArchiveIsActive();
  testArchiveAdmissionLeavesWorkersForCpuTasks();
  testFinishDrainsWorkSpawnedByActiveTask();
  testSingleWorkerPreservesFifoOrder();
  testTaskExceptionDoesNotStopWorker();
  testCancelDiscardsQueuedWorkAndJoins();
  return 0;
}
