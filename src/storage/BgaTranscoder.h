#pragma once

#include "ChartResourceInventory.h"
#include "VideoCompressionGeometry.h"

namespace storage_management {
EncodeResult EncodeBga(const AssetCandidate &, const StoragePolicy &,
                       const std::filesystem::path &temporaryOutput,
                       Checkpoint = {}, ProgressCallback = {});
} // namespace storage_management
