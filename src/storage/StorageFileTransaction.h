#pragma once

#include "StoragePlatform.h"

namespace storage_management {

class StorageFileTransaction {
public:
  StorageFileTransaction(StoragePlatformPaths, FileTransactionDependencies = {});
  FileResult Replace(const AssetCandidate &, const EncodeResult &, Checkpoint = {});
  FileResult Delete(const AssetCandidate &, Checkpoint = {});
  StorageReport Recover(Checkpoint = {});

private:
  FileResult Run(const AssetCandidate &, const EncodeResult *, Checkpoint);
  StoragePlatformPaths paths_;
  FileTransactionDependencies dependencies_;
};

} // namespace storage_management
