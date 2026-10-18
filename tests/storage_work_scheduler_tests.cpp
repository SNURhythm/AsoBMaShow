#include "../src/storage/StorageWorkScheduler.h"
#include "../src/utils/ParallelWork.h"
#include "StorageTestSupport.h"
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <thread>
#include <set>
#include <cstdlib>
#include <new>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
using namespace storage_management;
using storage_test::require;
using namespace std::chrono_literals;
using MediaKind = asobmshow::chart_assets::MediaKind;
namespace {
thread_local bool observeEncodingAllocations = false;
thread_local std::size_t encodingAllocations = 0, failedEncodingAllocation = static_cast<std::size_t>(-1);
}
void *operator new(std::size_t size) {
  if (observeEncodingAllocations && encodingAllocations++ == failedEncodingAllocation) {
    observeEncodingAllocations = false; throw std::bad_alloc();
  }
  if (void *value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc();
}
void operator delete(void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
namespace {
void platformDefaultsUseExistingWorkerPolicy() {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
  require(DefaultStorageExecutionLimits().audioWorkers == parallel_worker_count(std::numeric_limits<std::size_t>::max()),
          "mobile audio conversion uses the chart audio loading worker policy");
#else
  require(DefaultStorageExecutionLimits().audioWorkers >= 1 && DefaultStorageExecutionLimits().audioWorkers <= 4,
          "desktop audio workers remain within measured cap");
#endif
}
StoragePreflight input(std::initializer_list<const char *> paths) {
  StoragePreflight result;
  for (const auto path : paths) result.directories[path] = {.directory = path, .eligibleCount = 1};
  result.eligibleCount = paths.size(); return result;
}
auto space = [] { return std::optional<std::uint64_t>(100); };
void encoderLaunchFailureReturnsEveryReservation() {
  StorageWorkBudget budget({.audioWorkers = 1}, space);
  std::atomic<unsigned> runs = 0;
  std::optional<StorageEncodingJob> job;
  auto work = [&] { ++runs; return EncodeResult{}; };
  encodingAllocations = 0; observeEncodingAllocations = true;
  job.emplace(std::move(*budget.Acquire(MediaKind::Audio, 100, {})), work);
  observeEncodingAllocations = false;
  const auto allocations = encodingAllocations;
  job->Take(); job.reset();
  require(allocations != 0 && runs == 1, "real asynchronous encoder startup allocates owned state");
  for (std::size_t index = 0; index < allocations; ++index) {
    failedEncodingAllocation = index; encodingAllocations = 0; observeEncodingAllocations = true;
    bool threw = false;
    try { job.emplace(std::move(*budget.Acquire(MediaKind::Audio, 100, {})), work); }
    catch (...) { threw = true; }
    observeEncodingAllocations = false;
    require(threw && !job && runs == 1, "failed encoder startup never executes work");
    bool waiting;
    require(budget.TryAcquire(MediaKind::Audio, 100, waiting).has_value(), "failed launch releases both scratch and codec admission");
  }
  failedEncodingAllocation = static_cast<std::size_t>(-1);
}
void twoVideosLeaveAudioCapacity() {
  StorageWorkBudget budget({.audioWorkers = 1}, space);
  auto first = budget.Acquire(MediaKind::Video, 10, {});
  std::mutex mutex; std::condition_variable changed;
  bool secondEntered = false, release = false;
  std::thread second([&] {
    auto lease = budget.Acquire(MediaKind::Video, 10, {});
    std::unique_lock lock(mutex); secondEntered = lease.has_value(); changed.notify_all();
    changed.wait(lock, [&] { return release; });
  });
  auto audio = budget.Acquire(MediaKind::Audio, 10, {});
  bool overlapped, capped, waiting;
  { std::unique_lock lock(mutex);
    overlapped = changed.wait_for(lock, 2s, [&] { return secondEntered; });
    capped = !budget.TryAcquire(MediaKind::Video, 10, waiting) && waiting;
    release = true; changed.notify_all(); }
  first.reset(); second.join();
  require(overlapped && capped && audio.has_value(), "two videos overlap with audio while a third video waits");
}
void completedEncodersRetainScratchUntilConsumed() {
  StorageWorkBudget budget({.audioWorkers = 1}, space);
  bool waiting = false;
  {
    StorageEncodingJob first(std::move(*budget.Acquire(MediaKind::Video, 40, {})), [] { return EncodeResult{}; });
    StorageEncodingJob failed(std::move(*budget.Acquire(MediaKind::Video, 40, {})), []() -> EncodeResult {
      throw std::runtime_error("encoder failed");
    });
    first.Take();
    bool caught = false;
    try { failed.Take(); } catch (const std::runtime_error &) { caught = true; }
    require(caught, "encoder exception reaches the owner after worker cleanup");
    require(budget.TryAcquire(MediaKind::Video, 20, waiting).has_value(), "completed encoders release both video slots");
    require(!budget.TryAcquire(MediaKind::Audio, 21, waiting) && waiting,
        "completed and failed jobs retain scratch until their outputs are consumed");
  }
  require(budget.TryAcquire(MediaKind::Audio, 100, waiting).has_value(), "job destruction releases every scratch reservation");
  auto output = budget.Acquire(MediaKind::Audio, 40, {});
  auto other = budget.Acquire(MediaKind::Video, 40, {});
  require(output->GrowTo(60) && !output->GrowTo(61), "growing output shares the remaining scratch capacity with other jobs");
  other.reset();
  require(output->GrowTo(100), "output growth can use scratch released by a settled peer");
}
void independentDirectoriesOverlapWithinBudget() {
  std::mutex mutex; std::condition_variable changed; int entered = 0;
  std::uint64_t done = 0; std::atomic<int> callbacks = 0;
  auto result = RunStorageDirectories(input({"/a", "/b"}), {.audioWorkers = 2}, space,
      [&](const auto &, auto &budget, auto check, auto progress) {
        auto lease = budget.Acquire(MediaKind::Audio, 40, check);
        require(lease.has_value(), "scratch and audio slot admitted");
        std::unique_lock lock(mutex); ++entered; changed.notify_all();
        require(changed.wait_for(lock, 2s, [&] { return entered == 2; }), "independent directories overlap");
        lock.unlock(); progress({.filesDone = 1});
        StorageReport report; report.converted = 1; report.files.resize(80); return report;
      }, {}, [&](const auto &value) {
        require(++callbacks == 1 && value.filesDone >= done && value.filesDone <= 2,
            "progress is serialized and aggregate completed count is monotonic");
        done = value.filesDone; --callbacks;
      });
  require(done == 2, "progress includes both workers");
  require(result.converted == 2 && result.files.size() == 100, "aggregate counters survive bounded details");
}
void directoryAndVideoLimitsAreRespected() {
  std::mutex mutex; std::set<std::filesystem::path> active; std::set<std::filesystem::path> done;
  std::atomic<int> videos = 0, visits = 0;
  auto result = RunStorageDirectories(input({"/a", "/a/child", "/b", "/b/child"}), {.audioWorkers = 4}, space,
      [&](const auto &summary, auto &budget, auto check, auto) {
        { std::lock_guard lock(mutex);
          if (summary.directory.parent_path() != "/") require(done.contains(summary.directory.parent_path()), "parent settles before child");
          require(active.insert(summary.directory).second, "directory runs once"); }
        { auto lease = budget.Acquire(MediaKind::Video, 10, check);
          require(lease.has_value() && ++videos <= 2, "at most two videos and bounded scratch at a time");
          --videos; }
        { std::lock_guard lock(mutex); active.erase(summary.directory); done.insert(summary.directory); }
        ++visits; return StorageReport{};
      });
  require(visits == 4 && !result.recoveryRequired, "all unique directories visited");
}
void reservationsWaitReleaseAndRecheckSpace() {
  for (const auto kind : {MediaKind::Audio, MediaKind::Video}) {
    std::mutex mutex; std::condition_variable changed;
    int checks = 0; bool completed = false, admitted = false;
    std::atomic<std::uint64_t> free = 100;
    StorageWorkBudget budget({.audioWorkers = 2}, [&] { return std::optional<std::uint64_t>(free); });
    const std::uint64_t bytes = kind == MediaKind::Video ? 10 : 60;
    auto first = budget.Acquire(kind, bytes, {});
    require(first.has_value(), "initial scratch reservation admitted");
    std::optional<StorageWorkBudget::Lease> otherVideo;
    if (kind == MediaKind::Video) otherVideo.emplace(std::move(*budget.Acquire(kind, bytes, {})));
    std::thread waiter([&] {
      auto second = budget.Acquire(kind, bytes, [&] {
        std::lock_guard lock(mutex); ++checks; changed.notify_all(); return true;
      });
      std::lock_guard lock(mutex); admitted = second.has_value(); completed = true; changed.notify_all();
    });
    bool waited;
    { std::unique_lock lock(mutex);
      waited = changed.wait_for(lock, 2s, [&] { return checks >= 2 || completed; }) && !completed;
      free = bytes - 1; }
    first.reset(); otherVideo.reset(); waiter.join();
    require(waited && !admitted, "waiting reservation rechecks reduced real space after first lease releases");
    free = 100;
    require(budget.Acquire(kind, 100, {}).has_value(), "failed admission releases all slots and bytes");
  }
}
void activeWorkDrainsBeforeReturn() {
  std::mutex mutex; std::condition_variable changed;
  int entered = 0, settled = 0;
  std::atomic<bool> cancel = false;
  auto result = RunStorageDirectories(input({"/a", "/a/child", "/b", "/b/child"}), {.audioWorkers = 2}, space,
      [&](const auto &, auto &budget, auto check, auto) {
        auto lease = budget.Acquire(MediaKind::Audio, 40, check);
        require(lease.has_value(), "both active transactions reserved");
        std::unique_lock lock(mutex); ++entered; changed.notify_all();
        require(changed.wait_for(lock, 2s, [&] { return entered == 2; }), "both workers active before cancellation");
        cancel = true; ++settled; return StorageReport{};
      }, [&] { return !cancel; });
  require(result.cancelled && !result.failed && entered == 2 && settled == 2, "job waits for both active transactions and stops queued work");
}
void cancelStopsAdmissionAndDrainsTransactions() {
  std::atomic<bool> cancel = false; int visits = 0; bool settled = false;
  auto result = RunStorageDirectories(input({"/a", "/a/child", "/a/other"}), {.audioWorkers = 1}, space,
      [&](const auto &, auto &budget, auto check, auto) {
        ++visits; auto lease = budget.Acquire(MediaKind::Audio, 50, check);
        require(lease.has_value(), "first worker admitted"); cancel = true;
        require(!check(), "active work sees cancellation"); settled = true; return StorageReport{};
      }, [&] { return !cancel; });
  require(visits == 1 && settled && result.cancelled, "cancellation drains active work without admitting more");
}
void recoverySignalStopsActiveFileAdmission() {
  std::mutex mutex; std::condition_variable changed; int entered = 0;
  bool signalled = false;
  auto result = RunStorageDirectories(input({"/a", "/a/child", "/b"}), {.audioWorkers = 2}, space,
      [&](const auto &summary, auto &budget, auto check, auto) {
        std::unique_lock lock(mutex); ++entered; changed.notify_all();
        require(changed.wait_for(lock, 2s, [&] { return entered == 2; }), "both directories started");
        if (summary.directory == "/a") {
          budget.StopForRecovery(); signalled = true; changed.notify_all();
        } else require(changed.wait_for(lock, 2s, [&] { return signalled; }), "recovery signal published");
        require(!check(), "recovery signal stops file admission before either directory returns");
        return StorageReport{};
      });
  require(result.recoveryRequired && !result.failed && entered == 2, "fatal signal drains active directories and leaves queued work untouched");
}
void workerExceptionsBecomeRecoveryReports() {
  auto result = RunStorageDirectories(input({"/a", "/b"}), {.audioWorkers = 2}, space,
      [](const auto &, auto &, auto, auto) { return StorageReport{}; }, []() -> bool { throw std::runtime_error("checkpoint failure"); });
  require(result.recoveryRequired && result.failed, "checkpoint exception drains the pool and retains recovery admission");
  result = RunStorageDirectories(input({"/a"}), {}, space,
      [](const auto &, auto &, auto, auto) -> StorageReport { throw 42; });
  require(result.recoveryRequired && result.failed == 1, "nonstandard worker exception is also contained");
}
void startupFailureRequiresRecoveryOnlyAfterWorkersStart() {
  for (const bool startWorker : {false, true}) {
    std::mutex mutex; std::condition_variable changed;
    bool entered = false, settled = false; int launches = 0;
    auto result = RunStorageDirectories(input({"/a", "/b"}), {.audioWorkers = 2}, space,
        [&](const auto &, auto &, auto check, auto) {
          { std::lock_guard lock(mutex); entered = true; changed.notify_all(); }
          while (check()) std::this_thread::yield();
          settled = true; return StorageReport{};
        }, {}, {}, [&](std::function<void()> worker) -> std::thread {
          if (startWorker && launches++ == 0) return std::thread(std::move(worker));
          if (startWorker) {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, 2s, [&] { return entered; }), "first worker starts before later launch fails");
          }
          throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        });
    require(result.failed == 1 && !result.cancelled && result.recoveryRequired == startWorker,
        "failed first worker launch must not require recovery when no work began");
    require(result.files.size() == 1 && result.files.front().reason.code ==
        (startWorker ? StorageReason::RecoveryRequired : StorageReason::IoError),
        "startup failure reports whether a transaction could have begun");
    require(entered == startWorker && settled == startWorker, "all launched workers settle before startup failure returns");
  }
}
void filesystemRootIsAnAncestor() {
  std::mutex mutex; std::condition_variable changed;
  bool parentDone = false, childStarted = false; int checks = 0;
  auto result = RunStorageDirectories(input({"/", "/a"}), {.audioWorkers = 2}, space,
      [&](const auto &summary, auto &, auto, auto) {
        std::unique_lock lock(mutex);
        if (summary.directory == "/") {
          require(changed.wait_for(lock, 2s, [&] { return checks >= 4 || childStarted; }), "idle worker observes checkpoint while parent is active");
          parentDone = true;
        } else {
          childStarted = true; changed.notify_all();
          require(parentDone, "filesystem root settles before descendant");
        }
        return StorageReport{};
      }, [&] { std::lock_guard lock(mutex); ++checks; changed.notify_all(); return true; });
  require(!result.failed && parentDone && childStarted, "root dependency is included");
}
void recoveryFailureStopsOtherAdmissions() {
  int visits = 0;
  auto result = RunStorageDirectories(input({"/a", "/a/child", "/a/other"}), {.audioWorkers = 1}, space,
      [&](const auto &, auto &, auto, auto) { ++visits; StorageReport result; result.recoveryRequired = true; return result; });
  require(visits == 1 && result.recoveryRequired, "unresolved journal halts other admissions");
}
}
int main(int argc, char **argv) {
  try {
    const std::string filter = argc > 1 ? argv[1] : "";
    if (filter.empty() || filter == "startup") encoderLaunchFailureReturnsEveryReservation();
    if (filter.empty() || filter == "video-overlap") twoVideosLeaveAudioCapacity();
    if (filter.empty() || filter == "video-overlap") completedEncodersRetainScratchUntilConsumed();
    if (filter.empty() || filter == "defaults") platformDefaultsUseExistingWorkerPolicy();
    if (filter.empty() || filter == "exception") workerExceptionsBecomeRecoveryReports();
    if (filter.empty() || filter == "startup") startupFailureRequiresRecoveryOnlyAfterWorkersStart();
    if (filter.empty() || filter == "overlap") independentDirectoriesOverlapWithinBudget();
    if (filter.empty() || filter == "serialize") directoryAndVideoLimitsAreRespected();
    if (filter.empty()) { reservationsWaitReleaseAndRecheckSpace(); activeWorkDrainsBeforeReturn(); recoverySignalStopsActiveFileAdmission(); filesystemRootIsAnAncestor(); }
    if (filter.empty() || filter == "cancel") cancelStopsAdmissionAndDrainsTransactions();
    if (filter.empty() || filter == "recovery") recoveryFailureStopsOtherAdmissions();
    std::cout << "storage work scheduler tests passed\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
