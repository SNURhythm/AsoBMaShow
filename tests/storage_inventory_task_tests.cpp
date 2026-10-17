#include "../src/storage/StorageInventoryTask.h"
#include "StorageTestSupport.h"
#include <atomic>
#include <condition_variable>
#include <future>
#include <latch>
#include <iostream>
using namespace storage_management;
using storage_test::require;
namespace {
void measurementsArriveBeforeScanCompletion() {
  std::latch entered(1), secondPublished(1);
  std::promise<void> second, release;
  auto secondRequested = second.get_future();
  auto released = release.get_future();
  std::atomic<int> ticks{0}, wakes{0};
  StorageInventoryTask task({.measurements = [&](const auto &, auto, auto publish, auto) {
    publish({{.groupId = "first", .totalBytes = 10}});
    publish({{.groupId = "first", .totalBytes = 20}});
    entered.count_down(); secondRequested.wait();
    publish({{.groupId = "first", .totalBytes = 25}});
    publish({{.groupId = "first", .totalBytes = 30}});
    secondPublished.count_down(); released.wait();
    StorageMeasurements result; result.groups.push_back({.groupId = "last", .totalBytes = 30}); return result;
  }, .now = [&] { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(ticks.load())); },
  .wake = [&] { ++wakes; }});
  task.StartMeasurements({.groups = {{"first", "/first"}, {"last", "/last"}}});
  // Failure must release the worker too, so a failed assertion cannot hang its destructor.
  entered.wait();
  const auto early = task.TakeMeasurements();
  const bool beforeCompletion = !task.TakeResult();
  ticks = 50; second.set_value(); secondPublished.wait();
  const bool throttled = task.TakeMeasurements().empty();
  ticks = 100; const auto next = task.TakeMeasurements();
  release.set_value();
  require(throttled && next.size() == 1 && next.front().totalBytes == 30,
          "updates are coalesced until the 100 ms publication boundary");
  require(early.size() == 1 && early.front().totalBytes == 20 && beforeCompletion,
          "coalesced measurements arrive before scan completion");
  std::optional<StorageInventoryTaskResult> done;
  while (!(done = task.TakeResult())) std::this_thread::yield();
  require(done->complete && !done->preflight && task.TakeMeasurements().size() == 1, "completion flushes final rows");
  require(wakes <= 2, "bursts notify once plus completion");
}
void selectedReviewCancelsUnrelatedScan() {
  std::latch entered(1);
  bool cancelled = false;
  std::vector<FolderSelection> reviewed;
  StorageInventoryTask task({.measurements = [&](const auto &, auto check, auto publish, auto stop) {
    publish({{.groupId = "unrelated", .totalBytes = 11}});
    entered.count_down();
    std::mutex mutex; std::condition_variable_any changed; std::unique_lock lock(mutex);
    changed.wait(lock, stop, [] { return false; });
    cancelled = !check();
    publish({{.groupId = "unrelated", .totalBytes = 99}});
    return StorageMeasurements{};
  }, .preflight = [&](const auto &request, auto, auto) {
    reviewed = request.groups;
    StoragePreflight result; result.request = request; result.eligibleCount = 1; return result;
  }, .wake = [] {}});
  task.StartMeasurements({.groups = {{"unrelated", "/unrelated"}}});
  entered.wait();
  task.StartPreflight({.groups = {{"selected", "/selected"}}});
  std::optional<StorageInventoryTaskResult> done;
  while (!(done = task.TakeResult())) std::this_thread::yield();
  require(cancelled && reviewed.size() == 1 && reviewed.front().groupId == "selected", "review interrupts unrelated scan");
  require(task.TakeMeasurements().empty() && done->preflight && done->preflight->eligibleCount == 1,
          "replaced scan cannot publish old rows or evidence");
}
void failuresRemainUnavailable() {
  StorageInventoryTask task({.measurements = [](const auto &, auto, auto, auto) -> StorageMeasurements {
    throw std::runtime_error("unreadable");
  }, .wake = [] {}});
  task.StartMeasurements({.groups = {{"folder", "/unreadable"}}});
  std::optional<StorageInventoryTaskResult> done;
  while (!(done = task.TakeResult())) std::this_thread::yield();
  const auto rows = task.TakeMeasurements();
  require(!done->complete && rows.size() == 1 && !rows.front().totalBytes &&
      rows.front().reason.code == StorageReason::IoError, "failed scan publishes unavailable totals");
  task.Cancel(); require(task.TakeMeasurements().empty() && !task.TakeResult(), "cancel retires results");
}
}
int RunStorageInventoryTaskTests() {
  try { measurementsArriveBeforeScanCompletion(); selectedReviewCancelsUnrelatedScan(); failuresRemainUnavailable(); }
  catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
  return 0;
}
