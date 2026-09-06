#include "library/ChartLibraryTaskTypes.h"
#include "library/ChartLibraryTaskService.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
  }
}

const chart_library_tasks::TaskInfo *
taskFor(const chart_library_tasks::Snapshot &snapshot, std::uint64_t id) {
  for (const auto &task : snapshot.tasks) {
    if (task.id == id) {
      return &task;
    }
  }
  return nullptr;
}

template <typename Predicate>
bool waitUntil(Predicate predicate) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::yield();
  }
  return predicate();
}

void testSnapshotCarriesQueueAndProgressAsValues() {
  chart_library_tasks::Snapshot snapshot{
      .revision = 7,
      .activeCount = 1,
      .tasks = {{.id = 42,
                 .title = "Refresh Library",
                 .status = chart_library_tasks::TaskStatus::Running,
                 .fraction = 0.25,
                 .current = 3,
                 .total = 12,
                 .detail = "Scanning folders"}},
      .progress = {.valid = true,
                   .revision = 8,
                   .taskId = 42,
                   .current = 3,
                   .total = 12,
                   .basisPoints = 2500,
                   .stage = ChartScanProgressStage::ScanningRoots}};

  expect(snapshot.tasks.front().title == "Refresh Library",
         "snapshot owns the task title");
  expect(snapshot.progress.basisPoints == 2500,
         "snapshot owns progress basis points");
  snapshot.tasks.front().title = "Changed";
  expect(snapshot.tasks.front().title == "Changed",
         "snapshot task values are independently mutable");
}

void testWorkerRunsQueuedTasksOnceInOrder() {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::string> titles;
  chart_library_tasks::ChartLibraryTaskService service(
      [&](const auto &request, const auto &, auto, auto) {
        {
          std::lock_guard lock(mutex);
          titles.push_back(request.title);
        }
        changed.notify_all();
        return chart_library_tasks::TaskRunResult{};
      });

  service.start();
  service.start();
  service.enqueue({.title = "first"});
  service.enqueue({.title = "second"});

  std::unique_lock lock(mutex);
  const bool finished = changed.wait_for(
      lock, std::chrono::seconds(2), [&] { return titles.size() == 2; });
  expect(finished, "worker completes both queued tasks");
  expect(titles == std::vector<std::string>{"first", "second"},
         "one worker runs tasks once in FIFO order");
  lock.unlock();
  service.shutdown();
}

void testGameplayPauseBlocksCurrentAndQueuedTasksUntilResume() {
  std::mutex mutex;
  std::condition_variable changed;
  bool firstEntered = false;
  bool releaseFirstToCheckpoint = false;
  std::vector<std::string> completed;

  chart_library_tasks::ChartLibraryTaskService service(
      [&](const auto &request, const auto &, auto publishProgress,
          auto waitForResume) {
        publishProgress({.current = 1,
                         .total = 4,
                         .stage = ChartScanProgressStage::ScanningRoots},
                        "Scanning roots");
        if (request.title == "first") {
          std::unique_lock lock(mutex);
          firstEntered = true;
          changed.notify_all();
          changed.wait(lock, [&] { return releaseFirstToCheckpoint; });
        }
        if (!waitForResume()) {
          return chart_library_tasks::TaskRunResult{
              .disposition =
                  chart_library_tasks::TaskRunDisposition::Paused,
              .detail = "Paused"};
        }
        {
          std::lock_guard lock(mutex);
          completed.push_back(request.title);
        }
        changed.notify_all();
        return chart_library_tasks::TaskRunResult{};
      });

  service.start();
  const auto firstId = service.enqueue({.title = "first"});
  {
    std::unique_lock lock(mutex);
    expect(changed.wait_for(lock, std::chrono::seconds(2),
                            [&] { return firstEntered; }),
           "first task reaches its checkpoint");
  }

  service.setGameplayPaused(true);
  const auto secondId = service.enqueue({.title = "second"});
  auto paused = service.snapshot();
  const auto statusFor = [&](std::uint64_t id) {
    for (const auto &task : paused.tasks) {
      if (task.id == id) {
        return task.status;
      }
    }
    return chart_library_tasks::TaskStatus::Failed;
  };
  expect(statusFor(firstId) == chart_library_tasks::TaskStatus::Paused,
         "gameplay pause publishes current task as paused");
  expect(statusFor(secondId) == chart_library_tasks::TaskStatus::Paused,
         "gameplay pause publishes queued task as paused");
  expect(paused.progress.valid && paused.progress.taskId == firstId &&
             paused.progress.current == 1 &&
             paused.progress.stage == ChartScanProgressStage::ScanningRoots,
         "pause retains current task progress");

  {
    std::lock_guard lock(mutex);
    releaseFirstToCheckpoint = true;
  }
  changed.notify_all();
  {
    std::unique_lock lock(mutex);
    const bool ranWhilePaused = changed.wait_for(
        lock, std::chrono::milliseconds(50), [&] { return !completed.empty(); });
    expect(!ranWhilePaused, "current task stays blocked during gameplay");
  }

  service.setGameplayPaused(false);
  {
    std::unique_lock lock(mutex);
    expect(changed.wait_for(lock, std::chrono::seconds(2),
                            [&] { return completed.size() == 2; }),
           "resume completes current and queued tasks");
    expect(completed == std::vector<std::string>{"first", "second"},
           "resume preserves current-before-queued ordering");
  }

  auto completedSnapshot = service.snapshot();
  completedSnapshot.tasks.front().detail = "mutated copy";
  expect(service.snapshot().tasks.front().detail == "Complete",
         "snapshot mutation cannot change service state");
  service.shutdown();
}

