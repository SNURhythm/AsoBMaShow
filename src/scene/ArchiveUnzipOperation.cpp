#include "ArchiveUnzipOperation.h"

#include "../ChartLibraryScanner.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace {

bool eligible(const ChartMetaRecord &record) {
  return record.solidArchive && !record.unavailable &&
         !record.meta.BmsPath.empty() &&
         !archive_file::isVirtualPath(record.meta.BmsPath);
}

bool deleteCompletedArchive(const ArchiveUnzipResult &result,
                            ChartRepository::Session &session,
                            const std::stop_token &stopToken,
                            std::string &message, bool &deleted) {
  deleted = false;
  if (!result.success || !result.scanCommitted || result.cancelled ||
      result.archivePath.empty() || stopToken.stop_requested()) {
    message = "Archive is unavailable for deletion";
    return false;
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(result.archivePath, error) || error) {
    message = "Archive is unavailable for deletion";
    return false;
  }
  if (stopToken.stop_requested()) {
    message = "Unzip cancelled. Original archive kept.";
    return false;
  }
  if (!std::filesystem::remove(result.archivePath, error)) {
    message = "Could not delete archive" +
              (error ? ": " + error.message() : std::string());
    return false;
  }
  deleted = true;
  const bool refreshed = session.DeleteArchiveRecords(result.archivePath);
  message = refreshed ? "Original archive deleted"
                      : "Original archive deleted. Failed to refresh library.";
  return refreshed;
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
  std::error_code error;
  return std::filesystem::is_regular_file(result_->archivePath, error) && !error;
}

bool ArchiveUnzipOperation::deleteArchive(std::string &message) {
  if (!canDeleteArchive()) {
    message = "Archive is unavailable for deletion";
    return false;
  }
  auto session = repository_.OpenSession();
  if (!session || !session->EnsureSchema()) {
    message = "Could not open library. Original archive kept.";
    return false;
  }
  bool deleted = false;
  deleteCompletedArchive(*result_, *session, {}, message, deleted);
  if (deleted) {
    result_.reset();
  }
  return deleted;
}

void ArchiveUnzipOperation::keepArchive() { result_.reset(); }

ArchiveUnzipResult ArchiveUnzipOperation::RunAll(
    ChartRepository &repository, bool deleteAfterUnzip,
    const std::stop_token &stopToken,
    archive_file::UnzipProgressCallback progress) {
  ArchiveUnzipResult result;
  result.batch = true;
  const auto initialRevision = repository.GetLibraryRevision();
  std::string lastError;
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
        for (std::size_t archiveIndex = 0; archiveIndex < archives.size(); ++archiveIndex) {
          if (stopToken.stop_requested()) {
            break;
          }
          const auto &record = archives[archiveIndex];
          const auto filename = record.meta.BmsPath.filename().string();
          const auto publish = [&](const archive_file::UnzipProgress &archiveProgress) {
            if (progress) {
              progress({
                  .fraction = (static_cast<double>(archiveIndex) +
                               std::clamp(archiveProgress.fraction, 0.0, 1.0)) /
                              static_cast<double>(archives.size()),
                  .current = archiveIndex + 1,
                  .total = archives.size(),
                  .message = filename + " (" + std::to_string(archiveIndex + 1) +
                             "/" + std::to_string(archives.size()) + ") - " +
                             archiveProgress.message,
              });
            }
          };
          publish({.fraction = 0.0, .message = "Preparing unzip"});
          const auto archiveResult = Run(record, repository, stopToken, publish,
                                         !deleteAfterUnzip);
          result.libraryChanged = result.libraryChanged || archiveResult.libraryChanged;
          result.scanCommitted = result.scanCommitted || archiveResult.scanCommitted;
          if (archiveResult.cancelled || stopToken.stop_requested()) {
            break;
          }
          ++result.completedCount;
          if (!archiveResult.success || !archiveResult.scanCommitted) {
            ++result.failedCount;
            lastError = filename + ": " + archiveResult.message;
            continue;
          }
          ++result.succeededCount;
          if (deleteAfterUnzip) {
            bool deleted = false;
            std::string message;
            const bool refreshed = deleteCompletedArchive(
                archiveResult, *session, stopToken, message, deleted);
            if (deleted) {
              ++result.deletedCount;
              result.libraryChanged = true;
            }
            if (!refreshed && !stopToken.stop_requested()) {
              ++result.deletionFailedCount;
              lastError = filename + ": " + message;
            }
          }
        }
      }
    }
  } catch (const std::exception &error) {
    lastError = error.what();
  } catch (...) {
    lastError = "Unzip All failed";
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
  ArchiveUnzipResult result;
  const auto initialRevision = repository.GetLibraryRevision();
  auto finish = [&]() {
    result.libraryChanged = result.libraryChanged ||
                            repository.GetLibraryRevision() != initialRevision;
    return result;
  };
  result.archivePath = record.meta.BmsPath;
  result.rootPath = result.archivePath.parent_path();
  if (result.rootPath.empty()) {
    result.rootPath = ".";
  }
  auto cancel = [&]() {
    result.success = false;
    result.cancelled = true;
    result.message = "Unzip cancelled";
    result.chartPath.clear();
  };
  try {
    if (stopToken.stop_requested()) {
      cancel();
      return finish();
    }
    if (!eligible(record)) {
      result.message = "Selected item is not a solid archive.";
      return finish();
    }
    std::string error;
    const auto extracted = archive_file::unzipArchiveFully(
        result.archivePath, result.rootPath, &error, &stopToken, progress,
        nullptr, reuseCompletedFolder);
    if (extracted) {
      result.outputFolder = extracted->outputFolder;
    }
    if (stopToken.stop_requested()) {
      cancel();
      return finish();
    }
    if (result.outputFolder.empty()) {
      result.message = error.empty() ? "Unzip failed" : "Unzip failed: " + error;
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
        });
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
