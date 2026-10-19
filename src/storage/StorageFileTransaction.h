#pragma once

#include "StoragePlatform.h"

namespace storage_management {

struct StorageRecoveryObserver {
  // Observe one validated record at a time; failed recovery never advances
  // reviewed evidence, even when its partial mutations invalidate estimates.
  std::function<void(const std::filesystem::path &)> before;
  std::function<void(const std::filesystem::path &, const std::vector<StorageFileChange> &)> after;
};

class StorageFileTransaction {
public:
  StorageFileTransaction(StoragePlatformPaths, FileTransactionDependencies = {});
  FileResult Replace(const AssetCandidate &, const EncodeResult &, Checkpoint = {});
  // Caller supplies decoder-validated retained output identity and content hash.
  FileResult ReuseExisting(const AssetCandidate &, Checkpoint = {});
  FileResult Delete(const AssetCandidate &, Checkpoint = {});
  // Call at startup before workers, or while holding the shared conversion permit.
  // After journal recovery, removes recognized private jobs abandoned by a crash.
  StorageReport Recover(Checkpoint = {}, const StorageRecoveryObserver & = {});

private:
  bool PrepareJournalDirectory(std::string &);
  FileResult Run(const AssetCandidate &, const EncodeResult *, Checkpoint, bool reuse = false);
  StoragePlatformPaths paths_;
  FileTransactionDependencies dependencies_;
};

} // namespace storage_management
