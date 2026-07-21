#include "DownloadedArchiveWorkflow.h"

#include "../BmsChartFile.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <optional>
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

bool isHexKey(const std::string &value, std::size_t length) {
  return value.size() == length &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isxdigit(character) != 0;
         });
}

std::optional<std::vector<unsigned char>>
readFileBytes(const std::filesystem::path &path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > static_cast<std::uintmax_t>(
                          std::numeric_limits<std::size_t>::max())) {
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  }
  if (input.gcount() != static_cast<std::streamsize>(bytes.size()) ||
      (!input && !input.eof())) {
    return std::nullopt;
  }
  return bytes;
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
                       const std::string &archiveKey) {
  std::error_code error;
  if (!std::filesystem::is_directory(root, error) || error) {
    return {.message = "Could not inspect extracted archive contents."};
  }

  const std::string key = normalizedKey(archiveKey);
  const bool matchSha256 = isHexKey(key, 64);
  const bool matchMd5 = isHexKey(key, 32);
  bool foundBmsFile = false;
  bool incompleteRead = false;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  const auto end = std::filesystem::recursive_directory_iterator();
  while (!error && iterator != end) {
    std::error_code entryError;
    if (iterator->is_regular_file(entryError)) {
      if (asobmshow::bms_chart_file::isBmsChartPath(iterator->path())) {
        foundBmsFile = true;
        if (matchSha256 || matchMd5) {
          const auto bytes = readFileBytes(iterator->path());
          if (!bytes) {
            incompleteRead = true;
          } else if (matchSha256 && bms_parser::sha256(*bytes) == key) {
            return {.disposition = ExtractedArchiveDisposition::Match,
                    .foundBmsFile = true};
          } else if (matchMd5) {
            const std::string text(bytes->begin(), bytes->end());
            if (bms_parser::md5(text) == key) {
              return {.disposition = ExtractedArchiveDisposition::Match,
                      .foundBmsFile = true};
            }
          }
        }
      }
    } else if (entryError) {
      incompleteRead = true;
    }
    iterator.increment(error);
  }
  if (error || incompleteRead) {
    return {.foundBmsFile = foundBmsFile,
            .message = "Could not read every extracted archive file."};
  }
  if (!matchSha256 && !matchMd5) {
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

  if (directDecision.disposition == DirectArchiveDisposition::KeepArchive) {
    reportProgress(progressCallback, "Saving downloaded archive");
    const auto artifact = archiveArtifact(request);
    std::string commitError;
    if (!dependencies.commitArtifact(artifact, commitError)) {
      result.status = BmsSearchResult::Status::DownloadFailed;
      result.message = commitError.empty() ? "Could not keep downloaded archive."
                                           : commitError;
      return false;
    }
    result.status = BmsSearchResult::Status::Downloaded;
    result.outputPath = artifact.destinationPath;
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
                                   progressCallback)) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = extractError.empty() ? "Archive extraction failed."
                                          : extractError;
    return false;
  }
  if (reportCancelled(cancelled, result)) {
    return false;
  }

  const auto extractedDecision = dependencies.decideExtracted(
      request.attempt.extractedPath, request.archiveKey);
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
  if (!dependencies.commitArtifact(artifact, commitError)) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = commitError.empty() ? "Could not keep unarchived files."
                                         : commitError;
    return false;
  }
  result.status = BmsSearchResult::Status::Downloaded;
  result.outputPath = artifact.destinationPath;
  result.message = extractedDecision.foundBmsFile
                       ? "Downloaded and unarchived BMS archive."
                       : "Archive unarchived, but no BMS file was found.";
  return true;
}

} // namespace asobmshow::bms_search
