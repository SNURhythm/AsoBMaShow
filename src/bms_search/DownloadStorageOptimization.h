#pragma once

#include "../storage/StorageTypes.h"

namespace asobmshow::bms_search {
// root is the extracted subtree of one private, disposable download attempt.
// Journals and encoder output stay in its parent, outside the promoted package.
storage_management::StorageReport optimizeExtractedDownload(
    const std::filesystem::path &root, const storage_management::StoragePolicy &policy,
    storage_management::Checkpoint checkpoint, storage_management::ProgressCallback progress);
}
