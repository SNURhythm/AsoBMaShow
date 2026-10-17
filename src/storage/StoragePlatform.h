#pragma once

#include "ChartResourceInventory.h"

namespace storage_management {

struct StoragePlatformPaths {
  std::filesystem::path journalRoot;
  std::filesystem::path temporaryRoot;
  // Recovery authority comes from the application, never from a journal file.
  std::vector<std::filesystem::path> allowedRoots;
};
StoragePlatformPaths MakeStoragePlatformPaths(
    const std::filesystem::path &privateApplicationRoot,
    std::vector<std::filesystem::path> allowedRoots);

using StorageCommitPrecondition = std::function<bool(const AssetCandidate &,
    const std::vector<StorageFileChange> &, Checkpoint, std::string &)>;

struct FileTransactionDependencies {
  std::function<std::optional<FileIdentity>(const std::filesystem::path &, std::string &)> identity;
  std::function<bool(const std::filesystem::path &, std::string &)> createExclusive;
  std::function<bool(const std::filesystem::path &, const std::filesystem::path &,
                     const FileIdentity &, Checkpoint, std::string &)> copyAndSync;
  std::function<bool(const std::filesystem::path &, const std::filesystem::path &, std::string &)> renameNoReplace;
  std::function<bool(const std::filesystem::path &, std::string &)> remove;
  std::function<bool(const std::filesystem::path &, std::string &)> syncDirectory;
  std::function<void(std::string_view)> faultPoint;
  StorageCommitPrecondition beforeCommit;
};
FileTransactionDependencies DefaultFileTransactionDependencies();
std::optional<std::string> HashStorageFile(const std::filesystem::path &, Checkpoint, std::string &);

} // namespace storage_management
