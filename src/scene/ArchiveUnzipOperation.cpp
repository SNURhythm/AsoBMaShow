#include "ArchiveUnzipOperation.h"

#include "../ArchiveSourceIdentity.h"
#include "../ChartLibraryScanner.h"
#include "../library/ArchiveUnzipRecovery.h"

#include <algorithm>
#include <exception>
#include <map>
#include <utility>

namespace {

bool eligible(const ChartMetaRecord &record) {
  return record.solidArchive && !record.unavailable &&
         !record.meta.BmsPath.empty() &&
         !archive_file::isVirtualPath(record.meta.BmsPath);
}

ArchiveUnzipResult extractArchive(
    const ChartMetaRecord &record, const std::stop_token &stopToken,
    archive_file::UnzipProgressCallback progress, bool reuseCompletedFolder,
    archive_file::UnzipPrepareCallback prepare = nullptr,
    archive_file::UnzipBudget *budget = nullptr,
    archive_file::PauseCallback pause = nullptr) {
  ArchiveUnzipResult result;
  result.archivePath = record.meta.BmsPath;
  result.rootPath = result.archivePath.parent_path();
  if (result.rootPath.empty()) result.rootPath = ".";
  try {
    if (!stopToken.stop_requested()) {
      if (!eligible(record)) {
        result.message = "Selected item is not a solid archive.";
        return result;
      }
      std::string error;
      const auto extracted = archive_file::unzipArchiveFully(
          result.archivePath, result.rootPath, &error, &stopToken, progress,
          pause, reuseCompletedFolder, prepare, budget);
      if (extracted) {
        result.outputFolder = extracted->outputFolder;
        result.archiveKey = extracted->archiveKey;
        result.success = true;
      } else {
        result.message = error.empty() ? "Unzip failed" : "Unzip failed: " + error;
      }
    }
  } catch (const std::exception &error) {
    result.message = "Unzip failed: " + std::string(error.what());
  } catch (...) {
    result.message = "Unzip failed";
  }
  if (stopToken.stop_requested()) {
    result.success = false;
    result.cancelled = true;
    result.message = "Unzip cancelled";
  }
  return result;
}

bool archiveIdentityMatches(const ArchiveUnzipResult &result) {
  return !result.archiveKey.empty() &&
         archive_source_identity::KeyForPath(result.archivePath) == result.archiveKey;
}

bool deleteCompletedArchive(const ArchiveUnzipResult &result,
                            const std::stop_token &stopToken,
                            std::string &message) {
  if (!result.success || result.outputFolder.empty() || result.cancelled ||
      result.archivePath.empty() || stopToken.stop_requested()) {
    message = "Archive is unavailable for deletion";
    return false;
  }
  if (!archiveIdentityMatches(result)) {
    message = "Archive changed or is unavailable. Original archive kept.";
    return false;
  }
  if (stopToken.stop_requested()) {
    message = "Unzip cancelled. Original archive kept.";
    return false;
  }
  std::error_code error;
  if (!std::filesystem::remove(result.archivePath, error)) {
    message = "Could not delete archive" +
              (error ? ": " + error.message() : std::string());
    return false;
  }
  message = "Original archive deleted";
  return true;
}

}

ArchiveUnzipOperation::ArchiveUnzipOperation(ChartRepository &repository)
    : repository_(repository) {}

ArchiveUnzipOperation::~ArchiveUnzipOperation() { cancelAndWait(); }

bool ArchiveUnzipOperation::start(const ChartMetaRecord &record) {
  if (inProgress_ || !eligible(record)) {
    return false;
  }
  cancelAndWait();
  inProgress_ = true;
  try {
    worker_ = std::jthread([this, record](const std::stop_token &stopToken) {
      auto result = Run(record, repository_, stopToken,
                        [this](const archive_file::UnzipProgress &progress) {
        std::lock_guard lock(mutex_);
        pendingProgress_ = progress;
      });
      std::lock_guard lock(mutex_);
      libraryChangedPending_ = libraryChangedPending_ || result.libraryChanged;
      pendingResult_ = std::move(result);
    });
  } catch (...) {
    inProgress_ = false;
    return false;
  }
  return true;
}

bool ArchiveUnzipOperation::inProgress() const { return inProgress_; }

