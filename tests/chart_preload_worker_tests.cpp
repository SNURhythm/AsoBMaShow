#include "../src/scene/ChartPreloadWorker.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

namespace preload_allocation_fault {
thread_local bool observing = false;
thread_local std::size_t allocations = 0;
thread_local std::size_t failAt = std::numeric_limits<std::size_t>::max();
}

void *operator new(std::size_t size) {
  using namespace preload_allocation_fault;
  if (observing && allocations++ == failAt) {
    observing = false;
    throw std::bad_alloc();
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

namespace {
int failures = 0;
void expect(bool value, const std::string &message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

ChartMetaRecord makeRecord(std::string title) {
  ChartMetaRecord record;
  record.meta.Title = std::move(title);
  record.meta.BmsPath = "/charts/" + record.meta.Title + ".bms";
  return record;
}

void testThreadStartupFailureAllowsRetryOfTheSameChart() {
  const auto record = makeRecord("startup");
  const auto measureRequest = [&] {
    ChartPreloadWorker worker(std::chrono::milliseconds(0));
    preload_allocation_fault::allocations = 0;
    preload_allocation_fault::observing = true;
    worker.request(record);
    preload_allocation_fault::observing = false;
    const auto count = preload_allocation_fault::allocations;
    worker.stop();
    return count;
  };
  (void)measureRequest(); // Warm one-time thread-library initialization.
  const auto allocationCount = measureRequest();
  expect(allocationCount > 0, "request measures actual thread startup allocations");
  if (allocationCount == 0) return;

  std::binary_semaphore processed{0};
  std::atomic_int calls = 0;
  ChartPreloadWorker worker(std::chrono::milliseconds(0));
  worker.configure([&](const ChartMetaRecord &, std::atomic_bool &) {
    ++calls;
    processed.release();
  });
  // request only notifies after constructing its thread, so its last caller
  // allocation is in real thread startup, after storing the pending chart.
  preload_allocation_fault::allocations = 0;
  preload_allocation_fault::failAt = allocationCount - 1;
  preload_allocation_fault::observing = true;
  bool threw = false;
  try {
    worker.request(record);
  } catch (const std::bad_alloc &) {
    threw = true;
  }
  preload_allocation_fault::observing = false;
  preload_allocation_fault::failAt = std::numeric_limits<std::size_t>::max();
  expect(threw, "preload thread startup failure propagates to the caller");
  expect(!worker.isRequesting("/charts/startup.bms") && calls == 0,
         "failed startup releases the queued chart without processing it");
  worker.request(record);
  expect(processed.try_acquire_for(std::chrono::seconds(3)),
         "an identical chart retry starts after thread creation failed");
  worker.stop();
  expect(calls == 1, "retry processes the chart exactly once");
}

// Processor records each started request and blocks until released so tests
// can observe latest-wins and supersede behavior deterministically.
struct RecordingProcessor {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::string> started;
  std::atomic_bool release{false};
  std::atomic<int> completed{0};
  int inFlight = 0;

  ChartPreloadWorker::Processor make(ChartPreloadWorker &worker) {
    return [this, &worker](const ChartMetaRecord &record,
                           std::atomic_bool &cancelled) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        started.push_back(record.meta.Title);
        ++inFlight;
      }
      cv.notify_all();
      while (!release.load(std::memory_order_acquire) &&
             !cancelled.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        --inFlight;
        ++completed;
      }
      cv.notify_all();
    };
  }

  void waitStarted(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return started.size() >= count; });
  }

  void waitCompleted(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return completed.load() >= static_cast<int>(count); });
  }

  [[nodiscard]] bool contains(const std::string &title) {
    std::lock_guard<std::mutex> lock(mutex);
    return std::ranges::find(started, title) != started.end();
  }
};

void testLatestWinsSupersedesQueued() {
  ChartPreloadWorker worker(std::chrono::milliseconds(20));
  RecordingProcessor recorder;
  worker.configure(recorder.make(worker));
  recorder.release.store(false);

  worker.request(makeRecord("A"));
  recorder.waitStarted(1);  // A starts after the debounce
  // While A is in flight, request B then C; only the latest should run next.
  worker.request(makeRecord("B"));
  worker.request(makeRecord("C"));
  recorder.release.store(true);
  recorder.waitCompleted(1);
  recorder.waitStarted(2);  // C runs after A
  recorder.release.store(true);
  recorder.waitCompleted(2);

  expect(recorder.contains("A"), "the first request runs");
  expect(!recorder.contains("B"),
         "a superseded queued request is abandoned (B never runs)");
  expect(recorder.contains("C"), "the latest request runs");
  worker.stop();
}

