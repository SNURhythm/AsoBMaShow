#include "TemporaryCache.h"
#include "../path.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <unordered_set>

namespace archive_file {
namespace {
bool stopRequested(const std::stop_token *token) {
  return token != nullptr && token->stop_requested();
}

std::uint64_t clampFileSizeForResult(std::uintmax_t value) {
  return static_cast<std::uint64_t>(
      std::min(value, static_cast<std::uintmax_t>(
                          std::numeric_limits<std::uint64_t>::max())));
}

void addClamped(std::uint64_t &total, std::uint64_t value) {
  const std::uint64_t maxValue = std::numeric_limits<std::uint64_t>::max();
  total = value > maxValue - total ? maxValue : total + value;
}

bool directoryStats(const std::filesystem::path &root, std::uint64_t &bytes,
                    std::uint64_t &entries,
                    const std::stop_token *stopToken = nullptr) {
  bytes = 0;
  entries = 0;
  std::error_code error;
  const bool rootIsFile = std::filesystem::is_regular_file(root, error);
  if (error) {
    return false;
  }
  if (rootIsFile) {
    const std::uintmax_t size = std::filesystem::file_size(root, error);
    if (error) {
      return false;
    }
    bytes = clampFileSizeForResult(size);
    entries = 1;
    return true;
  }
  const bool rootIsDirectory = std::filesystem::is_directory(root, error);
  if (error || !rootIsDirectory) {
    return false;
  }

  std::filesystem::recursive_directory_iterator it(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  while (!error && it != end) {
    if (stopRequested(stopToken)) {
      return false;
    }
    const std::filesystem::directory_entry &entry = *it;
    std::error_code entryError;
    const auto status = entry.symlink_status(entryError);
    if (std::filesystem::is_regular_file(status) && !entryError) {
      const std::uintmax_t size = entry.file_size(entryError);
      if (!entryError) {
        addClamped(bytes, clampFileSizeForResult(size));
      }
    }
    addClamped(entries, 1);
    it.increment(error);
  }
  return !error && !stopRequested(stopToken);
}

std::uint64_t directoryByteSize(const std::filesystem::path &root,
                                const std::stop_token *stopToken = nullptr) {
  std::error_code error;
  // Cleanup removes the link itself, so its target contributes no bytes.
  if (std::filesystem::is_symlink(root, error) || error) {
    return 0;
  }
  std::uint64_t bytes = 0;
  std::uint64_t entries = 0;
  directoryStats(root, bytes, entries, stopToken);
  return bytes;
}

} // namespace

std::optional<std::filesystem::path> TemporaryCache::materialize(
    const std::filesystem::path &cacheRoot, const OutputPath &outputPath,
    const std::vector<unsigned char> &bytes, std::string *errorMessage,
    const std::atomic_bool *cancelled) {
  if (cancelled != nullptr && cancelled->load(std::memory_order_relaxed)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Materialize cancelled.";
    }
    return std::nullopt;
  }

  std::lock_guard<std::mutex> lock(mutationMutex_);
  std::error_code error;
  std::filesystem::create_directories(cacheRoot, error);
  if (error) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not create archive cache: " + error.message();
    }
    return std::nullopt;
  }

  std::filesystem::path output = outputPath();
  bool needsWrite = true;
  const bool outputExists = std::filesystem::exists(output, error);
  if (error) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not check cached archive entry: " +
                      error.message();
    }
    return std::nullopt;
  }
  if (outputExists) {
    const std::uintmax_t size = std::filesystem::file_size(output, error);
    if (error) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not read cached archive entry size: " +
                        error.message();
      }
      return std::nullopt;
    }
    needsWrite = size != bytes.size();
  }
  if (needsWrite) {
    if (cancelled != nullptr && cancelled->load(std::memory_order_relaxed)) {
      if (errorMessage != nullptr) {
        *errorMessage = "Materialize cancelled.";
      }
      return std::nullopt;
    }

    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    if (!file) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not create cached archive entry: " +
                        fspath_to_utf8(output);
      }
      return std::nullopt;
    }
    if (!bytes.empty()) {
      constexpr std::size_t kMaterializeWriteChunkBytes = 1024 * 1024;
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        if (cancelled != nullptr &&
            cancelled->load(std::memory_order_relaxed)) {
          if (errorMessage != nullptr) {
            *errorMessage = "Materialize cancelled.";
          }
          file.close();
          std::filesystem::remove(output, error);
          return std::nullopt;
        }
        const std::size_t chunkBytes =
            std::min(kMaterializeWriteChunkBytes, bytes.size() - offset);
        file.write(reinterpret_cast<const char *>(bytes.data() + offset),
                   static_cast<std::streamsize>(chunkBytes));
        if (!file) {
          break;
        }
        offset += chunkBytes;
      }
    }
    if (!file) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not write cached archive entry: " +
                        fspath_to_utf8(output);
      }
      return std::nullopt;
    }
  }
  return output;
}

