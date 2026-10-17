#pragma once

#include "StoragePlatform.h"

namespace storage_management {

class StorageFileTransaction {
public:
  StorageFileTransaction(StoragePlatformPaths, FileTransactionDependencies = {});
  FileResult Replace(const AssetCandidate &, const EncodeResult &, Checkpoint = {});
  FileResult Delete(const AssetCandidate &, Checkpoint = {});
  // Call at startup before workers, or while holding the shared conversion permit.
  // After journal recovery, removes recognized private jobs abandoned by a crash.
  StorageReport Recover(Checkpoint = {});

private:
  bool PrepareJournalDirectory(std::string &);
  FileResult Run(const AssetCandidate &, const EncodeResult *, Checkpoint);
  StoragePlatformPaths paths_;
  FileTransactionDependencies dependencies_;
};

} // namespace storage_management