void testProgressUpdatesTaskRowAndProgressSnapshotTogether() {
  std::mutex mutex;
  std::condition_variable changed;
  bool progressPublished = false;
  bool release = false;
  chart_library_tasks::ChartLibraryTaskService service(
      [&](const auto &, const auto &, auto publishProgress, auto) {
        publishProgress({.current = 2,
                         .total = 8,
                         .stage = ChartScanProgressStage::ParsingCharts},
                        "Parsing charts");
        std::unique_lock lock(mutex);
        progressPublished = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
        return chart_library_tasks::TaskRunResult{};
      });

  const auto id = service.enqueue({.title = "refresh"});
  {
    std::unique_lock lock(mutex);
    expect(changed.wait_for(lock, std::chrono::seconds(2),
                            [&] { return progressPublished; }),
           "runner publishes progress");
  }
  const auto snapshot = service.snapshot();
  const auto *task = taskFor(snapshot, id);
  expect(task != nullptr && task->current == 2 && task->total == 8 &&
             task->fraction == 0.25 && task->detail == "Parsing charts",
         "task row advances with published progress");
  expect(snapshot.progress.valid && snapshot.progress.taskId == id &&
             snapshot.progress.current == 2 &&
             snapshot.progress.total == 8 &&
             snapshot.progress.basisPoints == 2500 &&
             snapshot.progress.stage == ChartScanProgressStage::ParsingCharts,
         "progress snapshot advances with the same values");

  {
    std::lock_guard lock(mutex);
    release = true;
  }
  changed.notify_all();
  expect(waitUntil([&] {
           const auto current = service.snapshot();
           const auto *row = taskFor(current, id);
           return row != nullptr &&
                  row->status == chart_library_tasks::TaskStatus::Complete;
         }),
         "progress task reaches terminal state");
  service.shutdown();
}