void testDedupSamePath() {
  ChartPreloadWorker worker(std::chrono::milliseconds(20));
  RecordingProcessor recorder;
  worker.configure(recorder.make(worker));
  recorder.release.store(false);

  worker.request(makeRecord("A"));
  recorder.waitStarted(1);
  // Re-requesting the same chart while it is in flight is a no-op.
  worker.request(makeRecord("A"));
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  recorder.release.store(true);
  recorder.waitCompleted(1);
  std::this_thread::sleep_for(std::chrono::milliseconds(40));

  expect(recorder.completed.load() == 1,
         "a duplicate request for the same in-flight path is deduplicated");
  worker.stop();
}

void testReselectCancelledInFlightPathReplacesPending() {
  ChartPreloadWorker worker(std::chrono::milliseconds(0));
  std::mutex mutex;
  std::condition_variable cv;
  bool release = false;
  std::vector<std::string> started;
  std::vector<bool> cancelledAtCompletion;
  worker.configure([&](const ChartMetaRecord &record, std::atomic_bool &cancelled) {
    std::unique_lock lock(mutex);
    started.push_back(record.meta.Title);
    cv.notify_all();
    cv.wait(lock, [&] { return release; });
    cancelledAtCompletion.push_back(cancelled.load(std::memory_order_acquire));
    cv.notify_all();
  });

  worker.request(makeRecord("A"));
  {
    std::unique_lock lock(mutex);
    expect(cv.wait_for(lock, std::chrono::seconds(2), [&] { return !started.empty(); }),
           "A starts before the B-to-A reselection");
  }
  worker.request(makeRecord("B"));
  worker.request(makeRecord("A"));
  {
    std::unique_lock lock(mutex);
    release = true;
    cv.notify_all();
    expect(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return cancelledAtCompletion.size() >= 2; }),
           "reselecting cancelled A schedules a fresh load");
    expect(started == std::vector<std::string>({"A", "A"}),
           "A-to-B-to-A processes only the original and latest A, never B");
    expect(cancelledAtCompletion == std::vector<bool>({true, false}),
           "the original A stays cancelled and the latest A can publish");
  }
  worker.stop();
}

void testCancelWithoutJoinStopsWorker() {
  ChartPreloadWorker worker(std::chrono::milliseconds(20));
  RecordingProcessor recorder;
  worker.configure(recorder.make(worker));
  recorder.release.store(false);

  worker.request(makeRecord("A"));
  recorder.waitStarted(1);
  worker.cancel();  // cooperative, non-blocking
  recorder.release.store(true);
  recorder.waitCompleted(1);
  expect(true, "cancel abandons in-flight work and returns to idle");
  worker.stop();
}

void testRequestAfterCancelProcessesNewItem() {
  ChartPreloadWorker worker(std::chrono::milliseconds(20));
  RecordingProcessor recorder;
  worker.configure(recorder.make(worker));
  recorder.release.store(false);

  // Select item A; it starts.
  worker.request(makeRecord("A"));
  recorder.waitStarted(1);
  // Change selection: cancel() (cooperative, no join) then request B.
  worker.cancel();
  recorder.release.store(true);
  recorder.waitCompleted(1);
  worker.request(makeRecord("B"));
  recorder.waitStarted(2);
  recorder.release.store(true);
  recorder.waitCompleted(2);

  expect(recorder.contains("B"),
         "a request after cancel() processes the new item");
  worker.stop();
}

void testStopJoinsAndIdleFires() {
  ChartPreloadWorker worker(std::chrono::milliseconds(20));
  RecordingProcessor recorder;
  worker.configure(recorder.make(worker));
  recorder.release.store(false);
  std::atomic<int> idleCount{0};
  worker.setOnIdle([&idleCount]() {
    idleCount.fetch_add(1, std::memory_order_relaxed);
  });

  worker.request(makeRecord("A"));
  recorder.waitStarted(1);
  recorder.release.store(true);
  recorder.waitCompleted(1);
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  expect(idleCount.load() >= 1, "onIdle fires after a processed request");

  worker.stop();
  expect(true, "stop joins the worker cleanly");
}

// A processor that models a long, cancellation-checking load (like a jukebox
// chart load): it loops until its cancellation flag is set, so a superseding
// request must be able to abort it promptly.
struct CancellableProcessor {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::string> started;
  std::atomic<int> completed{0};

