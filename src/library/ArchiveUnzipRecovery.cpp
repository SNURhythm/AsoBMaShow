#include "ArchiveUnzipRecovery.h"
#include "../ArchiveFile.h"

#include <chrono>

namespace archive_unzip_recovery {

namespace {

void logPending(const ArchiveUnzipRecoveryRecord &record, const char *reason,
                const std::error_code &error = {}) {
  archive_file::appendDebugLogLine(
      std::string("Unzip recovery pending: ") + reason +
      " archive=" + fspath_to_utf8(record.archivePath) +
      " output=" + fspath_to_utf8(record.outputFolder) +
      (error ? " error=" + error.message() + " (" +
                   error.category().name() + ":" + std::to_string(error.value()) + ")"
             : ""));
}

bool removeIncompleteOutput(const std::filesystem::path &outputFolder,
                            std::error_code &error) {
  std::filesystem::directory_iterator entry(outputFolder, error), end;
  while (!error && entry != end) {
    if (entry->path().filename() != ".asobmashow_unzip_incomplete") {
      std::filesystem::remove_all(entry->path(), error);
      if (error) return false;
    }
    entry.increment(error);
  }
  if (error) return false;
  std::filesystem::remove_all(outputFolder, error);
  return !error;
}

}

std::timed_mutex &operationMutex() {
  static std::timed_mutex mutex;
  return mutex;
}

std::unique_lock<std::timed_mutex> acquireOperationLock(
    const std::stop_token &stopToken, ChartScanPauseCallback checkpoint) {
  std::unique_lock lock(operationMutex(), std::defer_lock);
  while (!stopToken.stop_requested() && (!checkpoint || checkpoint())) {
    if (lock.try_lock_for(std::chrono::milliseconds(25))) break;
  }
  return lock;
}

bool deleteArchiveRecords(ChartRepository::Session &session,
                          const std::vector<std::filesystem::path> &archives) {
  if (archives.empty()) return true;
  auto batch = session.BeginScanBatch();
  if (!batch) return false;
  for (const auto &archive : archives) {
    if (!batch->DeleteChartsInArchive(archive) ||
        !batch->DeleteSolidArchive(archive) ||
        !batch->DeleteArchiveCache(archive)) return false;
  }
  return session.ClearScanCheckpoint() && batch->Commit();
}

Result recover(ChartRepository::Session &session,
               const std::stop_token &stopToken,
               ChartScanProgressCallback progress,
               ChartScanPauseCallback checkpoint) {
  Result result;
  auto lock = acquireOperationLock(stopToken, checkpoint);
  if (!lock.owns_lock()) return result;
  const auto pending = session.LoadUnzipRecovery();
  if (!pending) {
    archive_file::appendDebugLogLine("Unzip recovery pending: could not load recovery journal");
    return result;
  }
  std::vector<std::filesystem::path> folders, deletedArchives, acknowledged;
  bool accessible = true;
  for (const auto &record : *pending) {
    if (stopToken.stop_requested() || (checkpoint && !checkpoint())) return result;
    std::error_code error;
    if (!std::filesystem::is_directory(record.archivePath.parent_path(), error) || error) {
      logPending(record, "archive parent directory unavailable", error);
      accessible = false;
      continue;
    }
    const auto source = std::filesystem::symlink_status(record.archivePath, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      logPending(record, "could not inspect archive", error);
      accessible = false;
      continue;
    }
    const bool sourceExists = std::filesystem::exists(source);
    error.clear();
    const auto output = std::filesystem::symlink_status(record.outputFolder, error);
    if ((error && error != std::errc::no_such_file_or_directory) ||
        std::filesystem::is_symlink(output)) {
      logPending(record, "output is inaccessible or a symbolic link", error);
      accessible = false;
      continue;
    }
    if (!std::filesystem::exists(output)) {
      archive_file::appendDebugLogLine(
          "Unzip recovery: output no longer exists; acknowledging " +
          fspath_to_utf8(record.outputFolder));
      acknowledged.push_back(record.outputFolder);
      continue;
    }
    error.clear();
    const auto markerPath = record.outputFolder / ".asobmashow_unzip_complete";
    const auto markerStatus = std::filesystem::symlink_status(markerPath, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      logPending(record, "could not inspect completion marker", error);
      accessible = false;
      continue;
    }
    std::string completionFailure = "completion marker is missing or not a regular file";
    const bool complete = std::filesystem::is_regular_file(markerStatus) &&
        archive_file::unzipFolderHasMatchingCompleteMarker(
            record.outputFolder, record.archivePath, record.archiveKey, &completionFailure);
    if (!complete) {
      error.clear();
      if (sourceExists && archive_file::unzipFolderHasMatchingIncompleteMarker(
              record.outputFolder, record.archivePath, record.archiveKey, &error)) {
        if (removeIncompleteOutput(record.outputFolder, error)) {
          acknowledged.push_back(record.outputFolder);
        } else {
          logPending(record, "could not remove owned incomplete output", error);
          accessible = false;
        }
      } else {
        const auto reason = (sourceExists ? "output is unverified: " :
            "source archive is missing and output is unverified: ") + completionFailure;
        logPending(record, reason.c_str(), error);
        accessible = false;
      }
      continue;
    }
    error.clear();
    std::filesystem::remove(record.outputFolder / ".asobmashow_unzip_incomplete", error);
    if (error) {
      logPending(record, "could not remove incomplete marker", error);
      accessible = false;
      continue;
    }
    folders.push_back(record.outputFolder);
    acknowledged.push_back(record.outputFolder);
    if (record.deleteOriginal && !sourceExists) deletedArchives.push_back(record.archivePath);
  }
  if (stopToken.stop_requested() || (checkpoint && !checkpoint())) return result;
  const bool cleaned = deleteArchiveRecords(session, deletedArchives);
  if (!cleaned) {
    archive_file::appendDebugLogLine("Unzip recovery pending: could not reconcile deleted archive records");
  }
  result.libraryChanged = cleaned && !deletedArchives.empty();
  bool indexed = true;
  if (!folders.empty()) {
    ChartLibraryScanner scanner;
    const auto scan = scanner.ScanAddedWithResult(
        session, folders, &stopToken, progress, checkpoint, nullptr, nullptr, true);
    indexed = scan.completed && scan.committed;
    if (!indexed) {
      archive_file::appendDebugLogLine("Unzip recovery pending: extracted folder scan did not commit");
    }
    result.libraryChanged = result.libraryChanged || scan.changedCount > 0;
  }
  if (!cleaned || !indexed || stopToken.stop_requested() ||
      (checkpoint && !checkpoint())) return result;
  const bool cleared = session.ClearUnzipRecovery(acknowledged);
  if (!cleared) {
    archive_file::appendDebugLogLine("Unzip recovery pending: could not acknowledge recovery journal records");
  }
  result.completed = cleared && accessible;
  return result;
}

}