bool ArchiveUnzipOperation::startAll(bool deleteAfterUnzip) {
  if (inProgress_) {
    return false;
  }
  cancelAndWait();
  inProgress_ = true;
  try {
    worker_ = std::jthread([this, deleteAfterUnzip](const std::stop_token &stopToken) {
      auto result = RunAll(repository_, deleteAfterUnzip, stopToken,
                           [this](const archive_file::UnzipProgress &progress) {
        std::lock_guard lock(mutex_);
        pendingProgress_ = progress;
      });
      std::lock_guard lock(mutex_);
      libraryChangedPending_ = libraryChangedPending_ || result.libraryChanged;
      pendingResult_ = std::move(result);
    });
  } catch (...) {
    inProgress_ = false;
    return false;
  }
  return true;
}

bool ArchiveUnzipOperation::takeLibraryChanged() {
  std::lock_guard lock(mutex_);
  return std::exchange(libraryChangedPending_, false);
}

void ArchiveUnzipOperation::requestCancel() {
  if (worker_.joinable()) {
    worker_.request_stop();
  }
}

void ArchiveUnzipOperation::cancelAndWait() {
  requestCancel();
  if (worker_.joinable()) {
    worker_.join();
  }
  std::lock_guard lock(mutex_);
  pendingProgress_.reset();
  pendingResult_.reset();
  pendingDeleteResult_.reset();
  result_.reset();
  inProgress_ = false;
}

std::optional<archive_file::UnzipProgress>
ArchiveUnzipOperation::takeProgress() {
  std::lock_guard lock(mutex_);
  return std::exchange(pendingProgress_, std::nullopt);
}

