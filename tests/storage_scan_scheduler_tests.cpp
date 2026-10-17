#include "../src/storage/StorageScanScheduler.h"
#include "StorageTestSupport.h"

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

using namespace storage_management;
using storage_test::require;
using namespace std::chrono_literals;
namespace {
void independentDirectoriesOverlap() {
  std::vector<StorageScanJob> roots{{.path = "/one"}, {.path = "/two"}};
  std::mutex mutex;
  std::condition_variable changed;
  int entered = 0;
  bool overlapped = false;
  StorageScanScheduleMetrics metrics;
  require(RunStorageScan(roots, {.workers = 2, .maxSubmitted = 4, .metrics = &metrics},
      [&](const auto &, auto, auto sink) {
        std::unique_lock lock(mutex);
        ++entered;
        changed.notify_all();
        const bool both = changed.wait_for(lock, 2s, [&] { return entered == 2; });
        overlapped |= both;
        lock.unlock();
        require(both, "independent directory jobs execute concurrently");
        sink({.totals = {.bytes = 3, .files = 1}});
      }, [](auto) { return true; }), "parallel scan completes");
  require(overlapped && metrics.jobsFinished == 2 && metrics.peakSubmitted <= 4,
          "shared pool finishes each job once within admission bound");
}
void skewedRootsShareWorkers() {
  std::vector<StorageScanJob> roots{{.path = "/large"}};
  std::mutex mutex;
  std::condition_variable changed;
  int entered = 0;
  std::uint64_t rootBytes = 0;
  require(RunStorageScan(roots, {.workers = 2, .maxSubmitted = 4},
      [&](const auto &job, auto, auto sink) {
        if (!job.parentId) {
          sink({.children = {{.path = "/large/left"}, {.path = "/large/right"}}});
          return;
        }
        std::unique_lock lock(mutex);
        ++entered;
        changed.notify_all();
        require(changed.wait_for(lock, 2s, [&] { return entered == 2; }),
                "subdirectories share the pool instead of pinning their root to one worker");
        lock.unlock();
        sink({.totals = {.bytes = 5, .files = 1}});
      }, [&](auto batch) {
        if (!batch.job.parentId && batch.subtreeComplete) rootBytes = batch.totals.bytes;
        return true;
      }), "skewed tree completes");
  require(rootBytes == 10, "parent completes after summing both children exactly once");
}
void localResultsExcludeDescendants() {
  std::vector<StorageScanJob> roots{{.path = "/root"}};
  std::mutex mutex;
  std::condition_variable changed;
  bool childDone = false;
  std::uint64_t local = 0, recursive = 0;
  require(RunStorageScan(roots, {.workers = 2, .maxSubmitted = 4},
      [&](const auto &job, auto, auto sink) {
        if (job.parentId) { sink({.totals = {.bytes = 1}}); return; }
        sink({.children = {{.path = "/root/child"}}});
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, 2s, [&] { return childDone; }), "child completes before root enumeration ends");
        lock.unlock();
        sink({.totals = {.bytes = 10}});
      }, [&](auto batch) {
        if (batch.job.parentId && batch.subtreeComplete) {
          { std::lock_guard lock(mutex); childDone = true; }
          changed.notify_all();
        }
        if (!batch.job.parentId && batch.directoryComplete) local = batch.totals.bytes;
        if (!batch.job.parentId && batch.subtreeComplete) recursive = batch.totals.bytes;
        return true;
      }), "local/subtree completion scan succeeds");
  require(local == 10 && recursive == 11, "directory metadata never double-counts descendants that finished earlier");
}
void wideTreeBoundsFrontier() {
  storage_test::TempDirectory temp;
  temp.write("unrelated", "keep");
  StorageScanScheduleMetrics metrics;
  std::set<std::filesystem::path> visited;
  std::vector<StorageScanJob> roots{{.path = "/root"}};
  require(RunStorageScan(roots, {.workers = 1, .maxSubmitted = 2, .frontierRecords = 3,
      .frontierBytes = 1024, .scratchRoot = temp.root, .metrics = &metrics},
      [](const auto &job, auto, auto sink) {
        if (job.parentId) { sink({.totals = {.bytes = 1, .files = 1}}); return; }
        for (int batch = 0; batch < 10; ++batch) {
          StorageScanBatch children;
          for (int i = 0; i < 10; ++i) children.children.push_back({.path = "/root/" + std::to_string(batch * 10 + i)});
          if (!sink(std::move(children))) return;
        }
      }, [&](auto batch) {
        if (batch.directoryComplete) require(visited.insert(batch.job.path).second, "each directory runs once");
        if (!batch.job.parentId && batch.subtreeComplete)
          require(batch.totals.bytes == 100, "wide rollup retains spilled descendants");
        return true;
      }), "single-worker bounded discovery never deadlocks");
  require(visited.size() == 101 && metrics.spilledJobs > 0, "spilled tree retains all jobs");
  require(metrics.peakFrontierRecords <= 3, "frontier stays within resident record budget");
  require(metrics.peakSubmitted <= 2, "submitted jobs stay within admission budget");
  require(metrics.peakBufferedBatches <= 2, "result batches stay within global worker budget");
  require(std::distance(std::filesystem::directory_iterator(temp.root), std::filesystem::directory_iterator{}) == 1,
          "only owned scan spill is removed; unrelated files remain");
}
void saturatedCancellationWakesAllWaiters() {
  std::vector<StorageScanJob> roots{{.path = "/root"}};
  int delivered = 0;
  require(!RunStorageScan(roots, {.workers = 1, .maxSubmitted = 2},
      [](const auto &, auto check, auto sink) {
        for (int i = 0; i < 10000 && check(); ++i)
          if (!sink({.children = {{.path = "/child/" + std::to_string(i)}}})) return;
      }, [&](auto) { return ++delivered < 2; }), "consumer cancellation drains saturated workers");
  std::stop_source stop;
  std::mutex mutex;
  std::condition_variable changed;
  bool started = false;
  std::jthread canceller([&] {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return started; });
    stop.request_stop();
  });
  require(!RunStorageScan(roots, {.workers = 1, .maxSubmitted = 2, .stopToken = stop.get_token()},
      [&](const auto &, auto check, auto sink) {
        { std::lock_guard lock(mutex); started = true; }
        changed.notify_all();
        while (check()) if (!sink({})) break;
      }, [](auto) { return true; }), "stop token cancels producer and coordinator without polling");
}
void archiveJobsShareOnePool() {
  std::vector<StorageScanJob> roots;
  for (int i = 0; i < 10; ++i)
    roots.push_back({.kind = i % 2 ? StorageScanJobKind::Directory : StorageScanJobKind::Archive,
                     .path = "/" + std::to_string(i)});
  std::atomic<int> active{0}, peak{0}, archives{0}, archivePeak{0};
  StorageScanScheduleMetrics metrics;
  require(RunStorageScan(roots, {.workers = 3, .archiveBuilders = 1, .maxSubmitted = 6, .metrics = &metrics},
      [&](const auto &job, auto, auto sink) {
        const int current = ++active;
        peak.store(std::max(peak.load(), current));
        if (job.kind == StorageScanJobKind::Archive) {
          const int count = ++archives;
          archivePeak.store(std::max(archivePeak.load(), count));
        }
        sink({.totals = {.files = 1}});
        if (job.kind == StorageScanJobKind::Archive) --archives;
        --active;
      }, [](auto) { return true; }), "both work classes finish");
  require(metrics.jobsFinished == 10 && peak <= 3 && archivePeak == 1,
          "archive builders share worker cap and obey separate admission");
}
void spillFailureStopsAndCleansUp() {
  storage_test::TempDirectory temp;
  temp.write("not-directory");
  std::vector<StorageScanJob> roots{{.path = "/root"}};
  require(!RunStorageScan(roots, {.workers = 1, .frontierRecords = 1, .scratchRoot = temp.root / "not-directory"},
      [](const auto &, auto, auto sink) {
        sink({.children = {{.path = "/a"}, {.path = "/b"}, {.path = "/c"}}});
      }, [](auto) { return true; }), "spill I/O failure cancels pending work");
}
void firstScanCreatesItsPrivateSpillRoot() {
  storage_test::TempDirectory temp;
  temp.write("private/keep", "unrelated");
  const auto scratch = temp.root / "private/storage-management/scan";
  std::vector<StorageScanJob> roots{{.path = "/one"}, {.path = "/two"}, {.path = "/three"}};
  StorageScanScheduleMetrics metrics;
  require(RunStorageScan(roots, {.workers = 1, .frontierRecords = 1, .scratchRoot = scratch, .metrics = &metrics},
      [](const auto &, auto, auto sink) { sink({.totals = {.bytes = 1, .files = 1}}); },
      [](auto) { return true; }), "first large scan creates its missing private scratch hierarchy");
  require(metrics.jobsFinished == 3 && metrics.spilledJobs == 3 && std::filesystem::is_empty(scratch) &&
              std::filesystem::is_regular_file(temp.root / "private/keep"),
          "fresh-root spill completes all jobs and removes only its owned database directory");
}
}
int main() {
  try {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    require(DefaultStorageScanLimits().workers == 1, "unmeasured mobile defaults to one scan worker");
#endif
    independentDirectoriesOverlap();
    skewedRootsShareWorkers();
    localResultsExcludeDescendants();
    wideTreeBoundsFrontier();
    saturatedCancellationWakesAllWaiters();
    archiveJobsShareOnePool();
    spillFailureStopsAndCleansUp();
    firstScanCreatesItsPrivateSpillRoot();
  } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