bool TemporaryCache::cleanup(
    const std::filesystem::path &root, TemporaryCacheCleanupResult &result,
    const std::vector<std::filesystem::path> &protectedPaths,
    const PathKey &pathKey, std::string *errorMessage) {
  result = {};
  result.path = root;

  std::lock_guard<std::mutex> lock(mutationMutex_);
  std::error_code error;
  const bool exists = std::filesystem::exists(result.path, error);
  if (error) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not check archive cache: " + error.message();
    }
    return false;
  }
  if (!exists) {
    return true;
  }

  result.cacheExisted = true;
  std::unordered_set<std::string> protectedKeys;
  protectedKeys.reserve(protectedPaths.size());
  for (const auto &protectedPath : protectedPaths) {
    if (!protectedPath.empty()) {
      protectedKeys.insert(pathKey(protectedPath));
    }
  }

  std::vector<std::filesystem::path> cacheEntries;
  std::filesystem::directory_iterator it(
      result.path, std::filesystem::directory_options::skip_permission_denied,
      error);
  if (error) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not read archive cache: " + error.message();
    }
    return false;
  }

  const std::filesystem::directory_iterator end;
  while (it != end) {
    cacheEntries.push_back(it->path());
    it.increment(error);
    if (error) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not scan archive cache: " + error.message();
      }
      return false;
    }
  }

  for (const auto &entryPath : cacheEntries) {
    if (protectedKeys.contains(pathKey(entryPath))) {
      ++result.skippedEntries;
      continue;
    }

    const std::uint64_t entryBytes = directoryByteSize(entryPath);
    const std::uintmax_t removedEntries =
        std::filesystem::remove_all(entryPath, error);
    if (error) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not remove archive cache entry: " +
                        error.message();
      }
      return false;
    }
    result.removedBytes += entryBytes;
    result.removedEntries += clampFileSizeForResult(removedEntries);
  }

  error.clear();
  std::filesystem::remove(result.path, error);
  if (error && error != std::errc::directory_not_empty) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not remove empty archive cache folder: " +
                      error.message();
    }
    return false;
  }
  return true;
}

bool TemporaryCache::measure(
    const std::filesystem::path &root, TemporaryCacheUsageResult &result,
    std::string *errorMessage, const std::stop_token *stopToken) const {
  result = {};
  result.path = root;

  std::error_code error;
  const bool exists = std::filesystem::exists(result.path, error);
  if (error) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not check archive cache: " + error.message();
    }
    return false;
  }
  if (!exists) {
    return true;
  }

  result.cacheExisted = true;
  if (!directoryStats(result.path, result.bytes, result.entries, stopToken)) {
    if (stopRequested(stopToken)) {
      if (errorMessage != nullptr) {
        *errorMessage = "Archive cache measurement cancelled.";
      }
      return false;
    }
    if (errorMessage != nullptr) {
      *errorMessage = "Could not measure archive cache.";
    }
    return false;
  }
  return true;
}

} // namespace archive_file
