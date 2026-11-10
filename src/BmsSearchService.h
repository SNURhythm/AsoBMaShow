#pragma once

#include "i18n/Localization.h"
#include "storage/StorageTypes.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <string_view>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace downloads { struct Request; }

struct BmsSearchDownloadProgress {
  std::string message;
  std::uint64_t downloadedBytes = 0;
  std::uint64_t totalBytes = 0;
  std::optional<storage_management::StorageProgress> storageProgress;
};

using BmsSearchDownloadProgressCallback =
    std::function<void(const BmsSearchDownloadProgress &)>;

// Runs on the download worker. Keep the current transfer alive while waiting
// for the user's decision; true retries/resumes it, false cancels it.
using BmsSearchDownloadRetryCallback =
    std::function<bool(const std::string &message, bool canResume)>;

struct BmsSearchDownloadOptions {
  bool skipUnarchivingForNonSolidArchives = false;
  BmsSearchDownloadRetryCallback requestRetry;
  storage_management::StoragePolicy storagePolicy;
  // Application-owned probe; never captures a view or mutable settings.
  std::function<bool()> storagePauseRequested;
  // Mobile admission transfers ownership after provider resolution, before any
  // scene-owned payload work. Empty retains the synchronous desktop workflow.
  std::function<std::string(const downloads::Request &)> enqueueDownload;
};

enum class BmsSearchPendingArtifactKind { Archive, ExtractedDirectory };

struct BmsSearchPendingArtifact {
  BmsSearchPendingArtifactKind kind = BmsSearchPendingArtifactKind::Archive;
  std::filesystem::path stagingRoot;
  std::filesystem::path sourcePath;
  std::filesystem::path downloadRoot;
  std::filesystem::path destinationPath;
  std::string archiveName;
  std::string storageKey;
  std::filesystem::path alternateDestinationPath;
  // Private disk-backed inventory of files replaced by an optimized download.
  std::filesystem::path originalFilesIndexPath;
};

enum class BmsSearchPendingArtifactDecision { Keep, Delete };

struct BmsSearchResult {
  enum class Status {
    NotFound,
    NoDownloadLink,
    UnsupportedLink,
    Downloaded,
    HashMismatch,
    DownloadFailed,
  };

  Status status = Status::NotFound;
  std::string message;
  std::string patternUrl;
  std::string bmsUrl;
  std::string downloadUrl;
  std::string fallbackUrl;
  std::filesystem::path outputPath;
  std::filesystem::path debugPath;
  std::vector<std::filesystem::path> removedPaths;
  std::optional<BmsSearchPendingArtifact> pendingArtifact;
  // App-owned presentation retains its identity; transport diagnostics stay raw.
  i18n::Text presentationMessage;
  long downloadHttpStatus = 0;
  std::optional<storage_management::StorageReport> storageReport;
  std::string queuedDownloadId;
};

class BmsSearchService {
public:
  static constexpr const char *kSkipUnarchivingSettingLabel =
      "Skip unarchiving for non-solid archives";

  BmsSearchResult findAndDownload(
      const std::string &sha256, const std::string &md5,
      const std::filesystem::path &libraryRoot, std::atomic_bool &cancelled,
      BmsSearchDownloadProgressCallback progressCallback = nullptr,
      const std::string &title = "", const std::string &artist = "",
      BmsSearchDownloadOptions options = {}) const;

  BmsSearchResult resolvePendingArtifact(
      BmsSearchResult result,
      BmsSearchPendingArtifactDecision decision) const;

  static bool canDownload(std::string_view md5) {
    while (!md5.empty() && std::isspace(static_cast<unsigned char>(md5.front()))) md5.remove_prefix(1);
    while (!md5.empty() && std::isspace(static_cast<unsigned char>(md5.back()))) md5.remove_suffix(1);
    return md5.size() == 32 && std::all_of(md5.begin(), md5.end(), [](unsigned char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
  }
};
