#pragma once

#include "../ArchiveFile.h"
#include "../ThreadCompat.h"
#include "../repositories/ChartRepository.h"

#include <cstddef>
#include <mutex>
#include <optional>

struct ArchiveUnzipResult {
  bool success = false;
  bool cancelled = false;
  bool libraryChanged = false;
  bool scanCommitted = false;
  bool batch = false;
  std::size_t archiveCount = 0;
  std::size_t completedCount = 0;
  std::size_t succeededCount = 0;
  std::size_t failedCount = 0;
  std::size_t deletedCount = 0;
  std::size_t deletionFailedCount = 0;
  std::filesystem::path rootPath, archivePath, outputFolder, chartPath;
  std::string message;
};

class ArchiveUnzipOperation final {
public:
  explicit ArchiveUnzipOperation(ChartRepository &repository);
  ~ArchiveUnzipOperation();
  ArchiveUnzipOperation(const ArchiveUnzipOperation &) = delete;
  ArchiveUnzipOperation &operator=(const ArchiveUnzipOperation &) = delete;

  bool start(const ChartMetaRecord &record);
  bool startAll(bool deleteAfterUnzip);
  bool inProgress() const;
  void requestCancel();
  void cancelAndWait();
  std::optional<archive_file::UnzipProgress> takeProgress();
  std::optional<ArchiveUnzipResult> takeResult();
  bool takeLibraryChanged();
  bool canDeleteArchive() const;
  bool deleteArchive(std::string &message);
  void keepArchive();

  static ArchiveUnzipResult
  Run(const ChartMetaRecord &record, ChartRepository &repository,
      const std::stop_token &stopToken,
      archive_file::UnzipProgressCallback progress = nullptr,
      bool reuseCompletedFolder = true);
  static ArchiveUnzipResult
  RunAll(ChartRepository &repository, bool deleteAfterUnzip,
         const std::stop_token &stopToken,
         archive_file::UnzipProgressCallback progress = nullptr);

private:
  ChartRepository &repository_;
  std::mutex mutex_;
  std::optional<archive_file::UnzipProgress> pendingProgress_;
  std::optional<ArchiveUnzipResult> pendingResult_;
  std::optional<ArchiveUnzipResult> result_;
  bool libraryChangedPending_ = false;
  bool inProgress_ = false;
  std::jthread worker_;
};
