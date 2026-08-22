#pragma once

#include "ThreadCompat.h"
#include "bms_parser.hpp"
#include "path.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace archive_file {

inline constexpr std::array<std::string_view, 22> kArchiveExtensions = {
    ".tar.bz2", ".tar.gz", ".tar.xz", ".tar.zst", ".tbz2", ".tgz",
    ".txz",     ".tzst",   ".zip",    ".zipx",    ".cbz",  ".7z",
    ".cb7",     ".rar",    ".cbr",    ".lzh",     ".lha",  ".tar",
    ".bz2",     ".gz",     ".xz",     ".zst",
};

namespace detail {

inline std::string asciiLowerCopy(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (unsigned char ch : value) {
    result.push_back(static_cast<char>(std::tolower(ch)));
  }
  return result;
}

inline bool endsWithArchiveExtension(std::string_view value,
                                     std::string_view extension) {
  return value.size() >= extension.size() &&
         value.compare(value.size() - extension.size(), extension.size(),
                       extension) == 0;
}

} // namespace detail

inline std::string archiveExtensionFromName(std::string_view name) {
  const std::string lowerName = detail::asciiLowerCopy(name);
  for (std::string_view extension : kArchiveExtensions) {
    if (detail::endsWithArchiveExtension(lowerName, extension)) {
      return std::string(extension);
    }
  }
  return "";
}

inline std::string archiveExtensionFromPath(const std::filesystem::path &path) {
  return archiveExtensionFromName(fspath_to_utf8(path.filename()));
}

inline bool isRecognizedArchiveExtension(std::string_view extension) {
  if (extension.empty()) {
    return false;
  }
  const std::string lowerExtension = detail::asciiLowerCopy(extension);
  for (std::string_view archiveExtension : kArchiveExtensions) {
    if (lowerExtension == archiveExtension) {
      return true;
    }
  }
  return false;
}

struct Entry {
  std::filesystem::path path;
  bool directory = false;
  std::uint64_t size = 0;
  std::size_t order = 0;
  std::int64_t offset = -1;
  bool solid = false;
};

struct EntryRange {
  std::size_t start = 0;
  std::size_t end = 0;
};

struct FileData {
  std::filesystem::path path;
  std::vector<unsigned char> bytes;
};

struct SourcePreference {
  int priority = 0;
  std::uint64_t archiveSize = 0;
};

struct UnzipProgress {
  double fraction = 0.0;
  std::uint64_t current = 0;
  std::uint64_t total = 0;
  std::string message;
};

using UnzipProgressCallback = std::function<void(const UnzipProgress &)>;
using PauseCallback = std::function<bool()>;
using CachePathNormalizer = std::function<void(std::filesystem::path &)>;
using FileDataCallback = std::function<bool(FileData &&)>;
#if defined(ASOBMASHOW_ARCHIVE_FILE_STREAMING_TEST_HOOKS)
using StreamingEntryObserverForTesting =
    std::function<void(const std::filesystem::path &,
                       const std::filesystem::path &)>;
#endif

struct UnzipArchiveResult {
  std::filesystem::path outputFolder;
  std::uint64_t fileCount = 0;
  std::uint64_t uncompressedSize = 0;
};

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

bool isArchiveSupportAvailable();
bool hasSupportedArchiveExtension(const std::filesystem::path &path);
void setCachePathNormalizer(CachePathNormalizer normalizer);
void appendDebugLogLine(const std::string &message);
#if defined(ASOBMASHOW_ARCHIVE_FILE_STREAMING_TEST_HOOKS)
void setStreamingEntryObserverForTesting(
    StreamingEntryObserverForTesting observer);
#endif
std::uint64_t debugLogRevision();
std::vector<std::string> debugLogLines();
std::string debugLogText();
bool isVirtualPath(const std::filesystem::path &path);
bool splitVirtualPath(const std::filesystem::path &path,
                      std::filesystem::path &archivePath,
                      std::filesystem::path &innerPath);