  ChartPreloadWorker::Processor make(ChartPreloadWorker &worker) {
    return [this, &worker](const ChartMetaRecord &record,
                           std::atomic_bool &cancelled) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        started.push_back(record.meta.Title);
      }
      cv.notify_all();
      while (!cancelled.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      completed.fetch_add(1, std::memory_order_relaxed);
      cv.notify_all();
    };
  }

  void waitStarted(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return started.size() >= count; });
  }

  void waitCompleted(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return completed.load() >= static_cast<int>(count); });
  }
};

void testSupersedingRequestAbortsInFlightLoadPromptly() {
  ChartPreloadWorker worker(std::chrono::milliseconds(20));
  CancellableProcessor recorder;
  worker.configure(recorder.make(worker));

  // A starts a long load that only checks its cancellation flag.
  worker.request(makeRecord("A"));
  recorder.waitStarted(1);
  // Changing selection to B must abort A's in-flight load promptly instead of
  // blocking until A would have completed on its own.
  worker.request(makeRecord("B"));
  recorder.waitCompleted(1);  // A's load observes cancellation and returns
  recorder.waitStarted(2);     // B then starts
  worker.stop();
  expect(true, "a superseding request aborts the in-flight load");
}

void testIdleCancelRunsCleanupOnWorker() {
  ChartPreloadWorker worker(std::chrono::milliseconds(0));
  std::mutex mutex;
  std::condition_variable cv;
  bool cleanupPending = false;
  int idleCount = 0;
  int cleanupCount = 0;
  std::thread::id cleanupThread;
  worker.configure([](const ChartMetaRecord &, std::atomic_bool &) {});
  worker.setOnIdle([&] {
    std::lock_guard lock(mutex);
    ++idleCount;
    if (cleanupPending) {
      cleanupPending = false;
      ++cleanupCount;
      cleanupThread = std::this_thread::get_id();
    }
    cv.notify_all();
  });
  worker.request(makeRecord("A"));
  {
    std::unique_lock lock(mutex);
    expect(cv.wait_for(lock, std::chrono::seconds(2), [&] { return idleCount > 0; }),
           "preview load finishes before idle cancellation");
  }
  for (int iteration = 1; iteration <= 3; ++iteration) {
    {
      std::lock_guard lock(mutex);
      cleanupPending = true;
    }
    worker.cancel();
    std::unique_lock lock(mutex);
    expect(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return cleanupCount == iteration; }),
           "each idle cancellation runs pending preview cleanup");
    expect(cleanupThread != std::this_thread::get_id(),
           "idle cancellation never mutates the jukebox on the UI thread");
  }
  worker.stop();
}

void testActiveCancelDefersCleanupUntilProcessorReturns() {
  ChartPreloadWorker worker(std::chrono::milliseconds(0));
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  bool release = false;
  bool finished = false;
  bool cleanupPending = false;
  bool cleaned = false;
  bool concurrentCleanup = false;
  worker.configure([&](const ChartMetaRecord &, std::atomic_bool &) {
    std::unique_lock lock(mutex);
    entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return release; });
    finished = true;
  });
  worker.setOnIdle([&] {
    std::lock_guard lock(mutex);
    if (cleanupPending) {
      concurrentCleanup = !finished;
      cleaned = true;
      cleanupPending = false;
    }
    cv.notify_all();
  });
  worker.request(makeRecord("blocked archive enumeration"));
  {
    std::unique_lock lock(mutex);
    expect(cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }),
           "the uninterruptible processor starts");
    cleanupPending = true;
  }
  const auto started = std::chrono::steady_clock::now();
  worker.cancel();
  expect(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(250),
         "active cancellation returns without waiting for the processor");
  {
    std::unique_lock lock(mutex);
    expect(!cleaned, "cleanup cannot overlap the active processor");
    release = true;
    cv.notify_all();
    expect(cv.wait_for(lock, std::chrono::seconds(2), [&] { return cleaned; }),
           "active cancellation eventually cleans up");
    expect(!concurrentCleanup, "the processor relinquishes jukebox ownership first");
  }
  worker.stop();
}
}  // namespace

int main() {
  testThreadStartupFailureAllowsRetryOfTheSameChart();
  testLatestWinsSupersedesQueued();
  testDedupSamePath();
  testReselectCancelledInFlightPathReplacesPending();
  testCancelWithoutJoinStopsWorker();
  testRequestAfterCancelProcessesNewItem();
  testStopJoinsAndIdleFires();
  testSupersedingRequestAbortsInFlightLoadPromptly();
  testIdleCancelRunsCleanupOnWorker();
  testActiveCancelDefersCleanupUntilProcessorReturns();
  if (failures != 0) {
    std::cerr << failures << " failures\n";
    return 1;
  }
  std::cout << "Chart preload worker tests passed\n";
  return 0;
}
