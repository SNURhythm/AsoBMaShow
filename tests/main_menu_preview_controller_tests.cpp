#include "scene/MainMenuPreviewController.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>

using namespace std::chrono_literals;

namespace {
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool released = false;
  void block() {
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_all();
    assert(changed.wait_for(lock, 5s, [&] { return released; }));
  }
  void waitUntilEntered() {
    std::unique_lock lock(mutex);
    assert(changed.wait_for(lock, 5s, [&] { return entered; }));
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
  }
};

void waitUntil(const auto &ready) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!ready() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  assert(ready());
}

ChartMetaRecord chart(const char *path) {
  ChartMetaRecord record;
  record.meta.BmsPath = path;
  return record;
}

void testDeferredReleaseWaitsForLoadWithoutBlockingSelection() {
  Gate load;
  std::atomic_bool loading = false;
  std::atomic_bool cancellationObserved = false;
  std::atomic_bool releasedDuringLoad = false;
  std::atomic_bool releasedOnSelectionThread = false;
  std::atomic_int releases = 0;
  const auto selectionThread = std::this_thread::get_id();
  MainMenuPreviewController preview(
      [&](const auto &, std::atomic_bool &cancelled) {
        loading = true;
        load.block();
        cancellationObserved = cancelled.load();
        loading = false;
      },
      [&] {
        releasedDuringLoad = loading.load();
        releasedOnSelectionThread = std::this_thread::get_id() == selectionThread;
        ++releases;
      }, 0ms);
  preview.request(chart("old.bms"));
  load.waitUntilEntered();
  preview.cancelAndReleaseWhenIdle();
  assert(releases == 0);
  load.release();
  waitUntil([&] { return releases.load() == 1; });
  preview.stop();
  assert(cancellationObserved && !releasedDuringLoad && !releasedOnSelectionThread);
  assert(releases == 1);
}

void testReplacementWithdrawsReleaseAndReloadsCancelledSamePath() {
  for (const bool samePath : {false, true}) {
    Gate firstLoad;
    Gate secondLoad;
    std::atomic_int loads = 0;
    std::atomic_int releases = 0;
    std::atomic_bool cancelledFirst = false;
    MainMenuPreviewController preview(
        [&](const auto &record, std::atomic_bool &cancelled) {
          if (++loads == 1) {
            firstLoad.block();
            cancelledFirst = cancelled.load();
          } else {
            assert(record.meta.BmsPath == (samePath ? "old.bms" : "new.bms"));
            assert(!cancelled.load());
            secondLoad.block();
          }
        }, [&] { ++releases; }, 0ms);
    preview.request(chart("old.bms"));
    firstLoad.waitUntilEntered();
    preview.cancelAndReleaseWhenIdle();
    preview.request(chart(samePath ? "old.bms" : "new.bms"));
    firstLoad.release();
    secondLoad.waitUntilEntered();
    assert(cancelledFirst && releases == 0);
    secondLoad.release();
    preview.stop();
    assert(loads == 2 && releases == 0);
  }
}

void testStopWaitsForDeferredReleaseExactlyOnce() {
  Gate release;
  std::atomic_int loads = 0;
  std::atomic_int releases = 0;
  MainMenuPreviewController preview(
      [&](const auto &, auto &) { ++loads; },
      [&] { ++releases; release.block(); }, 0ms);
  preview.request(chart("old.bms"));
  waitUntil([&] { return loads.load() == 1; });
  preview.cancelAndReleaseWhenIdle();
  release.waitUntilEntered();
  auto stop = std::async(std::launch::async, [&] { preview.stop(); });
  assert(stop.wait_for(20ms) == std::future_status::timeout);
  release.release();
  stop.get();
  assert(releases == 1);
}

void testRepeatedIdleCancellationReleasesWithoutAnotherLoad() {
  std::atomic_int loads = 0;
  std::atomic_int releases = 0;
  MainMenuPreviewController preview(
      [&](const auto &, auto &) { ++loads; }, [&] { ++releases; }, 0ms);
  preview.request(chart("old.bms"));
  waitUntil([&] { return loads.load() == 1; });
  preview.cancelAndReleaseWhenIdle();
  waitUntil([&] { return releases.load() == 1; });
  // Release only runs after the processor returns, so subsequent cancellations
  // operate with no load in flight and no new request to wake the worker.
  for (int expected = 2; expected <= 3; ++expected) {
    preview.cancelAndReleaseWhenIdle();
    waitUntil([&] { return releases.load() == expected; });
  }
  preview.stop();
  assert(loads == 1 && releases == 3);
}

void testStopRetainsPublishedChartAndAllowsRestart() {
  Gate load;
  std::atomic_int loads = 0;
  std::atomic_int releases = 0;
  MainMenuPreviewController preview(
      [&](const auto &, auto &) {
        if (++loads == 1) load.block();
      }, [&] { ++releases; }, 0ms);
  preview.request(chart("old.bms"));
  load.waitUntilEntered();
  preview.cancel();
  auto stop = std::async(std::launch::async, [&] { preview.stop(); });
  assert(stop.wait_for(20ms) == std::future_status::timeout);
  load.release();
  stop.get();
  assert(releases == 0);
  preview.request(chart("new.bms"));
  waitUntil([&] { return loads.load() == 2; });
  preview.stop();
  assert(releases == 0);
}

void testDestructionJoinsBeforeCallbackStateDies() {
  Gate load;
  auto lifetime = std::make_shared<int>(1);
  std::weak_ptr<int> observed = lifetime;
  std::atomic_bool cancelled = false;
  std::atomic_bool destroyed = false;
  std::atomic_int releases = 0;
  auto preview = std::make_unique<MainMenuPreviewController>(
      [&](const auto &, std::atomic_bool &token) {
        load.block();
        cancelled = token.load();
      }, [lifetime, &releases] { assert(*lifetime == 1); ++releases; }, 0ms);
  lifetime.reset();
  preview->request(chart("old.bms"));
  load.waitUntilEntered();
  preview->cancelAndReleaseWhenIdle();
  std::jthread shutdown([&] { preview.reset(); destroyed = true; });
  assert(!observed.expired());
  load.release();
  shutdown.join();
  assert(cancelled && destroyed && releases == 1 && observed.expired());
}

void testUnstartedWorkerDoesNotReleaseOnCallingThread() {
  std::atomic_int loads = 0;
  std::atomic_int releases = 0;
  MainMenuPreviewController preview(
      [&](const auto &, auto &) { ++loads; }, [&] { ++releases; }, 0ms);
  preview.cancelAndReleaseWhenIdle();
  preview.stop();
  assert(releases == 0);
  preview.request(chart("new.bms"));
  waitUntil([&] { return loads.load() == 1; });
  preview.stop();
  assert(releases == 0);
}
} // namespace

int main() {
  testDeferredReleaseWaitsForLoadWithoutBlockingSelection();
  testReplacementWithdrawsReleaseAndReloadsCancelledSamePath();
  testStopWaitsForDeferredReleaseExactlyOnce();
  testRepeatedIdleCancellationReleasesWithoutAnotherLoad();
  testStopRetainsPublishedChartAndAllowsRestart();
  testDestructionJoinsBeforeCallbackStateDies();
  testUnstartedWorkerDoesNotReleaseOnCallingThread();
}
