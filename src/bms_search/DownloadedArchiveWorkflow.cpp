#include "DownloadedArchiveWorkflow.h"

#include "../BmsChartFile.h"
#include "../CanonicalDigest.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <system_error>
#include <vector>

namespace asobmshow::bms_search {
namespace {

std::string normalizedKey(const std::string &value) {
  const auto first = std::find_if_not(
      value.begin(), value.end(),
      [](unsigned char character) { return std::isspace(character) != 0; });
  const auto last = std::find_if_not(
                        value.rbegin(), value.rend(),
                        [](unsigned char character) {
                          return std::isspace(character) != 0;
                        })
                        .base();
  if (first >= last) {
    return {};
  }
  std::string result(first, last);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return result;
}

void reportProgress(const BmsSearchDownloadProgressCallback &callback,
                    const std::string &message) {
  if (callback) {
    callback({.message = message});
  }
}

bool reportCancelled(std::atomic_bool &cancelled, BmsSearchResult &result) {
  if (!cancelled.load()) {
    return false;
  }
  result.status = BmsSearchResult::Status::DownloadFailed;
  result.message = "Lookup cancelled.";
  result.outputPath.clear();
  result.removedPaths.clear();
  result.pendingArtifact.reset();
  return true;
}

BmsSearchPendingArtifact archiveArtifact(
    const DownloadedArchiveWorkflowRequest &request) {
  return {.kind = BmsSearchPendingArtifactKind::Archive,
          .stagingRoot = request.attempt.root,
          .sourcePath = request.attempt.archivePath,
          .downloadRoot = request.downloadRoot,
          .destinationPath =
              request.downloadRoot / "_archives" / request.archiveName,
          .archiveName = request.archiveName,
          .storageKey = request.storageKey,
          .alternateDestinationPath =
              request.downloadRoot / request.storageKey};
}

BmsSearchPendingArtifact extractedArtifact(
    const DownloadedArchiveWorkflowRequest &request) {
  return {.kind = BmsSearchPendingArtifactKind::ExtractedDirectory,
          .stagingRoot = request.attempt.root,
          .sourcePath = request.attempt.extractedPath,
          .downloadRoot = request.downloadRoot,
          .destinationPath = request.downloadRoot / request.storageKey,
          .archiveName = request.archiveName,
          .storageKey = request.storageKey,
          .alternateDestinationPath =
              request.downloadRoot / "_archives" / request.archiveName};
}

} // namespace

ExtractedArchiveDecision
decideExtractedArchive(const std::filesystem::path &root,
                       const std::string &archiveKey,
                       archive_file::PauseCallback pauseCallback,
                       ArchiveVerificationLimits limits) {
  std::atomic_bool cancelled = false;
  const auto checkpoint = [pauseCallback, &cancelled] {
    if (cancelled.load()) return false;
    if (pauseCallback && !pauseCallback()) cancelled.store(true);
    return !cancelled.load();
  };
  if (!checkpoint()) return {.message = "Archive verification cancelled."};
  std::error_code error;
  if (!std::filesystem::is_directory(root, error) || error) {
    return {.message = "Could not inspect extracted archive contents."};
  }

  const std::string key = normalizedKey(archiveKey);
  const bool matchSha256 =
      canonical_digest::isCanonicalLowerHex(key, 64);
  const bool matchMd5 = canonical_digest::isCanonicalLowerHex(key, 32);
  bool foundBmsFile = false;
  std::vector<std::filesystem::path> bmsPaths;
  std::uint64_t declaredBytes = 0;
  std::uint64_t inspectedEntries = 0;
  std::filesystem::recursive_directory_iterator iterator(
      root, error);
  const auto end = std::filesystem::recursive_directory_iterator();
  while (!error && iterator != end) {
    if (!checkpoint()) return {.message = "Archive verification cancelled."};
    if (inspectedEntries >= limits.maxEntries) {
      return {.message = "Archive exceeds the BMS verification entry-count limit."};
    }
    ++inspectedEntries;
    std::error_code entryError;
    if (iterator->is_regular_file(entryError)) {
      if (asobmshow::bms_chart_file::isBmsChartPath(iterator->path())) {
        foundBmsFile = true;
        const auto size = iterator->file_size(entryError);
        if (entryError) return {.message = "Could not size an extracted BMS file."};
        if (size > limits.maxMemberBytes ||
            size > limits.maxTotalBytes - declaredBytes) {
          return {.message = "Archive exceeds the BMS verification byte limit."};
        }
        declaredBytes += size;
        bmsPaths.push_back(iterator->path());
      }
    } else if (entryError) {
      return {.message = "Could not inspect an extracted archive entry."};
    }
    iterator.increment(error);
  }
  if (error) {
    return {.foundBmsFile = foundBmsFile,
            .message = "Could not read every extracted archive file."};
  }
  bool matched = !matchSha256 && !matchMd5;
  std::uint64_t actualBytes = 0;
  for (const auto &path : bmsPaths) {
    std::vector<unsigned char> bytes;
    std::string readError;
    const auto maximumBytes = static_cast<std::size_t>(std::min({
        limits.maxMemberBytes, limits.maxTotalBytes - actualBytes,
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())}));
    if (!archive_file::readFileBoundedWithCheckpoint(
            path, bytes, maximumBytes, &readError, {}, checkpoint)) {
      return {.message = cancelled.load() ? "Archive verification cancelled."
                         : readError.empty()
                             ? "Could not read an extracted BMS file."
                             : readError};
    }
    actualBytes += bytes.size();
    const auto match = matchesArchiveChartHash(bytes, key, checkpoint);
    if (!match) return {.message = "Archive verification cancelled."};
    matched = matched || *match;
  }
  if (!checkpoint()) return {.message = "Archive verification cancelled."};
  if (matched) {
    return {.disposition = ExtractedArchiveDisposition::Match,
            .foundBmsFile = foundBmsFile,
            .message = foundBmsFile
                           ? std::string()
                           : "Archive unarchived, but no BMS file was found."};
  }
  return {.disposition = ExtractedArchiveDisposition::HashMismatch,
          .foundBmsFile = foundBmsFile,
          .message = foundBmsFile
                         ? "Archive did not contain the selected BMS chart."
                         : "Archive did not contain a BMS chart file."};
}

