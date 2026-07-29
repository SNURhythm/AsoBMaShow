#include "../src/ChartScanWorkScheduler.h"

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
  testSingleWorkerPreservesFifoOrder();
  testTaskExceptionDoesNotStopWorker();
  testCancelDiscardsQueuedWorkAndJoins();
  return 0;
}
