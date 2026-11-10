#include "DownloadCoordinator.h"
#include "DownloadUrlPolicy.h"
#include "../AtomicFile.h"
#include <algorithm>
#include <fstream>

namespace downloads {
namespace {
bool completeFile(const std::filesystem::path &path) {
  std::error_code error;
  return std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) && !error;
}
}
DownloadCoordinator::DownloadCoordinator(std::filesystem::path root, CoordinatorHooks hooks)
    : store_(std::move(root)), hooks_(std::move(hooks)) {
  for (auto job : store_.jobs()) {
    // Only acceptFile can move an OS-completed file to this attempt's path.
    // A process death after rename but before the manifest write needs no scan.
    if (job.state == State::Transferring && completeFile(store_.archivePath(job.id, job.attempt))) {
      job.state = State::Downloaded;
      job.downloadedBytes = std::filesystem::file_size(store_.archivePath(job.id, job.attempt));
      store_.update(job);
    } else if (job.state == State::Importing) {
      job.state = State::NeedsAttention;
      job.message = "Import interrupted. Retry to continue from the downloaded archive.";
      store_.update(job);
    }
  }
  worker_ = std::jthread([this] { importLoop(); });
}
DownloadCoordinator::~DownloadCoordinator() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    interrupted_ = true;
  }
  changed_.notify_all();
  if (worker_.joinable()) worker_.join();
}
Job DownloadCoordinator::submit(Request request) {
  Job job;
  {
    std::lock_guard lock(mutex_);
    job = store_.submit(std::move(request));
  }
  pumpTransfers();
  return job;
}
std::vector<Job> DownloadCoordinator::jobs() const {
  std::lock_guard lock(mutex_);
  auto jobs = store_.jobs();
  for (auto &job : jobs) {
    applyTransientStateLocked(job);
    if (job.state != State::Importing) continue;
    if (const auto progress = importProgress_.find(job.id); progress != importProgress_.end()) {
      job.message = progress->second.message;
      job.downloadedBytes = progress->second.downloadedBytes;
      job.totalBytes = progress->second.totalBytes;
    }
  }
  return jobs;
}
std::optional<Job> DownloadCoordinator::get(const std::string &id) const {
  std::lock_guard lock(mutex_);
  auto job = store_.get(id);
  if (job) applyTransientStateLocked(*job);
  return job;
}
void DownloadCoordinator::applyTransientStateLocked(Job &job) const {
  if (failedHandoffs_.contains(job.id)) { job.state = State::Failed; job.message = persistenceError_; }
  job.persistenceBlocked = !persistenceError_.empty() && activeImport_ != job.id &&
      (job.state == State::Downloaded || job.state == State::Importing);
  if (job.persistenceBlocked) job.message = persistenceError_;
}
std::string DownloadCoordinator::error() const { std::lock_guard lock(mutex_); return persistenceError_; }
std::filesystem::path DownloadCoordinator::archivePath(const std::string &id) const {
  const auto job = store_.get(id);
  if (!job) throw std::runtime_error("Download is no longer available.");
  return store_.archivePath(id, job->attempt);
}
std::filesystem::path DownloadCoordinator::jobPath(const std::string &id) const { return store_.jobPath(id); }
void DownloadCoordinator::setForeground(bool foreground) {
  {
    std::lock_guard lock(mutex_);
    if (foreground_ == foreground) return;
    foreground_ = foreground;
    if (!foreground && !continued_.contains(activeImport_)) interrupted_ = true;
  }
  changed_.notify_all();
  pumpTransfers();
}
void DownloadCoordinator::enableImports(bool enabled) {
  {
    std::lock_guard lock(mutex_);
    if (importsEnabled_ == enabled) return;
    importsEnabled_ = enabled;
    if (!enabled) interrupted_ = true;
  }
  changed_.notify_all();
}
bool DownloadCoordinator::tryReserveMaintenance() {
  std::lock_guard lock(mutex_);
  if (stopping_ || maintenanceReserved_ || !activeImport_.empty()) return false;
  const auto jobs = store_.jobs();
  if (std::any_of(jobs.begin(), jobs.end(), [](const auto &job) { return job.publicationPrepared; })) return false;
  maintenanceReserved_ = true;
  return true;
}
void DownloadCoordinator::releaseMaintenance() {
  {
    std::lock_guard lock(mutex_);
    maintenanceReserved_ = false;
  }
  changed_.notify_all();
}
void DownloadCoordinator::setContinuedProcessing(const std::string &id, bool permitted) {
  {
    std::lock_guard lock(mutex_);
    if (permitted) continued_.insert(id);
    else {
      continued_.erase(id);
      if (!foreground_ && activeImport_ == id) interrupted_ = true;
    }
  }
  changed_.notify_all();
}
bool DownloadCoordinator::cancel(const std::string &id) {
  Job cancelled;
  {
    std::lock_guard lock(mutex_);
    auto job = store_.get(id);
    if (!job || job->state == State::Cancelled || job->state == State::Complete || job->publicationPrepared) return false;
    job->state = State::Cancelled;
    job->result.reset();
    job->decision.reset();
    job->message = "Download cancelled.";
    if (!store_.update(*job)) return false;
    failedHandoffs_.erase(id);
    cancelled = *job;
    if (activeImport_ == id) interrupted_ = true;
    continued_.erase(id);
  }
  if (hooks_.cancelTransfer) hooks_.cancelTransfer(cancelled);
  finishActivity(cancelled, false);
  changed_.notify_all();
  pumpTransfers();
  return true;
}
void DownloadCoordinator::interrupt(const std::string &id, const std::string &message) {
  std::optional<Job> interrupted;
  {
    std::lock_guard lock(mutex_);
    auto job = store_.get(id);
    if (!job || terminal(job->state)) return;
    continued_.erase(id);
    explicitlyInterrupted_.insert(id);
    if (activeImport_ == id) interrupted_ = true;
    else job->state = job->state == State::Transferring ? State::Failed : State::NeedsAttention;
    job->message = message;
    if (store_.update(*job)) interrupted = *job;
  }
  if (interrupted) {
    if (hooks_.cancelTransfer) hooks_.cancelTransfer(*interrupted);
    finishActivity(*interrupted, false);
  }
  changed_.notify_all();
  pumpTransfers();
}
bool DownloadCoordinator::retry(const std::string &id) {
  {
    std::lock_guard lock(mutex_);
    auto job = store_.get(id);
    if (foreground_ && job && job->state == State::Complete && !job->libraryAcknowledged && !job->libraryError.empty()) {
      job->libraryError.clear();
      return store_.update(*job);
    }
    if (!foreground_ || !job || activeImport_ == id ||
        (!failedHandoffs_.contains(id) && job->state != State::Failed && job->state != State::NeedsAttention &&
         !(!persistenceError_.empty() && (job->state == State::Downloaded || job->state == State::Importing)))) return false;
    if (job->decision == BmsSearchPendingArtifactDecision::Delete && !job->publicationPrepared &&
        job->result && job->result->pendingArtifact) {
      // A crash may have occurred after unlinking the retained archive. Finish
      // the saved deletion instead of fetching or importing the package again.
      job->state = State::Downloaded;
    } else if (job->publicationPrepared && job->result && job->result->pendingArtifact) {
      job->decision = BmsSearchPendingArtifactDecision::Keep;
      job->state = State::Downloaded;
    } else if (completeFile(store_.archivePath(id, job->attempt))) {
      job->state = State::Downloaded;
      job->result.reset();
    } else {
      ++job->attempt;
      job->state = State::Queued;
      job->downloadedBytes = job->totalBytes = 0;
      job->result.reset();
    }
    job->message.clear();
    if (!store_.update(*job)) return false;
    failedHandoffs_.erase(id);
    persistenceError_.clear();
    explicitlyInterrupted_.erase(id);
  }
  changed_.notify_all();
  pumpTransfers();
  return true;
}
bool DownloadCoordinator::decide(const std::string &id, BmsSearchPendingArtifactDecision decision) {
  {
    std::lock_guard lock(mutex_);
    auto job = store_.get(id);
    if (!foreground_ || !job || job->state != State::NeedsAttention || job->publicationPrepared ||
        !job->result || !job->result->pendingArtifact) return false;
    job->decision = decision;
    job->state = State::Downloaded;
    if (!store_.update(*job)) return false;
  }
  changed_.notify_all();
  return true;
}
bool DownloadCoordinator::forget(const std::string &id) {
  std::lock_guard lock(mutex_);
  if (activeImport_ == id) return false;
  auto job = store_.get(id);
  if (job && failedHandoffs_.contains(id)) {
    job->state = State::Cancelled;
    job->result.reset();
    if (!store_.update(*job)) return false;
    failedHandoffs_.erase(id);
    if (failedHandoffs_.empty()) persistenceError_.clear();
  }
  if (!job || !terminal(job->state) || job->publicationPrepared ||
      (job->state == State::Complete && !job->libraryAcknowledged)) return false;
  if (hooks_.cleanup) hooks_.cleanup(*job);
  return store_.forget(id);
}
bool DownloadCoordinator::isCurrentTransfer(const std::string &id, std::uint64_t attempt) const {
  std::lock_guard lock(mutex_);
  if (failedHandoffs_.contains(id)) return false;
  const auto job = store_.get(id);
  return job && job->state == State::Transferring && job->attempt == attempt;
}
void DownloadCoordinator::transferProgress(const std::string &id, std::uint64_t attempt,
                                           std::uint64_t bytes, std::uint64_t total) {
  std::optional<Job> progress;
  {
    std::lock_guard lock(mutex_);
    auto job = store_.get(id);
    if (!job || job->state != State::Transferring || job->attempt != attempt) return;
    // Persist coarse snapshots; native UI may publish finer-grained progress.
    if (bytes < job->downloadedBytes ||
        (bytes - job->downloadedBytes < 1024 * 1024 && total == job->totalBytes && bytes != total)) return;
    job->downloadedBytes = bytes; job->totalBytes = total;
    store_.update(*job);
    progress = std::move(job);
  }
  if (hooks_.reportProgress) hooks_.reportProgress(*progress);
}
bool DownloadCoordinator::acceptFile(const std::string &id, std::uint64_t attempt,
                                     const std::filesystem::path &completedFile) {
  try {
    std::lock_guard lock(mutex_);
    auto job = store_.get(id);
    if (!job || job->state != State::Transferring || job->attempt != attempt || !completeFile(completedFile)) return false;
    const auto destination = store_.archivePath(id, attempt);
    std::string error;
    if (completedFile != destination) {
      if (!atomic_file::syncFile(completedFile, error) ||
          !atomic_file::renameDurably(completedFile, destination, error) ||
          !atomic_file::syncDirectory(destination.parent_path(), error))
        throw std::runtime_error("Could not retain the downloaded archive: " + error);
    }
    job->downloadedBytes = std::filesystem::file_size(destination);
    job->totalBytes = job->downloadedBytes;
    job->state = State::Downloaded;
    job->message = "Download complete. Ready to import.";
    if (!store_.update(*job)) return false;
  }
  catch (...) {
    std::optional<Job> failed;
    {
      std::lock_guard lock(mutex_);
      const auto job = store_.get(id);
      if (job && job->attempt == attempt && job->state == State::Transferring) {
        failedHandoffs_.insert(id);
        persistenceError_ = "Could not retain download state. Free storage and retry; completed files will be recovered.";
        failed = job;
      }
    }
    if (failed) finishActivity(*failed, false);
    throw;
  }
  changed_.notify_all();
  return true;
}
void DownloadCoordinator::transferFailed(const std::string &id, std::uint64_t attempt,
                                         const std::string &message, long httpStatus) {
  std::optional<Job> failed;
  {
    std::lock_guard lock(mutex_);
    auto job = store_.get(id);
    if (!job || job->state != State::Transferring || job->attempt != attempt) return;
    const auto failedAttempt = *job;
    job->state = State::Failed;
    job->message = message.substr(0, 8192);
    try {
      if (store_.update(*job)) failed = failedAttempt;
    } catch (...) {
      failedHandoffs_.insert(id);
      persistenceError_ = "Could not retain download state. Free storage and retry; completed files will be recovered.";
      failed = failedAttempt;
    }
  }
  if (failed) finishActivity(*failed, false);
  pumpTransfers();
}
std::vector<Job> DownloadCoordinator::readyForLibrary() const {
  auto result = store_.jobs();
  std::erase_if(result, [](const Job &job) {
    return job.state != State::Complete || job.libraryAcknowledged || !job.result || job.result->outputPath.empty();
  });
  return result;
}
bool DownloadCoordinator::acknowledgeLibrary(const std::string &id) {
  std::lock_guard lock(mutex_);
  auto job = store_.get(id);
  if (!job || job->state != State::Complete) return false;
  job->libraryAcknowledged = true;
  return store_.update(*job);
}
bool DownloadCoordinator::setLibraryError(const std::string &id, const std::string &message) {
  std::lock_guard lock(mutex_);
  auto job = store_.get(id);
  if (!job || job->state != State::Complete || job->libraryAcknowledged) return false;
  job->libraryError = message.substr(0, 8192);
  return store_.update(*job);
}
void DownloadCoordinator::pumpTransfers() {
  std::optional<Job> next;
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || !foreground_ || !hooks_.startTransfer || !activeImport_.empty()) return;
    const auto jobs = store_.jobs();
    if (std::any_of(jobs.begin(), jobs.end(), [](const Job &job) {
          return job.state == State::Transferring || job.state == State::Downloaded || job.state == State::Importing;
        })) return;
    for (auto job : jobs) {
      if (job.state != State::Queued) continue;
      job.state = State::Transferring;
      try {
        if (store_.update(job)) next = store_.get(job.id);
      } catch (...) {
        persistenceError_ = "Could not save download state. Free storage and reopen the app.";
      }
      break;
    }
  }
  if (!next) return;
  const auto error = hooks_.startTransfer(*next, store_.archivePath(next->id, next->attempt));
  if (!error.empty()) transferFailed(next->id, next->attempt, error);
  else {
    const auto current = get(next->id);
    if ((!current || current->state == State::Cancelled) && hooks_.cancelTransfer) hooks_.cancelTransfer(*next);
  }
}
std::optional<Job> DownloadCoordinator::importableLocked() const {
  if (!importsEnabled_ || maintenanceReserved_ || !hooks_.importArchive || stopping_ || !persistenceError_.empty()) return {};
  for (const auto &job : store_.jobs()) {
    if (job.state == State::Downloaded && (foreground_ || continued_.contains(job.id))) return job;
  }
  return {};
}
bool DownloadCoordinator::preparePublication(const std::string &id, const BmsSearchPendingArtifact &artifact) {
  std::lock_guard lock(mutex_);
  auto job = store_.get(id);
  if (!job || job->state != State::Importing || interrupted_.load()) return false;
  job->publicationPrepared = true;
  job->result = BmsSearchResult{.pendingArtifact = artifact};
  return store_.update(*job);
}
void DownloadCoordinator::finishActivity(const Job &job, bool success) noexcept {
  try { if (hooks_.finishActivity) hooks_.finishActivity(job, success); } catch (...) {}
}
void DownloadCoordinator::importLoop() {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      changed_.wait(lock, [this] { return stopping_ || importableLocked().has_value(); });
      if (stopping_) return;
      job = *importableLocked();
      job.state = State::Importing;
      try {
        if (!store_.update(job)) continue;
      } catch (...) {
        persistenceError_ = "Could not save import state. Free storage and retry.";
        continue;
      }
      activeImport_ = job.id;
      interrupted_ = false;
    }
    BmsSearchResult result;
    std::string continuation;
    try {
      if (hooks_.beginImport) hooks_.beginImport(job);
      const BeforePublication prepare = [this, id = job.id](const auto &artifact) { return preparePublication(id, artifact); };
      const auto progress = [this, id = job.id](const BmsSearchDownloadProgress &event) {
        auto current = store_.get(id);
        if (!current) return;
        {
          std::lock_guard lock(mutex_);
          importProgress_[id] = event;
        }
        current->message = event.message;
        current->downloadedBytes = event.downloadedBytes;
        current->totalBytes = event.totalBytes;
        // Import progress is transient; do not fsync once per archive member.
        if (hooks_.reportProgress) hooks_.reportProgress(*current);
      };
      if (!job.confirmationUsed && !job.decision && hooks_.continueTransfer)
        continuation = hooks_.continueTransfer(job, store_.archivePath(job.id, job.attempt));
      if (!continuation.empty() && !allowedRedirect(transferRequest(job).url, continuation))
        throw std::runtime_error("Unsafe download continuation.");
      if (!continuation.empty()) {
        // The coordinator persists a new attempt before asking the OS to fetch
        // it. No importer-owned network work or replay of the original request.
      } else if (job.decision && job.result && job.result->pendingArtifact && hooks_.resolveArtifact)
        result = hooks_.resolveArtifact(job, *job.decision, interrupted_, prepare);
      else result = hooks_.importArchive(job, store_.archivePath(job.id, job.attempt), interrupted_, progress, prepare);
    } catch (const std::exception &) {
      result.status = BmsSearchResult::Status::DownloadFailed;
      result.message = "Import could not finish. Retry to recover the downloaded files.";
    }
    bool success = false;
    {
      std::lock_guard lock(mutex_);
      auto current = store_.get(job.id);
      if (current && current->state == State::Importing) {
        success = !result.outputPath.empty() ||
            (job.decision == BmsSearchPendingArtifactDecision::Delete && !result.pendingArtifact &&
             result.status != BmsSearchResult::Status::DownloadFailed);
        if (!continuation.empty() && !interrupted_) {
          current->transferUrl = continuation;
          current->confirmationUsed = true;
          ++current->attempt;
          current->state = State::Queued;
          current->downloadedBytes = current->totalBytes = 0;
          current->result.reset();
        } else if (success) {
          current->state = State::Complete;
          current->publicationPrepared = false;
          current->result = result;
          current->libraryAcknowledged = result.outputPath.empty();
        } else if (job.decision == BmsSearchPendingArtifactDecision::Delete) {
          current->state = State::NeedsAttention;
          current->result = job.result;
        } else if (current->publicationPrepared) {
          // Keep the prepared artifact so retry can reconcile a file swap.
          current->state = State::NeedsAttention;
        } else if (result.pendingArtifact) {
          current->state = State::NeedsAttention;
          current->result = result;
        } else {
          current->state = interrupted_ && !explicitlyInterrupted_.contains(job.id) ? State::Downloaded : State::NeedsAttention;
          current->result.reset();
        }
        current->message = interrupted_ && !success ? "Import paused. Retry when ready." : result.message;
        if (success || job.decision != BmsSearchPendingArtifactDecision::Delete) current->decision.reset();
        try { store_.update(*current); } catch (...) {
          // The persisted Importing/prepare record and retained archive remain
          // the recovery authority; do not falsely report success to the OS.
          success = false;
          persistenceError_ = "Could not save import completion. Free storage and retry to recover.";
        }
      }
      importProgress_.erase(job.id);
      activeImport_.clear();
      continued_.erase(job.id);
    }
    finishActivity(job, success);
    pumpTransfers();
  }
}
} // namespace downloads