std::optional<ArchiveUnzipResult> ArchiveUnzipOperation::takeResult() {
  std::optional<ArchiveUnzipResult> result;
  {
    std::lock_guard lock(mutex_);
    result = std::exchange(pendingResult_, std::nullopt);
  }
  if (!result) {
    return std::nullopt;
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  {
    std::lock_guard lock(mutex_);
    pendingProgress_.reset();
  }
  inProgress_ = false;
  result_ = result;
  return result;
}

bool ArchiveUnzipOperation::canDeleteArchive() const {
  if (inProgress_ || !result_ || result_->batch || !result_->success ||
      !result_->scanCommitted || result_->cancelled ||
      result_->archivePath.empty()) {
    return false;
  }
  return archiveIdentityMatches(*result_);
}

bool ArchiveUnzipOperation::startDeleteArchive() {
  if (inProgress_ || !result_ || result_->batch || !result_->success ||
      !result_->scanCommitted || result_->cancelled ||
      result_->archivePath.empty()) {
    return false;
  }
  inProgress_ = true;
  try {
    worker_ = std::jthread([this, completed = *result_](const std::stop_token &stopToken) {
      ArchiveDeleteResult result;
      try {
        auto operationLock = archive_unzip_recovery::acquireOperationLock(stopToken);
        if (!operationLock.owns_lock() || stopToken.stop_requested()) {
          result.message = "Deletion cancelled. Original archive kept.";
        } else {
          auto session = repository_.OpenSession();
          if (!session || !session->EnsureSchema()) {
            result.message = "Could not open library. Original archive kept.";
          } else {
            result.deleted = deleteCompletedArchive(completed, stopToken, result.message);
            if (result.deleted && !session->DeleteArchiveRecords(completed.archivePath)) {
              result.message = "Original archive deleted. Failed to refresh library.";
            }
          }
        }
        result.canRetry = !result.deleted && archiveIdentityMatches(completed);
      } catch (...) {
        result.message = result.deleted
            ? "Original archive deleted. Failed to refresh library."
            : "Could not delete archive. Original archive kept.";
      }
      std::lock_guard lock(mutex_);
      libraryChangedPending_ = libraryChangedPending_ || result.deleted;
      pendingDeleteResult_ = std::move(result);
    });
  } catch (...) {
    inProgress_ = false;
    return false;
  }
  return true;
}

std::optional<ArchiveDeleteResult> ArchiveUnzipOperation::takeDeleteResult() {
  std::optional<ArchiveDeleteResult> result;
  {
    std::lock_guard lock(mutex_);
    result = std::exchange(pendingDeleteResult_, std::nullopt);
  }
  if (!result) {
    return std::nullopt;
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  inProgress_ = false;
  if (result->deleted || !result->canRetry) {
    result_.reset();
  }
  return result;
}

void ArchiveUnzipOperation::keepArchive() { result_.reset(); }

ArchiveUnzipResult ArchiveUnzipOperation::RunAll(
    ChartRepository &repository, bool deleteAfterUnzip,
    const std::stop_token &stopToken,
    archive_file::UnzipProgressCallback progress,
    archive_file::UnzipLimits limits) {
  auto lock = archive_unzip_recovery::acquireOperationLock(stopToken);
  ArchiveUnzipResult result;
  result.batch = true;
  archive_file::UnzipBudget budget{.limits = limits};
  const auto initialRevision = repository.GetLibraryRevision();
  std::string lastError;
  std::vector<std::filesystem::path> completedFolders;
  std::vector<std::filesystem::path> deletedArchives;
  bool queried = false;
  try {
    if (!stopToken.stop_requested()) {
      auto session = repository.OpenSession();
      if (!session || !session->EnsureSchema()) {
        lastError = "Could not open library. Original archives kept.";
      } else {
        ChartMetaQuery query;
        query.solidArchivesOnly = true;
        std::vector<ChartMetaRecord> archives;
        session->QueryChartMeta(query, archives, stopToken);
        queried = true;
        result.archiveCount = archives.size();
        const auto plan = archive_file::unzipExecutionPlan(limits, archives.size());
        budget.concurrentArchives = plan.archiveWorkers;
        std::stop_source workersStop;
        std::mutex resultMutex, progressMutex, journalMutex;
        std::vector<double> fractions(archives.size(), 0.0);
        std::map<std::size_t, std::string> activeArchives;
        std::size_t completedArchives = 0;
        double aggregateFraction = 0.0;
        const auto processArchive = [&](std::size_t archiveIndex) {
          const auto &record = archives[archiveIndex];
          const auto filename = record.meta.BmsPath.filename().string();
          const auto label = filename + " (" + std::to_string(archiveIndex + 1) +
                             "/" + std::to_string(archives.size()) + ") - ";
          const auto publish = [&](const archive_file::UnzipProgress &archiveProgress,
                                   bool finished = false, bool completed = false) {
            if (progress) {
              std::lock_guard progressLock(progressMutex);
              const auto fraction = std::max(fractions[archiveIndex],
                  std::clamp(archiveProgress.fraction, 0.0, 1.0));
              aggregateFraction += fraction - fractions[archiveIndex];
              fractions[archiveIndex] = fraction;
              if (finished) {
                activeArchives.erase(archiveIndex);
                if (completed) ++completedArchives;
              } else {
                activeArchives[archiveIndex] = label + archiveProgress.message;
              }
              archive_file::UnzipProgress snapshot{
                  .fraction = std::min(0.9, 0.9 * aggregateFraction /
                                               static_cast<double>(archives.size())),
                  .current = completedArchives,
                  .total = archives.size(),
                  .message = label + archiveProgress.message,
                  .archiveIndex = archiveIndex + 1,
              };
              for (const auto &[index, message] : activeArchives) {
                snapshot.activeArchives.push_back(message);
              }
              progress(snapshot);
            }
          };
          publish({.fraction = 0.0, .message = "Preparing unzip"});
          const auto archiveResult = extractArchive(record, workersStop.get_token(),
              [&](const archive_file::UnzipProgress &archiveProgress) { publish(archiveProgress); },
              !deleteAfterUnzip,
              [&](const std::filesystem::path &folder, const std::string &key) {
                std::lock_guard journalLock(journalMutex);
                return session->SaveUnzipRecovery({
                    .archivePath = record.meta.BmsPath, .outputFolder = folder,
                    .archiveKey = key, .deleteOriginal = deleteAfterUnzip});
              }, &budget, [&] {
                if (stopToken.stop_requested()) workersStop.request_stop();
                return !workersStop.stop_requested();
              });
          bool completed = false;
          {
            std::lock_guard resultLock(resultMutex);
            if (!archiveResult.outputFolder.empty()) {
              completedFolders.push_back(archiveResult.outputFolder);
              ++result.succeededCount;
              ++result.completedCount;
              completed = true;
            }
            if (!archiveResult.cancelled && !stopToken.stop_requested()) {
              if (!archiveResult.success) {
                ++result.completedCount;
                completed = true;
                ++result.failedCount;
                lastError = filename + ": " + archiveResult.message;
                if (budget.exhausted) workersStop.request_stop();
              } else if (deleteAfterUnzip && !workersStop.stop_requested()) {
                std::string message;
                if (deleteCompletedArchive(archiveResult, stopToken, message)) {
                  deletedArchives.push_back(archiveResult.archivePath);
                  ++result.deletedCount;
                  result.libraryChanged = true;
                } else if (!workersStop.stop_requested()) {
                  ++result.deletionFailedCount;
                  lastError = filename + ": " + message;
                }
              }
            }
          }
          publish({.message = "Archive finished"}, true, completed);
        };
        const auto workerCount = std::min(archives.size(), plan.archiveWorkers);
        std::atomic_size_t nextArchive{workerCount};
        const auto worker = [&](std::size_t archiveIndex) {
          try {
            while (archiveIndex < archives.size() && !stopToken.stop_requested() &&
                   !workersStop.stop_requested() &&
                   !budget.exhausted) {
              processArchive(archiveIndex);
              archiveIndex = nextArchive.fetch_add(1);
            }
          } catch (const std::exception &error) {
            std::lock_guard resultLock(resultMutex);
            lastError = error.what();
            workersStop.request_stop();
          } catch (...) {
            std::lock_guard resultLock(resultMutex);
            lastError = "Unzip All failed";
            workersStop.request_stop();
          }
        };
        if (workerCount == 1) {
          worker(0);
        } else {
          std::vector<std::jthread> workers;
          workers.reserve(workerCount);
          try {
            for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
              workers.emplace_back([&, workerIndex] { worker(workerIndex); });
            }
          } catch (...) {
            workersStop.request_stop();
            throw;
          }
          for (auto &thread : workers) thread.join();
        }
      }
    }
  } catch (const std::exception &error) {
    lastError = error.what();
  } catch (...) {
    lastError = "Unzip All failed";
  }
  const auto extractionLimitError = budget.exhausted ? budget.failureMessage : std::string();
  if (!completedFolders.empty()) {
    try {
      if (progress) {
        progress({.fraction = 0.9, .total = completedFolders.size(),
                  .message = "Indexing extracted folders", .indexing = true});
      }
      auto session = repository.OpenSession();
      if (!session || !session->EnsureSchema()) {
        result.deletionFailedCount += deletedArchives.size();
        lastError = "Failed to index extracted folders: could not open library. Extracted files are kept.";
      } else {
        const bool cleaned = archive_unzip_recovery::deleteArchiveRecords(
            *session, deletedArchives);
        if (!cleaned) {
          result.deletionFailedCount += deletedArchives.size();
          lastError = "Failed to remove deleted archive records. Extracted files are kept; retry a library scan.";
        }
        ChartLibraryScanner scanner;
        const auto scan = scanner.ScanAddedWithResult(
            *session, completedFolders, nullptr,
            [&](const ChartScanProgress &scanProgress) {
              if (progress) {
                progress({
                    .fraction = 0.95,
                    .current = static_cast<std::uint64_t>(scanProgress.current),
                    .total = static_cast<std::uint64_t>(scanProgress.total),
                    .message = "Indexing extracted charts",
                    .indexing = true,
                });
              }
            }, nullptr, nullptr, nullptr, true);
        result.scanCommitted = scan.committed;
        result.libraryChanged = result.libraryChanged ||
                                (scan.committed && scan.changedCount > 0);
        if (!scan.completed || !scan.committed) {
          lastError = "Failed to index extracted folders. Extracted files are kept; retry a library scan.";
        } else if (cleaned && !session->ClearUnzipRecovery(completedFolders)) {
          lastError = "Library refreshed, but recovery work could not be acknowledged; it will retry on startup.";
        }
      }
    } catch (const std::exception &error) {
      lastError = "Failed to index extracted folders: " + std::string(error.what());
    } catch (...) {
      lastError = "Failed to index extracted folders. Extracted files are kept.";
    }
  }
  if (!extractionLimitError.empty() && lastError.find(extractionLimitError) == std::string::npos) {
    lastError = extractionLimitError + " " + lastError;
  }
  result.cancelled = stopToken.stop_requested();
  result.libraryChanged = result.libraryChanged ||
                          repository.GetLibraryRevision() != initialRevision;
  result.success = queried && !result.cancelled && lastError.empty() &&
                   result.completedCount == result.archiveCount;
  result.message = result.cancelled ? "Unzip All cancelled. " : "Unzip All: ";
  result.message += "Unzipped " + std::to_string(result.succeededCount) + "/" +
                    std::to_string(result.archiveCount) + "; failed " +
                    std::to_string(result.failedCount);
  if (deleteAfterUnzip) {
    result.message += "; deleted " + std::to_string(result.deletedCount) +
                      "; delete/refresh failures " +
                      std::to_string(result.deletionFailedCount);
  } else {
    result.message += "; originals kept";
  }
  if (!completedFolders.empty() && result.scanCommitted) {
    result.message += "; library indexed";
  }
  if (!lastError.empty()) {
    result.message += ". " + lastError;
  }
  return result;
}

ArchiveUnzipResult ArchiveUnzipOperation::Run(
    const ChartMetaRecord &record, ChartRepository &repository,
    const std::stop_token &stopToken,
    archive_file::UnzipProgressCallback progress,
    bool reuseCompletedFolder) {
  auto lock = archive_unzip_recovery::acquireOperationLock(stopToken);
  const auto initialRevision = repository.GetLibraryRevision();
  auto result = extractArchive(record, stopToken, progress, reuseCompletedFolder,
      [&](const std::filesystem::path &folder, const std::string &key) {
        auto session = repository.OpenSession();
        return session && session->EnsureSchema() && session->SaveUnzipRecovery({
            .archivePath = record.meta.BmsPath, .outputFolder = folder,
            .archiveKey = key, .deleteOriginal = false});
      });
  auto finish = [&]() {
    result.libraryChanged = result.libraryChanged ||
                            repository.GetLibraryRevision() != initialRevision;
    return result;
  };
  auto cancel = [&]() {
    result.success = false;
    result.cancelled = true;
    result.message = "Unzip cancelled";
    result.chartPath.clear();
  };
  if (!result.success) return finish();
  result.success = false;
  try {
    if (stopToken.stop_requested()) {
      cancel();
      return finish();
    }
    auto session = repository.OpenSession();
    if (!session || !session->EnsureSchema()) {
      result.message = "Unzipped archive. Failed to refresh library.";
      return finish();
    }
    if (progress) {
      progress({.fraction = 0.98, .message = "Refreshing library"});
    }
    ChartLibraryScanner scanner;
    const auto scan = scanner.ScanAddedWithResult(
        *session, {result.outputFolder}, &stopToken,
        [&](const ChartScanProgress &scanProgress) {
          if (progress) {
            progress({
                .fraction = 0.98,
                .current = static_cast<std::uint64_t>(scanProgress.current),
                .total = static_cast<std::uint64_t>(scanProgress.total),
                .message = scanProgress.stage == ChartScanProgressStage::ParsingCharts
                               ? "Indexing extracted charts"
                               : "Refreshing library",
            });
          }
        }, nullptr, nullptr, nullptr, true);
    result.scanCommitted = scan.committed;
    result.libraryChanged = scan.committed && scan.changedCount > 0;
    if (stopToken.stop_requested()) {
      cancel();
      return finish();
    }
    if (!scan.completed || !scan.committed) {
      result.message = "Unzipped archive. Failed to refresh library.";
      return finish();
    }
    ChartMetaQuery query;
    query.recursiveFolder = result.outputFolder;
    query.rawSongData = true;
    query.limit = 1;
    std::vector<ChartMetaRecord> charts;
    session->QueryChartMeta(query, charts, stopToken);
    if (stopToken.stop_requested()) {
      cancel();
      return finish();
    }
    if (!session->ClearUnzipRecovery({&result.outputFolder, 1})) {
      result.message = "Unzipped archive. Library refreshed, but recovery work could not be acknowledged; it will retry on startup.";
      return finish();
    }
    if (!charts.empty()) {
      result.chartPath = charts.front().meta.BmsPath;
    }
    result.success = true;
    result.message = scan.changedCount > 0
                         ? "Unzipped archive. Library refreshed."
                         : "Unzipped archive. Library already current.";
  } catch (const std::exception &error) {
    result.message = "Unzip failed: " + std::string(error.what());
  } catch (...) {
    result.message = "Unzip failed";
  }
  if (stopToken.stop_requested()) {
    cancel();
  }
  return finish();
}
