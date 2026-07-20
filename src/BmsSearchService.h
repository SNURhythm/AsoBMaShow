#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct BmsSearchDownloadProgress {
  std::string message;
  std::uint64_t downloadedBytes = 0;
  std::uint64_t totalBytes = 0;
};

using BmsSearchDownloadProgressCallback =
    std::function<void(const BmsSearchDownloadProgress &)>;

struct BmsSearchDownloadOptions {
  bool skipUnarchivingForNonSolidArchives = false;
};

enum class BmsSearchPendingArtifactKind { Archive, ExtractedDirectory };

struct BmsSearchPendingArtifact {
  BmsSearchPendingArtifactKind kind = BmsSearchPendingArtifactKind::Archive;
  std::filesystem::path stagingRoot;
  std::filesystem::path sourcePath;
  std::filesystem::path downloadRoot;
  std::filesystem::path destinationPath;
};

enum class BmsSearchPendingArtifactDecision { Keep, Delete };

struct BmsSearchCandidate {
  enum class Source {
    Horie,
  };

  Source source = Source::Horie;
  std::string id;
  std::string name;
  std::string title;
  std::string artist;
  std::string query;
  std::string sourceUrl;
};

struct BmsSearchResult {
  enum class Status {
    NotFound,
    NoDownloadLink,
    UnsupportedLink,
    Downloaded,
    HashMismatch,
    AmbiguousCandidates,
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
  std::vector<BmsSearchCandidate> candidates;
  std::optional<BmsSearchPendingArtifact> pendingArtifact;
};

class BmsSearchService {
public:
  static constexpr const char *kBaseUrl = "https://bmssearch.net";
  static constexpr const char *kSkipUnarchivingSettingLabel =
      "Skip unarchiving for non-solid archives";

  BmsSearchResult findAndDownload(
      const std::string &sha256, const std::string &md5,
      const std::filesystem::path &libraryRoot, std::atomic_bool &cancelled,
      BmsSearchDownloadProgressCallback progressCallback = nullptr,
      const std::string &title = "", const std::string &artist = "",
      BmsSearchDownloadOptions options = {}) const;

  BmsSearchResult downloadCandidate(
      const BmsSearchCandidate &candidate, const std::string &sha256,
      const std::string &md5, const std::filesystem::path &libraryRoot,
      std::atomic_bool &cancelled,
      BmsSearchDownloadProgressCallback progressCallback = nullptr,
      BmsSearchDownloadOptions options = {}) const;

  BmsSearchResult resolvePendingArtifact(
      BmsSearchResult result,
      BmsSearchPendingArtifactDecision decision) const;

  static std::string patternUrlForSha256(const std::string &sha256);
  static std::string searchUrlForText(const std::string &query);
  static std::string googleSearchUrlForSha256(const std::string &sha256);
};