void testFailuresCompletionsAndHistoryRemainObservable() {
  chart_library_tasks::ChartLibraryTaskService service(
      [](const auto &request, const auto &, auto, auto) {
        if (request.title == "throws") {
          throw std::runtime_error("scanner exploded");
        }
        if (request.kind ==
            chart_library_tasks::TaskKind::IndexDownloadedPath) {
          return chart_library_tasks::TaskRunResult{
              .downloadedIndex =
                  chart_library_tasks::DownloadedIndexCompletion{
                      .chartPath = "download/chart.bms",
                      .targetIdentity = {.sha256 = "sha"},
                      .selectionGeneration = 9}};
        }
        return chart_library_tasks::TaskRunResult{};
      });

  const auto failedId = service.enqueue({.title = "throws"});
  expect(waitUntil([&] {
           const auto current = service.snapshot();
           const auto *row = taskFor(current, failedId);
           return row != nullptr &&
                  row->status == chart_library_tasks::TaskStatus::Failed;
         }),
         "runner exception reaches failed state");
  const auto failed = service.snapshot();
  expect(taskFor(failed, failedId) != nullptr &&
             taskFor(failed, failedId)->detail == "scanner exploded",
         "runner exception detail remains observable");

  const auto downloadId = service.enqueue(
      {.kind = chart_library_tasks::TaskKind::IndexDownloadedPath,
       .title = "index"});
  expect(waitUntil([&] {
           const auto current = service.snapshot();
           const auto *row = taskFor(current, downloadId);
           return row != nullptr &&
                  row->status == chart_library_tasks::TaskStatus::Complete;
         }),
         "downloaded-index task completes");
  auto completions = service.takeDownloadedIndexCompletions();
  expect(completions.size() == 1 &&
             completions.front().chartPath == "download/chart.bms" &&
             completions.front().targetIdentity.sha256 == "sha" &&
             completions.front().selectionGeneration == 9,
         "downloaded-index completion is value-owned");
  expect(service.takeDownloadedIndexCompletions().empty(),
         "downloaded-index completions are drained once");

  for (int index = 0; index < 25; ++index) {
    service.enqueue({.title = "history " + std::to_string(index)});
  }
  expect(waitUntil([&] { return service.snapshot().activeCount == 0; }),
         "history tasks all reach terminal states");
  const auto history = service.snapshot();
  expect(history.tasks.size() == 24,
         "terminal task history retains exactly 24 rows");
  expect(history.tasks.front().title == "history 1" &&
             history.tasks.back().title == "history 24",
         "history removes the oldest terminal rows first");
  service.shutdown();
}

void testReservedPlatformCopyTaskCanBeQueuedOrFailed() {
  chart_library_tasks::ChartLibraryTaskService service(
      [](const auto &, const auto &, auto, auto) {
        return chart_library_tasks::TaskRunResult{};
      });

  const auto queuedId = service.reserve("Import Archive", "Copying archive");
  const auto reserved = service.snapshot();
  expect(taskFor(reserved, queuedId) != nullptr &&
             taskFor(reserved, queuedId)->status ==
                 chart_library_tasks::TaskStatus::Running &&
             taskFor(reserved, queuedId)->detail == "Copying archive",
         "platform copy reservation is visible as active work");
  expect(service.enqueueReserved(
             queuedId,
             {.kind = chart_library_tasks::TaskKind::AndroidImport,
              .title = "Import Archive",
              .androidImportPath = "copied.zip"}),
         "reserved platform copy can enter the worker queue");
  expect(waitUntil([&] {
           const auto current = service.snapshot();
           const auto *row = taskFor(current, queuedId);
           return row != nullptr &&
                  row->status == chart_library_tasks::TaskStatus::Complete;
         }),
         "queued reservation completes with its original task ID");

  const auto failedId = service.reserve("Import Folder", "Copying folder");
  expect(service.failReserved(failedId, "Folder import cancelled."),
         "reserved platform copy can publish a failure");
  const auto failed = service.snapshot();
  expect(taskFor(failed, failedId) != nullptr &&
             taskFor(failed, failedId)->status ==
                 chart_library_tasks::TaskStatus::Failed &&
             taskFor(failed, failedId)->detail == "Folder import cancelled.",
         "failed reservation retains the platform error");
  service.shutdown();
}

} // namespace

int main() {
  testSnapshotCarriesQueueAndProgressAsValues();
  testWorkerRunsQueuedTasksOnceInOrder();
  testGameplayPauseBlocksCurrentAndQueuedTasksUntilResume();
  testProgressUpdatesTaskRowAndProgressSnapshotTogether();
  testFailuresCompletionsAndHistoryRemainObservable();
  testReservedPlatformCopyTaskCanBeQueuedOrFailed();
  if (failures != 0) {
    std::cerr << failures << " chart library task test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "chart library task tests passed\n";
  return EXIT_SUCCESS;
}
