#include "DownloadedArchiveWorkflow.h"

#include "../BmsChartFile.h"
#include "../CanonicalDigest.h"
#include "../RAII.h"
#include "../bms_parser.hpp"
#include "../sqlite3.h"
#include "../path.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <thread>
#include <cctype>
#include <limits>
#include <new>
#include <stdexcept>
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
  result.presentationMessage = i18n::message("library.find_bms.result.cancelled");
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

// The attempt owns this private index, outside the promoted extracted tree.
// File names and chart identities stay on disk with a bounded SQLite cache.
UniqueResource<sqlite3, sqlite3_close> chartIdentityDatabase(const std::filesystem::path &path) {
  sqlite3 *raw = nullptr;
  const auto opened = sqlite3_open_v2(fspath_to_utf8(path).c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
  UniqueResource<sqlite3, sqlite3_close> database(raw);
  if (opened != SQLITE_OK || sqlite3_exec(raw,
      "PRAGMA temp_store=FILE; PRAGMA cache_size=-1024; PRAGMA cache_spill=ON;"
      "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;"
      "CREATE TABLE charts(path BLOB PRIMARY KEY, digest BLOB NOT NULL) WITHOUT ROWID;"
      "CREATE TABLE original_files(path BLOB PRIMARY KEY) WITHOUT ROWID; BEGIN;",
      nullptr, nullptr, nullptr) != SQLITE_OK)
    throw std::runtime_error("Cannot create chart identity index");
  return database;
}
bool chartIdentities(const std::filesystem::path &root,
                     const storage_management::Checkpoint &checkpoint, sqlite3 *database, bool verify) {
  sqlite3_stmt *raw = nullptr;
  const auto prepared = sqlite3_prepare_v2(database, verify
      ? "DELETE FROM charts WHERE path=? AND digest=?" : "INSERT INTO charts VALUES(?,?)", -1, &raw, nullptr);
  UniqueResource<sqlite3_stmt, sqlite3_finalize> statement(raw);
  if (prepared != SQLITE_OK) return false;
  sqlite3_stmt *filesRaw = nullptr;
  const auto filesPrepared = verify ? SQLITE_OK : sqlite3_prepare_v2(database,
      "INSERT INTO original_files VALUES(?)", -1, &filesRaw, nullptr);
  UniqueResource<sqlite3_stmt, sqlite3_finalize> filesStatement(filesRaw);
  if (filesPrepared != SQLITE_OK) return false;
  std::error_code error;
  if (!std::filesystem::is_directory(std::filesystem::symlink_status(root, error)) || error) return {};
  std::filesystem::recursive_directory_iterator it(root, error), end;
  for (; !error && it != end; it.increment(error)) {
    if (!checkpoint()) return {};
    const auto status = it->symlink_status(error);
    if (error || std::filesystem::is_symlink(status)) return {};
    const auto path = it->path().lexically_relative(root).generic_u8string();
    if (!verify && std::filesystem::is_regular_file(status) &&
        (sqlite3_bind_blob64(filesRaw, 1, path.data(), path.size(), SQLITE_TRANSIENT) != SQLITE_OK ||
         sqlite3_step(filesRaw) != SQLITE_DONE || sqlite3_reset(filesRaw) != SQLITE_OK ||
         sqlite3_clear_bindings(filesRaw) != SQLITE_OK)) return false;
    if (!asobmshow::bms_chart_file::isBmsChartPath(it->path())) continue;
    if (!std::filesystem::is_regular_file(status) || std::filesystem::hard_link_count(it->path(), error) != 1 || error) return {};
    std::ifstream input(it->path(), std::ios::binary);
    if (!input) return {};
    bms_parser::SHA256 hash; hash.init();
    std::array<unsigned char, archiveVerificationChunkBytes> buffer;
    while (input) {
      if (!checkpoint()) return {};
      input.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
      if (input.gcount() > 0) hash.update(buffer.data(), static_cast<unsigned int>(input.gcount()));
    }
    if (!input.eof()) return {};
    std::array<unsigned char, 32> digest{}; hash.final(digest.data());
    if (sqlite3_bind_blob64(raw, 1, path.data(), path.size(), SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_blob(raw, 2, digest.data(), digest.size(), SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_step(raw) != SQLITE_DONE || (verify && sqlite3_changes(database) != 1) ||
        sqlite3_reset(raw) != SQLITE_OK || sqlite3_clear_bindings(raw) != SQLITE_OK) return false;
  }
  if (error || !checkpoint()) return {};
  if (!verify) return true;
  // A changed/added chart fails its DELETE; any row left here is a removed chart.
  statement.reset(); raw = nullptr;
  const auto remaining = sqlite3_prepare_v2(database, "SELECT 1 FROM charts LIMIT 1", -1, &raw, nullptr);
  statement.reset(raw);
  return remaining == SQLITE_OK && sqlite3_step(raw) == SQLITE_DONE;
}

} // namespace

ExtractedArchiveDecision
decideExtractedArchive(const std::filesystem::path &root,
                       const std::string &archiveKey,
                       archive_file::PauseCallback pauseCallback,
                       ArchiveVerificationLimits limits) try {
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
} catch (const std::bad_alloc &) {
  return {.message = "Not enough memory to verify the selected chart."};
} catch (const std::length_error &) {
  return {.message = "Selected chart size is not representable."};
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
  result.presentationMessage = {};
  result.storageReport.reset();
  if (!dependencies.decideArchive || !dependencies.commitArtifact) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = "Find BMS archive processing is unavailable.";
    result.presentationMessage = i18n::message("library.find_bms.result.processing_unavailable");
    return false;
  }
  if (reportCancelled(cancelled, result)) {
    return false;
  }

  const bool optimize = !storage_management::IsNoOp(request.options.storagePolicy);
  const bool keepArchive = request.options.skipUnarchivingForNonSolidArchives && !optimize;
  if (keepArchive) {
    reportProgress(progressCallback, "Inspecting downloaded archive");
    reportProgress(progressCallback, "Validating archive contents");
  }
  const auto directDecision = dependencies.decideArchive(
      request.attempt.archivePath, request.archiveKey,
      keepArchive,
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
      if (commitError.empty()) {
        result.presentationMessage = i18n::message("library.find_bms.result.keep_archive_failed");
      }
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
    result.presentationMessage = i18n::message("library.find_bms.result.archive_mismatch");
    return true;
  }

  if (!dependencies.extractArchive || !dependencies.decideExtracted) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = "Find BMS archive extraction is unavailable.";
    result.presentationMessage = i18n::message("library.find_bms.result.extraction_unavailable");
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
    if (extractError.empty()) {
      result.presentationMessage = i18n::message("library.find_bms.result.extraction_failed");
    }
    return false;
  }
  if (reportCancelled(cancelled, result)) {
    return false;
  }

  ArchiveVerificationLimits verificationLimits;
  if (directDecision.verificationBytes > verificationLimits.maxTotalBytes) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = "Archive exceeds the BMS verification byte limit.";
    result.presentationMessage = i18n::message("library.find_bms.result.verification_limit");
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
    if (extractedDecision.message.empty()) {
      result.presentationMessage = i18n::message("library.find_bms.result.validation_failed");
    }
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
    result.presentationMessage = i18n::message("library.find_bms.result.extracted_mismatch");
    return true;
  }

  auto artifact = extractedArtifact(request);
  if (optimize) {
    const auto checkpoint = [&] {
      while (!cancelled.load() && request.options.storagePauseRequested &&
             request.options.storagePauseRequested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
      return !cancelled.load();
    };
    auto failOptimization = [&](const char *key, const std::string &message) {
      if (!reportCancelled(cancelled, result)) {
        result.status = BmsSearchResult::Status::DownloadFailed;
        result.message = message;
        result.presentationMessage = i18n::message(key);
      }
      return false;
    };
    auto storageProgress = [&](const storage_management::StorageProgress &progress) {
      if (progressCallback) progressCallback({.message = "Optimizing downloaded media", .storageProgress = progress});
    };
    try {
      if (!dependencies.optimizeExtracted)
        return failOptimization("library.find_bms.storage.unavailable", "Storage optimization is unavailable.");
      artifact.originalFilesIndexPath = request.attempt.root / "original-files.sqlite";
      const auto identities = chartIdentityDatabase(artifact.originalFilesIndexPath);
      if (!chartIdentities(request.attempt.extractedPath, checkpoint, identities.get(), false))
        return failOptimization("library.find_bms.storage.identity_failed", "Could not verify downloaded chart identities.");
      result.storageReport = dependencies.optimizeExtracted(request.attempt.extractedPath,
          request.options.storagePolicy, checkpoint, storageProgress);
      const auto &report = *result.storageReport;
      if (report.cancelled) cancelled.store(true);
      if (reportCancelled(cancelled, result)) return false;
      if (report.recoveryRequired || report.cleanupPending)
        return failOptimization("library.find_bms.storage.recovery_failed", "Storage cleanup did not finish; downloaded files were not installed.");
      storageProgress({.phase = storage_management::StoragePhase::Verify});
      if (!chartIdentities(request.attempt.extractedPath, checkpoint, identities.get(), true))
        return failOptimization("library.find_bms.storage.identity_failed", "Downloaded chart identities changed; files were not installed.");
      if (sqlite3_exec(identities.get(), "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
        return failOptimization("library.find_bms.storage.failed", "Could not save downloaded file ownership; files were not installed.");
      storageProgress({.phase = storage_management::StoragePhase::Commit});
      if (!checkpoint() || reportCancelled(cancelled, result)) {
        reportCancelled(cancelled, result); return false;
      }
    } catch (const std::exception &) {
      return failOptimization("library.find_bms.storage.failed", "Could not safely optimize downloaded media; files were not installed.");
    }
  }
  if (reportCancelled(cancelled, result)) return false;
  std::string commitError;
  std::vector<std::filesystem::path> removedPaths;
  if (!dependencies.commitArtifact(artifact, commitError, removedPaths)) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = commitError.empty() ? "Could not keep unarchived files."
                                         : commitError;
    if (commitError.empty()) {
      result.presentationMessage = i18n::message("library.find_bms.result.keep_extracted_failed");
    }
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
