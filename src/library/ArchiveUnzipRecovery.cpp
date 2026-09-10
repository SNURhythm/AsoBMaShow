#include "ArchiveUnzipRecovery.h"
#include "../repositories/ChartStorageIdentity.h"

#include <chrono>
#include <fstream>

namespace archive_unzip_recovery {

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
  if (!pending) return result;
  std::vector<std::filesystem::path> folders, deletedArchives, acknowledged;
  bool accessible = true;
  for (const auto &record : *pending) {
    if (stopToken.stop_requested() || (checkpoint && !checkpoint())) return result;
    std::error_code error;
    if (!std::filesystem::is_directory(record.archivePath.parent_path(), error) || error) {
      accessible = false;
      continue;
    }
    const auto source = std::filesystem::symlink_status(record.archivePath, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      accessible = false;
      continue;
    }
    const bool sourceExists = std::filesystem::exists(source);
    error.clear();
    const auto output = std::filesystem::symlink_status(record.outputFolder, error);
    if ((error && error != std::errc::no_such_file_or_directory) ||
        std::filesystem::is_symlink(output)) {
      accessible = false;
      continue;
    }
    error.clear();
    const auto markerPath = record.outputFolder / ".asobmashow_unzip_complete";
    const auto markerStatus = std::filesystem::symlink_status(markerPath, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      accessible = false;
      continue;
    }
    if (!std::filesystem::exists(markerStatus) && sourceExists) {
      acknowledged.push_back(record.outputFolder);
      continue;
    }
    if (!std::filesystem::is_directory(output) ||
        !std::filesystem::is_regular_file(markerStatus)) {
      accessible = false;
      continue;
    }
    std::ifstream marker(markerPath, std::ios::binary);
    std::string key, archivePath;
    const bool readable = bool(std::getline(marker, key)) && bool(std::getline(marker, archivePath));
    std::filesystem::path markerArchivePath = utf8_to_path_t(archivePath);
    chart_storage_identity::ToAbsolutePath(markerArchivePath);
    if (!readable || key != record.archiveKey ||
        markerArchivePath.lexically_normal() != record.archivePath.lexically_normal()) {
      error.clear();
      if (sourceExists && std::filesystem::is_regular_file(
              record.outputFolder / ".asobmashow_unzip_incomplete", error) && !error) {
        acknowledged.push_back(record.outputFolder);
        continue;
      }
      accessible = false;
      continue;
    }
    error.clear();
    std::filesystem::remove(record.outputFolder / ".asobmashow_unzip_incomplete", error);
    if (error) {
      accessible = false;
      continue;
    }
    folders.push_back(record.outputFolder);
    acknowledged.push_back(record.outputFolder);
    if (record.deleteOriginal && !sourceExists) deletedArchives.push_back(record.archivePath);
  }
  if (stopToken.stop_requested() || (checkpoint && !checkpoint())) return result;
  const bool cleaned = deleteArchiveRecords(session, deletedArchives);
  result.libraryChanged = cleaned && !deletedArchives.empty();
  bool indexed = true;
  if (!folders.empty()) {
    ChartLibraryScanner scanner;
    const auto scan = scanner.ScanAddedWithResult(
        session, folders, &stopToken, progress, checkpoint, nullptr, nullptr, true);
    indexed = scan.completed && scan.committed;
    result.libraryChanged = result.libraryChanged || scan.changedCount > 0;
  }
  if (!cleaned || !indexed || stopToken.stop_requested() ||
      (checkpoint && !checkpoint())) return result;
  result.completed = session.ClearUnzipRecovery(acknowledged) && accessible;
  return result;
}

}
