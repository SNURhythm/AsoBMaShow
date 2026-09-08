#pragma once

#include "../BmsSearchService.h"

#include <cstdint>
#include <functional>

namespace asobmshow::bms_search {

struct ArchiveExtractionLimits {
  std::uint64_t maxEntryBytes = 2ULL * 1024 * 1024 * 1024;
  std::uint64_t maxTotalBytes = 8ULL * 1024 * 1024 * 1024;
  std::uint64_t maxEntries = 100000;
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