bool processDownloadedArchive(
    const DownloadedArchiveWorkflowRequest &request,
    std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progressCallback,
    BmsSearchResult &result,
    const DownloadedArchiveWorkflowDependencies &dependencies) {
  result.outputPath.clear();
  result.removedPaths.clear();
  result.pendingArtifact.reset();
  if (!dependencies.decideArchive || !dependencies.commitArtifact) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = "Find BMS archive processing is unavailable.";
    return false;
  }
  if (reportCancelled(cancelled, result)) {
    return false;
  }

  if (request.options.skipUnarchivingForNonSolidArchives) {
    reportProgress(progressCallback, "Inspecting downloaded archive");
    reportProgress(progressCallback, "Validating archive contents");
  }
  const auto directDecision = dependencies.decideArchive(
      request.attempt.archivePath, request.archiveKey,
      request.options.skipUnarchivingForNonSolidArchives,
      [&cancelled] { return !cancelled.load(); });
  if (reportCancelled(cancelled, result)) {
    return false;
  }

  if (directDecision.disposition == DirectArchiveDisposition::Failed) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = directDecision.message;
    return false;
  }

  if (directDecision.disposition == DirectArchiveDisposition::KeepArchive) {
    reportProgress(progressCallback, "Saving downloaded archive");
    if (reportCancelled(cancelled, result)) return false;
    const auto artifact = archiveArtifact(request);
    std::string commitError;
    std::vector<std::filesystem::path> removedPaths;
    if (!dependencies.commitArtifact(artifact, commitError, removedPaths)) {
      result.status = BmsSearchResult::Status::DownloadFailed;
      result.message = commitError.empty() ? "Could not keep downloaded archive."
                                           : commitError;
      return false;
    }
    result.status = BmsSearchResult::Status::Downloaded;
    result.outputPath = artifact.destinationPath;
    result.removedPaths = std::move(removedPaths);
    result.message = directDecision.message.empty()
                         ? "Downloaded BMS archive."
                         : directDecision.message;
    return true;
  }

  if (directDecision.disposition == DirectArchiveDisposition::HashMismatch) {
    result.status = BmsSearchResult::Status::HashMismatch;
    result.pendingArtifact = archiveArtifact(request);
    result.message =
        "The downloaded archive does not contain the selected BMS chart. "
        "Choose Keep Files or Delete Files.";
    return true;
  }

  if (!dependencies.extractArchive || !dependencies.decideExtracted) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = "Find BMS archive extraction is unavailable.";
    return false;
  }

  reportProgress(progressCallback, "Unarchiving archive");
  std::string extractError;
  if (!dependencies.extractArchive(request.attempt.archivePath,
                                   request.attempt.extractedPath, extractError,
                                   progressCallback,
                                   [&cancelled] { return cancelled.load(); })) {
    if (reportCancelled(cancelled, result)) {
      return false;
    }
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = extractError.empty() ? "Archive extraction failed."
                                          : extractError;
    return false;
  }
  if (reportCancelled(cancelled, result)) {
    return false;
  }

  ArchiveVerificationLimits verificationLimits;
  if (directDecision.verificationBytes > verificationLimits.maxTotalBytes) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = "Archive exceeds the BMS verification byte limit.";
    return false;
  }
  verificationLimits.maxTotalBytes -= directDecision.verificationBytes;
  const auto extractedDecision = dependencies.decideExtracted(
      request.attempt.extractedPath, request.archiveKey,
      [&cancelled] { return !cancelled.load(); }, verificationLimits);
  if (reportCancelled(cancelled, result)) return false;
  if (extractedDecision.disposition ==
      ExtractedArchiveDisposition::Inconclusive) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = extractedDecision.message.empty()
                         ? "Could not validate extracted archive contents."
                         : extractedDecision.message;
    return false;
  }
  if (extractedDecision.disposition ==
      ExtractedArchiveDisposition::HashMismatch) {
    std::error_code ignoredCleanupError;
    std::filesystem::remove(request.attempt.archivePath, ignoredCleanupError);
    result.status = BmsSearchResult::Status::HashMismatch;
    result.pendingArtifact = extractedArtifact(request);
    result.message =
        "The unarchived files do not contain the selected BMS chart. "
        "Choose Keep Files or Delete Files.";
    return true;
  }

  const auto artifact = extractedArtifact(request);
  std::string commitError;
  std::vector<std::filesystem::path> removedPaths;
  if (!dependencies.commitArtifact(artifact, commitError, removedPaths)) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = commitError.empty() ? "Could not keep unarchived files."
                                         : commitError;
    return false;
  }
  result.status = BmsSearchResult::Status::Downloaded;
  result.outputPath = artifact.destinationPath;
  result.removedPaths = std::move(removedPaths);
  result.message = extractedDecision.foundBmsFile
                       ? "Downloaded and unarchived BMS archive."
                       : "Archive unarchived, but no BMS file was found.";
  return true;
}

} // namespace asobmshow::bms_search
