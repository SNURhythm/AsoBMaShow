#pragma once

#include "ArchiveDecision.h"
#include "DownloadStaging.h"

#include <atomic>
#include <functional>
#include <string>

namespace asobmshow::bms_search {

enum class ExtractedArchiveDisposition { Match, HashMismatch, Inconclusive };

struct ExtractedArchiveDecision {
  ExtractedArchiveDisposition disposition =
      ExtractedArchiveDisposition::Inconclusive;
  bool foundBmsFile = false;
  std::string message;
};

ExtractedArchiveDecision
decideExtractedArchive(const std::filesystem::path &root,
                       const std::string &archiveKey);

struct DownloadedArchiveWorkflowRequest {
  FindBmsDownloadAttempt attempt;
  std::filesystem::path downloadRoot;
  std::string archiveName;
  std::string storageKey;
  std::string archiveKey;
  BmsSearchDownloadOptions options;
};

struct DownloadedArchiveWorkflowDependencies {
  std::function<DirectArchiveDecision(
      const std::filesystem::path &, const std::string &, bool,
      archive_file::PauseCallback)>
      decideArchive;
  std::function<bool(const std::filesystem::path &,
                     const std::filesystem::path &, std::string &,
                     BmsSearchDownloadProgressCallback)>
      extractArchive;
  std::function<ExtractedArchiveDecision(const std::filesystem::path &,
                                         const std::string &)>
      decideExtracted;
  std::function<bool(const BmsSearchPendingArtifact &, std::string &,
                     std::vector<std::filesystem::path> &)>
      commitArtifact;
};

bool processDownloadedArchive(
    const DownloadedArchiveWorkflowRequest &request,
    std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progressCallback,
    BmsSearchResult &result,
    const DownloadedArchiveWorkflowDependencies &dependencies);

} // namespace asobmshow::bms_search
