#include "StorageWorkScheduler.h"
#include "../utils/ParallelWork.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <thread>
#include <utility>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace storage_management {
StorageExecutionLimits DefaultStorageExecutionLimits() {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
  // Match chart audio loading, including its headroom for foreground threads.
  return {.audioWorkers = parallel_worker_count(std::numeric_limits<std::size_t>::max())};
#else
  return {.audioWorkers = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 4)};
#endif
}
StorageWorkBudget::StorageWorkBudget(StorageExecutionLimits limits, std::function<std::optional<std::uint64_t>()> available)
    : limits_(limits), available_(std::move(available)) { limits_.audioWorkers = std::max<std::size_t>(1, limits_.audioWorkers); }
StorageWorkBudget::Lease::Lease(Lease &&other) noexcept : owner_(std::exchange(other.owner_, nullptr)), kind_(other.kind_), bytes_(other.bytes_) {}
StorageWorkBudget::Lease::~Lease() {
  if (!owner_) return;
  { std::lock_guard lock(owner_->mutex_);
    --(kind_ == MediaKind::Video ? owner_->video_ : owner_->audio_);
    owner_->reserved_ -= bytes_; }
  owner_->changed_.notify_all();
}
void StorageWorkBudget::StopForRecovery() { recoveryRequired_ = true; changed_.notify_all(); }
std::optional<StorageWorkBudget::Lease> StorageWorkBudget::Acquire(MediaKind kind, std::uint64_t bytes, Checkpoint check) {
  for (;;) {
    if (recoveryRequired_ || (check && !check())) return {};
    std::unique_lock lock(mutex_);
    const auto available = available_();
    if (!available) return {};
    auto &active = kind == MediaKind::Video ? video_ : audio_;
    const auto slots = kind == MediaKind::Video ? StorageExecutionLimits::videoWorkers : limits_.audioWorkers;
    if (recoveryRequired_) return {};
    if (active < slots && reserved_ <= *available && bytes <= *available - reserved_) {
      ++active; reserved_ += bytes; return Lease(this, kind, bytes);
    }
    // A reservation can finish and release both its capacity and temporary file.
    // With no active work, insufficient space cannot improve within this job.
    if (!audio_ && !video_) return {};
    changed_.wait_for(lock, std::chrono::milliseconds(50));
  }
}
namespace {
void merge(StorageReport &target, StorageReport source) {
  target.converted += source.converted; target.deleted += source.deleted;
  target.skipped += source.skipped; target.failed += source.failed;
  AccumulateStorageBytes(target.reclaimedBytes, target.addedBytes, source.reclaimedBytes, source.addedBytes);
  target.cancelled |= source.cancelled; target.recoveryRequired |= source.recoveryRequired;
  target.cleanupPending |= source.cleanupPending;
  for (auto &file : source.files) if (target.files.size() < 100) target.files.push_back(std::move(file));
  target.changedRoots.insert(target.changedRoots.end(), source.changedRoots.begin(), source.changedRoots.end());
  target.groupDeltas.insert(target.groupDeltas.end(), std::make_move_iterator(source.groupDeltas.begin()), std::make_move_iterator(source.groupDeltas.end()));
}
}
StorageReport RunStorageDirectories(const StoragePreflight &inventory, StorageExecutionLimits limits,
    std::function<std::optional<std::uint64_t>()> available, DirectoryWork work, Checkpoint check, ProgressCallback progress) {
  struct Job { const StorageDirectorySummary *summary; std::vector<std::size_t> children; std::uint64_t done = 0, reclaimed = 0, added = 0; };
  std::vector<Job> jobs;
  std::map<std::filesystem::path, std::size_t> positions;
  for (const auto &[path, summary] : inventory.directories) if (summary.eligibleCount) {
    positions.emplace(path, jobs.size()); jobs.push_back({&summary, {}});
  }
  std::deque<std::size_t> ready;
  for (const auto &[path, position] : positions) {
    auto parent = path.parent_path(); bool dependent = false;
    if (parent == path) parent.clear();
    while (!parent.empty()) {
      if (const auto found = positions.find(parent); found != positions.end()) {
        jobs[found->second].children.push_back(position); dependent = true; break;
      }
      if (parent == parent.parent_path()) break;
      parent = parent.parent_path();
    }
    if (!dependent) ready.push_back(position);
  }
  StorageReport report;
  StorageProgress state{.filesTotal = inventory.eligibleCount};
  StorageWorkBudget budget(limits, std::move(available));
  std::mutex mutex;
  std::condition_variable changed;
  std::atomic<bool> cancelled = false, fatal = false;
  std::size_t finished = 0;
  Checkpoint checkpoint = [&] {
    if (budget.RecoveryRequired()) fatal = true;
    if (cancelled || fatal) return false;
    if (check && !check()) { cancelled = true; changed.notify_all(); return false; }
    return !cancelled && !fatal;
  };
  auto workerLoop = [&] {
    for (;;) {
      if (!checkpoint()) return;
      std::unique_lock lock(mutex);
      if (cancelled || fatal || finished == jobs.size()) return;
      if (ready.empty()) { changed.wait_for(lock, std::chrono::milliseconds(50)); continue; }
      const auto index = ready.front(); ready.pop_front(); lock.unlock();
      StorageReport local;
      try {
        local = work(*jobs[index].summary, budget, checkpoint, [&](const StorageProgress &value) {
          std::lock_guard lock(mutex);
          auto &job = jobs[index];
          const auto done = std::max(job.done, value.filesDone);
          state.filesDone += done - job.done;
          AccumulateStorageBytes(state.reclaimedBytes, state.addedBytes, job.added, job.reclaimed);
          AccumulateStorageBytes(state.reclaimedBytes, state.addedBytes, value.reclaimedBytes, value.addedBytes);
          job.done = done; job.reclaimed = value.reclaimedBytes; job.added = value.addedBytes;
          state.phase = value.phase; state.currentPath = value.currentPath;
          if (progress) progress(state);
        });
      } catch (const std::exception &error) {
        local.failed = 1; local.recoveryRequired = true;
        local.files.push_back({.source = jobs[index].summary->directory, .outcome = FileOutcome::Failed,
            .reason = {StorageReason::RecoveryRequired, error.what()}});
      }
      if (local.recoveryRequired) fatal = true;
      if (local.cancelled) cancelled = true;
      lock.lock(); merge(report, std::move(local)); ++finished;
      // Publishing completion under this lock also publishes the updated owned
      // summary. Descendants may read it only after their parent has settled.
      for (const auto child : jobs[index].children) ready.push_back(child);
      lock.unlock(); changed.notify_all();
    }
  };
  auto failedWorker = [&](const char *detail) {
    fatal = true; budget.StopForRecovery(); changed.notify_all();
    std::lock_guard lock(mutex);
    ++report.failed; report.recoveryRequired = true;
    if (report.files.size() < 100) report.files.push_back({.outcome = FileOutcome::Failed,
        .reason = {StorageReason::RecoveryRequired, detail}});
  };
  auto worker = [&] {
    try { workerLoop(); }
    catch (const std::exception &error) { failedWorker(error.what()); }
    catch (...) { failedWorker("Unhandled exception in storage worker"); }
  };
  std::vector<std::thread> workers;
  try {
    const auto count = std::min(jobs.size(), std::max<std::size_t>(1, limits.audioWorkers));
    for (std::size_t i = 0; i < count; ++i) workers.emplace_back(worker);
  } catch (...) {
    failedWorker("Unable to start storage worker");
  }
  for (auto &thread : workers) thread.join();
  report.cancelled |= cancelled; report.recoveryRequired |= fatal || budget.RecoveryRequired();
  return report;
}
}
