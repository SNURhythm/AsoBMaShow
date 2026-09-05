#include "../src/scene/ChartPreloadWorker.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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
}  // namespace

int main() {
  testLatestWinsSupersedesQueued();
  testDedupSamePath();
  testCancelWithoutJoinStopsWorker();
  testStopJoinsAndIdleFires();
  if (failures != 0) {
    std::cerr << failures << " failures\n";
    return 1;
  }
  std::cout << "Chart preload worker tests passed\n";
  return 0;
}