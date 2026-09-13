#pragma once

#include <cstdint>
#include <filesystem>

namespace archive_file {

struct TemporaryCacheCleanupResult {
  std::filesystem::path path;
  bool cacheExisted = false;
  std::uint64_t removedEntries = 0;
  std::uint64_t removedBytes = 0;
  std::uint64_t skippedEntries = 0;
};

struct TemporaryCacheUsageResult {
  std::filesystem::path path;
  bool cacheExisted = false;
  std::uint64_t entries = 0;
  std::uint64_t bytes = 0;
};

} // namespace archive_file
