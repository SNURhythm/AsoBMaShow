#include "../src/storage/StorageInventoryTask.h"
#include "StorageTestSupport.h"
#include <atomic>
#include <condition_variable>
#include <future>
#include <latch>
#include <iostream>
using namespace storage_management;
using storage_test::require;
namespace bounded_allocation_probe {
extern thread_local bool observeStartup;
extern thread_local std::size_t startupAllocations, failedStartupAllocation;
}
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
void startupFailuresPublishUnavailableAndAllowRetry() {
  using namespace bounded_allocation_probe;
  for (const auto mode : {StorageInventoryMode::Measurements, StorageInventoryMode::Preflight}) {
    std::atomic<unsigned> runs = 0, wakes = 0;
    StorageInventoryTask task({.measurements = [&](const auto &request, auto, auto, auto) {
      ++runs; StorageMeasurements result;
      result.groups.push_back({.groupId = request.groups.front().groupId, .totalBytes = 42}); return result;
    }, .preflight = [&](const auto &, auto, auto) {
      ++runs; StoragePreflight result; result.eligibleCount = 1; return result;
    }, .wake = [&] { ++wakes; }});
    const auto start = [&](InventoryRequest request) {
      if (mode == StorageInventoryMode::Measurements) task.StartMeasurements(std::move(request));
      else task.StartPreflight(std::move(request));
    };
    InventoryRequest initial{.groups = {{"folder", "/folder"}}};
    startupAllocations = 0; observeStartup = true;
    start(std::move(initial));
    observeStartup = false;
    const auto allocations = startupAllocations;
    while (!task.TakeResult()) std::this_thread::yield();
    task.Cancel();
    require(allocations != 0 && runs == 1, "inventory startup uses real asynchronous worker construction");
    for (std::size_t index = 0; index < allocations; ++index) {
      InventoryRequest request{.groups = {{"folder", "/folder"}}};
      const auto priorRuns = runs.load(), priorWakes = wakes.load();
      failedStartupAllocation = index; startupAllocations = 0; observeStartup = true;
      bool threw = false;
      try { start(std::move(request)); } catch (...) { threw = true; }
      observeStartup = false;
      require(!threw, "inventory startup failure must not escape into the scene");
      const auto result = task.TakeResult();
      const auto rows = task.TakeMeasurements();
      require(result && result->mode == mode && !result->complete && !result->cancelled && !result->preflight &&
          rows.size() == 1 && rows.front().groupId == "folder" && !rows.front().totalBytes &&
          rows.front().reason.code == StorageReason::IoError && !rows.front().reason.detail.empty() &&
          runs == priorRuns && wakes == priorWakes + 1,
          "failed startup finishes with unavailable rows and wakes the scene without running a builder");
      const auto generation = rows.front().generation;
      start({.groups = {{"folder", "/folder"}}});
      std::optional<StorageInventoryTaskResult> retried;
      while (!(retried = task.TakeResult())) std::this_thread::yield();
      require(retried->complete && runs == priorRuns + 1 && !task.TakeResult(), "inventory can retry once resources recover");
      if (mode == StorageInventoryMode::Measurements) {
        const auto rows = task.TakeMeasurements();
        require(rows.size() == 1 && rows.front().totalBytes == 42 && rows.front().generation > generation,
            "retry replaces failed measurement provenance with the new generation");
      } else require(retried->preflight && retried->preflight->eligibleCount == 1, "retry publishes usable review evidence");
      task.Cancel();
    }
    failedStartupAllocation = static_cast<std::size_t>(-1);
  }
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
  try { startupFailuresPublishUnavailableAndAllowRetry(); measurementsArriveBeforeScanCompletion(); selectedReviewCancelsUnrelatedScan(); failuresRemainUnavailable(); }
  catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
  return 0;
}