std::filesystem::path makeVirtualPath(const std::filesystem::path &archivePath,
                                      const std::filesystem::path &innerPath);

bool listEntries(const std::filesystem::path &archivePath,
                 std::vector<Entry> &entries,
                 std::string *errorMessage = nullptr,
                 PauseCallback pauseCallback = nullptr);
bool readArchiveEntries(const std::filesystem::path &archivePath,
                        const std::vector<std::filesystem::path> &innerPaths,
                        std::vector<FileData> &files,
                        std::string *errorMessage = nullptr,
                        PauseCallback pauseCallback = nullptr);
bool readArchiveEntriesStreaming(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    FileDataCallback onFile,
    std::string *errorMessage = nullptr,
    PauseCallback pauseCallback = nullptr);
// Calls onFile from extractor worker threads. The callback must be thread-safe.
bool readArchiveEntriesConcurrently(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    FileDataCallback onFile,
    std::size_t maxWorkers,
    std::uint64_t maxInFlightBytes,
    std::string *errorMessage = nullptr,
    PauseCallback pauseCallback = nullptr);
bool readArchiveEntriesInRange(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const EntryRange &range,
    std::vector<FileData> &files,
    std::string *errorMessage = nullptr,
    PauseCallback pauseCallback = nullptr);
std::optional<EntryRange>
entryRangeForFolder(const std::filesystem::path &folderPath);
bool exists(const std::filesystem::path &path);
bool readFile(const std::filesystem::path &path,
              std::vector<unsigned char> &bytes,
              std::string *errorMessage = nullptr);
// Bounded variant for callers that receive untrusted media. Ordinary files
// and platform paths stream under the cap; archive entries are also checked
// against indexed uncompressed size before bounded extraction.
bool readFileBounded(const std::filesystem::path &path,
                     std::vector<unsigned char> &bytes,
                     std::size_t maximumBytes,
                     std::string *errorMessage = nullptr,
                     std::stop_token stop = {});
bool isInSolidArchiveFolder(const std::filesystem::path &path);
SourcePreference sourcePreferenceForPath(const std::filesystem::path &path);
std::string cacheKeyForPath(const std::filesystem::path &path);
std::optional<std::filesystem::path>
findFileWithExtensions(const std::filesystem::path &basePath,
                       const std::vector<std::string_view> &extensions);
std::optional<std::filesystem::path>
unzipVirtualFolderForChart(const std::filesystem::path &chartPath,
                           const std::filesystem::path &destinationRoot,
                           std::string *errorMessage = nullptr,
                           const std::stop_token *stopToken = nullptr,
                           UnzipProgressCallback progressCallback = nullptr);
std::optional<UnzipArchiveResult>
unzipArchiveFully(const std::filesystem::path &archivePath,
                  const std::filesystem::path &destinationRoot,
                  std::string *errorMessage = nullptr,
                  const std::stop_token *stopToken = nullptr,
                  UnzipProgressCallback progressCallback = nullptr);
std::optional<std::filesystem::path>
materializeFile(const std::filesystem::path &path,
                std::string *errorMessage = nullptr,
                const std::atomic_bool *cancelled = nullptr);
std::optional<std::filesystem::path>
materializeFileBytes(const std::filesystem::path &path,
                     const std::vector<unsigned char> &bytes,
                     std::string *errorMessage = nullptr,
                     const std::atomic_bool *cancelled = nullptr);
std::filesystem::path
materializedFileCachePath(const std::filesystem::path &path);
bool cleanupTemporaryCache(TemporaryCacheCleanupResult &result,
                           const std::vector<std::filesystem::path>
                               &protectedPaths = {},
                           std::string *errorMessage = nullptr);
bool measureTemporaryCache(TemporaryCacheUsageResult &result,
                           std::string *errorMessage = nullptr,
                           const std::stop_token *stopToken = nullptr);

void parseChart(bms_parser::Parser &parser, const std::filesystem::path &path,
                bms_parser::Chart **chart, bool addReadyMeasure,
                bool metaOnly, std::atomic_bool &cancelled);

} // namespace archive_file
