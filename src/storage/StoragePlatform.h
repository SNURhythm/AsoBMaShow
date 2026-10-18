#pragma once

#include "ChartResourceInventory.h"

namespace storage_management {

struct StoragePlatformPaths {
  std::filesystem::path journalRoot;
  std::filesystem::path temporaryRoot;
  // Recovery authority comes from the application, never from a journal file.
  std::vector<std::filesystem::path> allowedRoots;
  std::vector<std::filesystem::path> pathAccessRoots;
  // Expendable scan indexes; never read as transaction recovery authority.
  std::filesystem::path scanRoot;
  // Existing ancestor captured before journal creation; copied to every worker
  // so retries/concurrent first use cannot mistake an unsynced new directory for it.
  std::filesystem::path journalSyncRoot;
  // Optional application-owned relocation mapping. Recovery still validates
  // canonical paths, current authority, and recorded file identities afterward.
  std::function<std::filesystem::path(const std::filesystem::path &)> resolveRecoveryPath;
};
// On sandboxed platforms, roots must come from the OS private directory and
// authorized library/bookmark registrations, never from recovery records.
StoragePlatformPaths MakeStoragePlatformPaths(
    const std::filesystem::path &privateApplicationRoot,
    std::vector<std::filesystem::path> allowedRoots, bool sandboxedAncestors = false);

// Include global recovery scopes only while journals are pending. The caller
// collapses overlaps before reserving; inspection failures reject admission.
std::optional<std::vector<std::filesystem::path>> StorageReservationRoots(
    const StoragePlatformPaths &, std::vector<std::filesystem::path> selected, std::string &error);

// Startup only, before any scan worker exists. Unknown contents and links are
// retained; transaction journals are never part of this disposable cache sweep.
bool CleanupAbandonedStorageScans(const StoragePlatformPaths &, std::string &error, Checkpoint = {});

using StorageCommitPrecondition = std::function<bool(const AssetCandidate &,
    const std::vector<StorageFileChange> &, Checkpoint, std::string &)>;

struct StorageFileCreation {
  bool created = false;
  bool ready = false;
  // Captured from the creating handle, including when its flush fails.
  FileIdentity identity;
};

struct FileTransactionDependencies {
  std::function<std::optional<FileIdentity>(const std::filesystem::path &, std::string &)> identity;
  std::function<StorageFileCreation(const std::filesystem::path &, std::string &)> createExclusive;
  std::function<bool(const std::filesystem::path &, const std::filesystem::path &,
                     const FileIdentity &, Checkpoint, std::string &)> copyAndSync;
  std::function<bool(const std::filesystem::path &, const FileIdentity &,
                     const std::filesystem::path &, const FileIdentity &, std::string &)> preservePermissions;
  std::function<bool(const std::filesystem::path &, const std::filesystem::path &, std::string &)> renameNoReplace;
  std::function<bool(const std::filesystem::path &, std::string &)> remove;
  std::function<bool(const std::filesystem::path &, std::string &)> syncDirectory;
  std::function<void(std::string_view)> faultPoint;
  StorageCommitPrecondition beforeCommit;
};
FileTransactionDependencies DefaultFileTransactionDependencies();
std::optional<std::string> HashStorageFile(const std::filesystem::path &, Checkpoint, std::string &);

} // namespace storage_management
