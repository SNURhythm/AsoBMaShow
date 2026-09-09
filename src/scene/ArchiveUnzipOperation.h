#pragma once

#include "../ArchiveFile.h"
#include "../ThreadCompat.h"
#include "../repositories/ChartRepository.h"

#include <mutex>
#include <optional>

struct ArchiveUnzipResult {
  bool success = false;
  bool cancelled = false;
  bool libraryChanged = false;
  bool scanCommitted = false;
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
