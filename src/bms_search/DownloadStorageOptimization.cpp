#include "DownloadStorageOptimization.h"
#include "../storage/StorageOptimizer.h"

namespace asobmshow::bms_search {
storage_management::StorageReport optimizeExtractedDownload(
    const std::filesystem::path &root, const storage_management::StoragePolicy &policy,
    storage_management::Checkpoint checkpoint, storage_management::ProgressCallback progress) {
  using namespace storage_management;
  OptimizationRequest request;
  request.policy = policy;
  request.inventory = {.groups = {{"download", root}}, .policy = policy, .isolatedStaging = true};
  auto paths = MakeStoragePlatformPaths(root.parent_path(), {root});
  return StorageOptimizer(DefaultOptimizerDependencies(std::move(paths))).Run(
      request, std::move(checkpoint), std::move(progress));
}
}
