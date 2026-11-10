#pragma once

#include "../BmsSearchService.h"

#include <cstdint>
#include <functional>
#include <limits>

namespace asobmshow::bms_search {

// Selected downloads stream to disk; available space, rather than an authored
// resource-size quota, bounds extraction. Explicit caller limits still apply.
struct ArchiveExtractionLimits {
  std::uint64_t maxEntryBytes = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t maxTotalBytes = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t maxEntries = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t reservedFreeBytes = 256ULL * 1024 * 1024;
};

using ArchiveExtractionCancelled = std::function<bool()>;

bool extractZipArchive(
    const std::filesystem::path &archivePath,
    const std::filesystem::path &outputPath, std::string &errorMessage,
    BmsSearchDownloadProgressCallback progressCallback,
    ArchiveExtractionCancelled cancelled = {},
    ArchiveExtractionLimits limits = {});

bool extractDownloadedArchive(
    const std::filesystem::path &archivePath,
    const std::filesystem::path &outputPath, std::string &errorMessage,
    BmsSearchDownloadProgressCallback progressCallback,
    ArchiveExtractionCancelled cancelled = {},
    ArchiveExtractionLimits limits = {});

}
