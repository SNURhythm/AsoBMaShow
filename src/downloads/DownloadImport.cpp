#include "DownloadImport.h"
#include "DownloadPublication.h"
#include "../bms_search/Internal.h"
#include "../bms_search/DownloadedArchiveWorkflow.h"
#include "../bms_search/DownloadStorageIdentity.h"
#include "../bms_search/DownloadStorageOptimization.h"

namespace downloads {
namespace {
using namespace asobmshow::bms_search;
std::filesystem::path importRoot(const Job &job) {
  if (job.id.size() != 32 || !std::all_of(job.id.begin(), job.id.end(), [](unsigned char c) { return std::isxdigit(c); }))
    throw std::runtime_error("Invalid import identity.");
  auto id = job.id;
  id[12] = '4'; id[16] = '8';
  id.insert(20, "-"); id.insert(16, "-"); id.insert(12, "-"); id.insert(8, "-");
  return findBmsStagingBasePath() / id;
}
BmsSearchResult failed(const std::string &message) {
  return {.status = BmsSearchResult::Status::DownloadFailed, .message = message};
}
bool publish(const BmsSearchPendingArtifact &artifact, const std::filesystem::path &privateRoot,
             const BeforePublication &prepare, std::string &error,
             std::vector<std::filesystem::path> &removed) {
  PublicationJournal journal(privateRoot / "publication.json", artifact.destinationPath);
  const auto recovery = journal.recover();
  if (recovery == PublicationRecovery::Conflict) {
    error = "Downloaded files changed during recovery. Existing files were left untouched.";
    return false;
  }
  if (recovery == PublicationRecovery::Installed) {
    if (!prepare || !prepare(artifact)) { error = "Import paused during recovery."; return false; }
    return finishFindBmsPendingArtifactPublication(artifact, error, &removed);
  }
  const auto transaction = artifact.downloadRoot / kFindBmsTransactionDirectoryName / artifact.stagingRoot.filename();
  if (!journal.beginCopy(transaction)) { error = "Could not save the import recovery journal."; return false; }
  bool swapping = false;
  return commitFindBmsPendingArtifact(artifact, error,
      [&](const auto &from, const auto &to, auto &failure) {
        if (!swapping && (!prepare || !prepare(artifact))) {
          failure = std::make_error_code(std::errc::operation_canceled);
          return;
        }
        swapping = true;
        journal.rename(from, to, failure);
      }, &removed);
}
}
BmsSearchResult importArchiveJob(const Job &job, const std::filesystem::path &archive,
    const std::filesystem::path &privateJobRoot, std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progress, BeforePublication beforePublication) {
  using namespace asobmshow::bms_search;
  if (cancelled.load()) return failed("Import paused.");
  std::error_code error;
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(archive, error)) || error)
    return failed("Downloaded archive is no longer available. Retry the download.");
  const auto extension = archiveExtensionFromName(job.request.archiveName);
  const auto names = findBmsStorageNames(job.request.archiveName,
      extension.empty() ? ".archive" : extension,
      job.request.storageIdentity.empty() ? job.request.url : job.request.storageIdentity);
  const auto root = importRoot(job);
  std::filesystem::create_directories(root);
  std::filesystem::permissions(root, std::filesystem::perms::owner_all);
  const FindBmsDownloadAttempt attempt{root, root / names.archiveName, root / "extracted"};
  // Only this job owns the attempt. Restart interrupted extraction; retain the
  // immutable OS-completed archive and never fetch it again for import retries.
  std::filesystem::remove_all(attempt.extractedPath);
  std::filesystem::remove(root / "original-files.sqlite");
  std::filesystem::remove(attempt.archivePath);
  std::filesystem::create_hard_link(archive, attempt.archivePath, error);
  if (error) return failed("Could not stage the retained archive on its private volume.");
  std::filesystem::create_directory(attempt.extractedPath);
  BmsSearchDownloadOptions options;
  options.skipUnarchivingForNonSolidArchives = job.request.skipUnarchiving;
  options.storagePolicy = job.request.storagePolicy;
  options.storagePauseRequested = [&cancelled] { return cancelled.load(); };
  const auto reader = defaultArchiveReaderDependencies();
  DownloadedArchiveWorkflowDependencies dependencies{
      .decideArchive = [reader](const auto &path, const auto &hash, bool skip, auto checkpoint) {
        return decideDownloadedArchive(path, hash, skip, std::move(checkpoint), reader);
      },
      .extractArchive = [](const auto &path, const auto &destination, auto &message, auto notify, auto stopped) {
        return extractDownloadedArchive(path, destination, message, std::move(notify), std::move(stopped));
      },
      .decideExtracted = decideExtractedArchive,
      .optimizeExtracted = optimizeExtractedDownload,
      .commitArtifact = [&](const auto &artifact, auto &message, auto &removed) {
        return publish(artifact, privateJobRoot, beforePublication, message, removed);
      }};
  BmsSearchResult result;
  const DownloadedArchiveWorkflowRequest request{attempt, makeDownloadDirectory(job.request.libraryRoot),
      names.archiveName, names.storageKey, job.request.expectedHash, options};
  processDownloadedArchive(request, cancelled, std::move(progress), result, dependencies);
  // The attempt stays until publication/decision. It is private durable storage
  // on mobile, so pending mismatch artifacts survive process termination.
  return result;
}
BmsSearchResult resolveArchiveJob(const Job &job, const std::filesystem::path &privateJobRoot,
    BmsSearchPendingArtifactDecision decision, std::atomic_bool &cancelled,
    BeforePublication beforePublication) {
  if (!job.result || !job.result->pendingArtifact) return failed("No downloaded files need a decision.");
  if (cancelled.load()) return failed("Import paused.");
  auto result = *job.result;
  const auto artifact = *result.pendingArtifact;
  if (artifact.stagingRoot != importRoot(job)) return failed("Invalid import staging identity.");
  std::string error;
  if (decision == BmsSearchPendingArtifactDecision::Keep) {
    if (!publish(artifact, privateJobRoot, beforePublication, error, result.removedPaths)) {
      result.status = BmsSearchResult::Status::DownloadFailed;
      result.message = error;
      return result;
    }
    result.outputPath = artifact.destinationPath;
    result.status = BmsSearchResult::Status::Downloaded;
    result.message = "Downloaded files saved.";
  } else {
    if (job.publicationPrepared) return failed("Recover the interrupted installation before deleting its staging files.");
    if (!asobmshow::bms_search::deleteFindBmsPendingArtifact(artifact, error)) return failed(error);
    result.outputPath.clear();
    result.status = BmsSearchResult::Status::HashMismatch;
    result.message = "Downloaded files deleted.";
  }
  result.pendingArtifact.reset();
  return result;
}
} // namespace downloads
