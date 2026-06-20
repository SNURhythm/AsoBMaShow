#pragma once

#include "bms_parser.hpp"
#include "path.h"

#include <atomic>
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

struct UnzipArchiveResult {
  std::filesystem::path outputFolder;
  std::uint64_t fileCount = 0;
  std::uint64_t uncompressedSize = 0;
};

bool isArchiveSupportAvailable();
bool hasSupportedArchiveExtension(const std::filesystem::path &path);
void setCachePathNormalizer(CachePathNormalizer normalizer);
void appendDebugLogLine(const std::string &message);
std::uint64_t debugLogRevision();
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
                std::string *errorMessage = nullptr);
std::optional<std::filesystem::path>
materializeFileBytes(const std::filesystem::path &path,
                     const std::vector<unsigned char> &bytes,
                     std::string *errorMessage = nullptr);

void parseChart(bms_parser::Parser &parser, const std::filesystem::path &path,
                bms_parser::Chart **chart, bool addReadyMeasure,
                bool metaOnly, std::atomic_bool &cancelled);

} // namespace archive_file
