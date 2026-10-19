#pragma once

#include "ChartResourceInventory.h"
#include "VideoCompressionGeometry.h"

namespace storage_management {
StorageDiagnostic ValidateExistingBga(const std::filesystem::path &, Checkpoint = {});
EncodeResult EncodeBga(const AssetCandidate &, const StoragePolicy &,
                       const std::filesystem::path &temporaryOutput,
                       Checkpoint = {}, ProgressCallback = {});
} // namespace storage_management
