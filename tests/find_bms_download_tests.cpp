#include "bms_search/ArchiveDecision.h"
#include "bms_search/DownloadedArchiveWorkflow.h"
#include "bms_search/DownloadStaging.h"
#include "bms_search/DownloadStorageIdentity.h"
#include "scene/FindBmsDialogPolicy.h"
#include "scene/FindBmsProgressPresentation.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using asobmshow::bms_search::FindBmsDownloadAttempt;

asobmshow::bms_search::ArchiveReaderDependencies fakeArchiveReader(
    std::vector<archive_file::Entry> entries,
    std::vector<archive_file::FileData> files, bool listSucceeds = true,
    bool readSucceeds = true) {
  return {
      .listEntries =
          [entries = std::move(entries), listSucceeds](
              const std::filesystem::path &,
              std::vector<archive_file::Entry> &output, std::string *,
              archive_file::PauseCallback) {
            output = entries;
            return listSucceeds;
          },
      .readEntries =
          [files = std::move(files), readSucceeds](
              const std::filesystem::path &,
              const std::vector<std::filesystem::path> &,
              std::vector<archive_file::FileData> &output, std::string *,
              archive_file::PauseCallback) {
            output = files;
            return readSucceeds;
          }};
}

class CleanupPaths {
public:
  ~CleanupPaths() {
    for (auto iterator = paths_.rbegin(); iterator != paths_.rend();
         ++iterator) {
      std::error_code ignored;
      std::filesystem::remove_all(*iterator, ignored);
    }
  }

  void add(std::filesystem::path path) { paths_.push_back(std::move(path)); }

private:
  std::vector<std::filesystem::path> paths_;
};

void writeText(const std::filesystem::path &path, const std::string &text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  assert(output);
  output << text;
  assert(output.good());
}

std::string readText(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  assert(input);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::filesystem::path
testDownloadRoot(const FindBmsDownloadAttempt &attempt) {
  return std::filesystem::temp_directory_path() /
         ("AsoBMaShowFindBmsTestLibrary-" +
          attempt.root.filename().string()) /
         "BMSSEARCH";
}

void testStorageNamesDistinguishSameNamedPackages() {
  const auto first = asobmshow::bms_search::findBmsStorageNames(
      "song.zip", ".zip", "provider:file-a");
  const auto second = asobmshow::bms_search::findBmsStorageNames(
      "song.zip", ".zip", "provider:file-b");
  const auto repeated = asobmshow::bms_search::findBmsStorageNames(
      "song.zip", ".zip", "provider:file-a");
  assert(first.storageKey != second.storageKey);
  assert(first.archiveName != second.archiveName);
  assert(first.storageKey == repeated.storageKey);
  assert(first.archiveName == repeated.archiveName);
  assert(first.archiveName == first.storageKey + ".zip");
  const auto delimiter = first.storageKey.rfind("--");
  assert(delimiter != std::string::npos);
  assert(first.storageKey.size() - delimiter - 2 == 16);

  const auto unsafe = asobmshow::bms_search::findBmsStorageNames(
      "../song.zip", ".zip", "provider:file-c");
  assert(unsafe.storageKey.find('/') == std::string::npos);
  assert(unsafe.storageKey != ".");
  assert(unsafe.storageKey != "..");
}

void testStorageNamesPreserveLongAndCompoundExtensions() {
  const auto zip = asobmshow::bms_search::findBmsStorageNames(
      std::string(180, 'a'), ".zip", "zip-id");
  assert(zip.archiveName.ends_with(".zip"));
  assert(zip.archiveName.size() <= 128);
  const auto tar = asobmshow::bms_search::findBmsStorageNames(
      std::string(180, 'b') + ".tar.gz", ".zip", "tar-id");
  assert(tar.archiveName.ends_with(".tar.gz"));
  assert(tar.archiveName.size() <= 128);
}

BmsSearchPendingArtifact extractedArtifact(
    const FindBmsDownloadAttempt &attempt,
    const std::filesystem::path &downloadRoot) {
  return {.kind = BmsSearchPendingArtifactKind::ExtractedDirectory,
          .stagingRoot = attempt.root,
          .sourcePath = attempt.extractedPath,
          .downloadRoot = downloadRoot,
          .destinationPath = downloadRoot / "package"};
}

void testExtractedCommitMergesTransactionally() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto downloadRoot = testDownloadRoot(*attempt);
  cleanup.add(downloadRoot.parent_path());
  const auto artifact = extractedArtifact(*attempt, downloadRoot);
  writeText(artifact.destinationPath / "preserved.txt", "old");
  writeText(artifact.destinationPath / "chart.bms", "old chart");
  writeText(attempt->extractedPath / "chart.bms", "new chart");

  assert(asobmshow::bms_search::commitFindBmsPendingArtifact(artifact, error));
  assert(readText(artifact.destinationPath / "chart.bms") == "new chart");
  assert(readText(artifact.destinationPath / "preserved.txt") == "old");
  assert(!std::filesystem::exists(attempt->root));
}

void testDeleteRemovesOnlyAttempt() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto downloadRoot = testDownloadRoot(*attempt);
  cleanup.add(downloadRoot.parent_path());
  const auto artifact = extractedArtifact(*attempt, downloadRoot);
  writeText(artifact.destinationPath / "existing.bms", "existing");
  writeText(attempt->extractedPath / "wrong.bms", "wrong");

  assert(asobmshow::bms_search::deleteFindBmsPendingArtifact(artifact, error));
  assert(!std::filesystem::exists(attempt->root));
  assert(readText(artifact.destinationPath / "existing.bms") == "existing");
}

void testUnsafeStagingRootIsRefused() {
  const auto base = asobmshow::bms_search::findBmsStagingBasePath();
  const BmsSearchPendingArtifact artifact{
      .kind = BmsSearchPendingArtifactKind::ExtractedDirectory,
      .stagingRoot = base,
      .sourcePath = base / "extracted",
      .downloadRoot = base / "library/BMSSEARCH",
      .destinationPath = base / "library/BMSSEARCH/package"};
  std::string error;
  assert(!asobmshow::bms_search::deleteFindBmsPendingArtifact(artifact,
                                                              error));
  assert(!error.empty());
}

void testCommitRestoresDestinationWhenSwapFails() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto downloadRoot = testDownloadRoot(*attempt);
  cleanup.add(downloadRoot.parent_path());
  const auto artifact = extractedArtifact(*attempt, downloadRoot);
  writeText(artifact.destinationPath / "chart.bms", "old chart");
  writeText(attempt->extractedPath / "chart.bms", "new chart");
  auto failCommitSwap =
      [destination = artifact.destinationPath](
          const std::filesystem::path &from, const std::filesystem::path &to,
          std::error_code &renameError) {
        if (from.filename().string().find(".commit-") != std::string::npos &&
            to == destination) {
          renameError = std::make_error_code(std::errc::io_error);
          return;
        }
        std::filesystem::rename(from, to, renameError);
      };

  assert(!asobmshow::bms_search::commitFindBmsPendingArtifact(
      artifact, error, failCommitSwap));
  assert(readText(artifact.destinationPath / "chart.bms") == "old chart");
  assert(std::filesystem::exists(attempt->root));
  assert(!error.empty());
}

void testArchiveCommitAndResolution() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto downloadRoot = testDownloadRoot(*attempt);
  cleanup.add(downloadRoot.parent_path());
  writeText(attempt->archivePath, "archive bytes");
  const BmsSearchPendingArtifact artifact{
      .kind = BmsSearchPendingArtifactKind::Archive,
      .stagingRoot = attempt->root,
      .sourcePath = attempt->archivePath,
      .downloadRoot = downloadRoot,
      .destinationPath = downloadRoot / "_archives/package.zip"};
  BmsSearchResult result;
  result.status = BmsSearchResult::Status::HashMismatch;
  result.pendingArtifact = artifact;

  BmsSearchService service;
  result = service.resolvePendingArtifact(
      std::move(result), BmsSearchPendingArtifactDecision::Keep);
  assert(!result.pendingArtifact);
  assert(result.outputPath == artifact.destinationPath);
  assert(readText(artifact.destinationPath) == "archive bytes");
}

void testArchiveCommitRemovesExtractedAlternate() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto downloadRoot = testDownloadRoot(*attempt);
  cleanup.add(downloadRoot.parent_path());
  writeText(attempt->archivePath, "archive bytes");
  const auto extractedAlternate = downloadRoot / "package";
  writeText(extractedAlternate / "stale.bms", "stale chart");
  const BmsSearchPendingArtifact artifact{
      .kind = BmsSearchPendingArtifactKind::Archive,
      .stagingRoot = attempt->root,
      .sourcePath = attempt->archivePath,
      .downloadRoot = downloadRoot,
      .destinationPath = downloadRoot / "_archives/package.zip",
      .archiveName = "package.zip",
      .storageKey = "package",
      .alternateDestinationPath = extractedAlternate};

  assert(asobmshow::bms_search::commitFindBmsPendingArtifact(artifact, error));
  assert(readText(artifact.destinationPath) == "archive bytes");
  assert(!std::filesystem::exists(extractedAlternate));
}

void testExtractedCommitRemovesArchiveAlternate() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto downloadRoot = testDownloadRoot(*attempt);
  cleanup.add(downloadRoot.parent_path());
  writeText(attempt->extractedPath / "chart.bms", "new chart");
  const auto archiveAlternate = downloadRoot / "_archives/package.zip";
  writeText(archiveAlternate, "stale archive");
  const BmsSearchPendingArtifact artifact{
      .kind = BmsSearchPendingArtifactKind::ExtractedDirectory,
      .stagingRoot = attempt->root,
      .sourcePath = attempt->extractedPath,
      .downloadRoot = downloadRoot,
      .destinationPath = downloadRoot / "package",
      .archiveName = "package.zip",
      .storageKey = "package",
      .alternateDestinationPath = archiveAlternate};

  assert(asobmshow::bms_search::commitFindBmsPendingArtifact(artifact, error));
  assert(readText(artifact.destinationPath / "chart.bms") == "new chart");
  assert(!std::filesystem::exists(archiveAlternate));
}

void testExtractedCommitRemovesSameKeyArchiveWithDifferentExtension() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto downloadRoot = testDownloadRoot(*attempt);
  cleanup.add(downloadRoot.parent_path());
  writeText(attempt->extractedPath / "chart.bms", "new chart");
  const auto staleArchive = downloadRoot / "_archives/package.7z";
  writeText(staleArchive, "stale archive");
  const BmsSearchPendingArtifact artifact{
      .kind = BmsSearchPendingArtifactKind::ExtractedDirectory,
      .stagingRoot = attempt->root,
      .sourcePath = attempt->extractedPath,
      .downloadRoot = downloadRoot,
      .destinationPath = downloadRoot / "package",
      .archiveName = "package.zip",
      .storageKey = "package",
      .alternateDestinationPath = downloadRoot / "_archives/package.zip"};

  assert(asobmshow::bms_search::commitFindBmsPendingArtifact(artifact, error));
  assert(readText(artifact.destinationPath / "chart.bms") == "new chart");
  assert(!std::filesystem::exists(staleArchive));
}

#if !defined(_WIN32)
void testAlternateCleanupFailureDoesNotFailCommit() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto downloadRoot = testDownloadRoot(*attempt);
  cleanup.add(downloadRoot.parent_path());
  writeText(attempt->archivePath, "archive bytes");
  const auto extractedAlternate = downloadRoot / "package";
  writeText(extractedAlternate / "stale.bms", "stale chart");
  const BmsSearchPendingArtifact artifact{
      .kind = BmsSearchPendingArtifactKind::Archive,
      .stagingRoot = attempt->root,
      .sourcePath = attempt->archivePath,
      .downloadRoot = downloadRoot,
      .destinationPath = downloadRoot / "_archives/package.zip",
      .archiveName = "package.zip",
      .storageKey = "package",
      .alternateDestinationPath = extractedAlternate};
  std::error_code permissionError;
  std::filesystem::permissions(
      extractedAlternate,
      std::filesystem::perms::owner_read |
          std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace, permissionError);
  assert(!permissionError);

  const bool committed =
      asobmshow::bms_search::commitFindBmsPendingArtifact(artifact, error);
  std::error_code restoreError;
  std::filesystem::permissions(extractedAlternate,
                               std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace,
                               restoreError);

  assert(!restoreError);
  assert(committed);
  assert(error.empty());
  assert(readText(artifact.destinationPath) == "archive bytes");
  assert(std::filesystem::exists(extractedAlternate / "stale.bms"));
  assert(!std::filesystem::exists(attempt->root));
}
#endif

void testFailedResolutionRetainsPendingArtifact() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto downloadRoot = testDownloadRoot(*attempt);
  cleanup.add(downloadRoot.parent_path());
  const BmsSearchPendingArtifact artifact{
      .kind = BmsSearchPendingArtifactKind::Archive,
      .stagingRoot = attempt->root,
      .sourcePath = attempt->archivePath,
      .downloadRoot = downloadRoot,
      .destinationPath = downloadRoot / "_archives/package.zip"};
  writeText(artifact.destinationPath, "existing archive");
  BmsSearchResult result;
  result.status = BmsSearchResult::Status::HashMismatch;
  result.pendingArtifact = artifact;

  BmsSearchService service;
  result = service.resolvePendingArtifact(
      std::move(result), BmsSearchPendingArtifactDecision::Keep);
  assert(result.pendingArtifact);
  assert(result.pendingArtifact->stagingRoot == artifact.stagingRoot);
  assert(readText(artifact.destinationPath) == "existing archive");
  assert(std::filesystem::exists(attempt->root));
  assert(!result.message.empty());
}

void testAlreadyMissingAttemptCanBeDeleted() {
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  const auto downloadRoot = testDownloadRoot(*attempt);
  CleanupPaths cleanup;
  cleanup.add(downloadRoot.parent_path());
  const auto artifact = extractedArtifact(*attempt, downloadRoot);
  std::filesystem::remove_all(attempt->root);
  assert(asobmshow::bms_search::deleteFindBmsPendingArtifact(artifact, error));
}

void testUnsafeArchiveNameAndDownloadRootAreRefused() {
  std::string error;
  assert(!asobmshow::bms_search::createFindBmsDownloadAttempt("../bad.zip",
                                                              error));
  assert(!error.empty());

  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  CleanupPaths cleanup;
  cleanup.add(attempt->root);
  writeText(attempt->extractedPath / "chart.bms", "chart");
  const BmsSearchPendingArtifact artifact{
      .kind = BmsSearchPendingArtifactKind::ExtractedDirectory,
      .stagingRoot = attempt->root,
      .sourcePath = attempt->extractedPath,
      .downloadRoot = attempt->root / "BMSSEARCH",
      .destinationPath = attempt->root / "BMSSEARCH/package"};
  error.clear();
  assert(!asobmshow::bms_search::commitFindBmsPendingArtifact(artifact,
                                                              error));
  assert(!error.empty());
}

void testUnsafeAlternateDestinationIsRefused() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto downloadRoot = testDownloadRoot(*attempt);
  cleanup.add(downloadRoot.parent_path());
  writeText(attempt->archivePath, "archive");
  const auto outsidePath = downloadRoot.parent_path() / "outside";
  writeText(outsidePath / "chart.bms", "outside chart");
  const BmsSearchPendingArtifact artifact{
      .kind = BmsSearchPendingArtifactKind::Archive,
      .stagingRoot = attempt->root,
      .sourcePath = attempt->archivePath,
      .downloadRoot = downloadRoot,
      .destinationPath = downloadRoot / "_archives/package.zip",
      .archiveName = "package.zip",
      .storageKey = "package",
      .alternateDestinationPath = outsidePath};

  assert(!asobmshow::bms_search::commitFindBmsPendingArtifact(artifact, error));
  assert(!error.empty());
  assert(std::filesystem::exists(outsidePath / "chart.bms"));
}

void testDirectArchiveSha256MatchStaysPacked() {
  const std::string chart = "#TITLE TEST\n#00111:01\n";
  const std::vector<unsigned char> bytes(chart.begin(), chart.end());
  const std::string sha256 = bms_parser::sha256(bytes);
  const archive_file::Entry entry{.path = "song/chart.bms",
                                  .directory = false,
                                  .size = bytes.size(),
                                  .solid = false};
  const archive_file::FileData file{.path = entry.path, .bytes = bytes};
  const auto decision = asobmshow::bms_search::decideDownloadedArchive(
      "package.zip", sha256, true, nullptr,
      fakeArchiveReader({entry}, {file}));
  assert(decision.disposition ==
         asobmshow::bms_search::DirectArchiveDisposition::KeepArchive);
  assert(decision.foundBmsFile);
}

void testDirectArchiveDisabledDoesNotInspect() {
  bool listed = false;
  asobmshow::bms_search::ArchiveReaderDependencies reader;
  reader.listEntries =
      [&listed](const std::filesystem::path &,
                std::vector<archive_file::Entry> &, std::string *,
                archive_file::PauseCallback) {
        listed = true;
        return true;
      };
  const auto decision = asobmshow::bms_search::decideDownloadedArchive(
      "package.zip", "", false, nullptr, reader);
  assert(decision.disposition ==
         asobmshow::bms_search::DirectArchiveDisposition::Unarchive);
  assert(!listed);
}

void testDirectArchiveMismatchIsConfirmed() {
  const std::string chart = "#TITLE TEST\n";
  const std::vector<unsigned char> bytes(chart.begin(), chart.end());
  const archive_file::Entry entry{.path = "chart.bms",
                                  .directory = false,
                                  .size = bytes.size(),
                                  .solid = false};
  const archive_file::FileData file{.path = entry.path, .bytes = bytes};
  const auto decision = asobmshow::bms_search::decideDownloadedArchive(
      "package.zip", std::string(64, '0'), true, nullptr,
      fakeArchiveReader({entry}, {file}));
  assert(decision.disposition ==
         asobmshow::bms_search::DirectArchiveDisposition::HashMismatch);
}

void testDirectArchiveIncompleteReadFallsBack() {
  const archive_file::Entry entry{.path = "chart.bms",
                                  .directory = false,
                                  .size = 10,
                                  .solid = false};
  const auto decision = asobmshow::bms_search::decideDownloadedArchive(
      "package.zip", std::string(64, '0'), true, nullptr,
      fakeArchiveReader({entry}, {}));
  assert(decision.disposition ==
         asobmshow::bms_search::DirectArchiveDisposition::Unarchive);
}

void testSolidEmptyAndFailedListingsFallBack() {
  const archive_file::Entry solidEntry{.path = "chart.bms",
                                       .directory = false,
                                       .size = 10,
                                       .solid = true};
  auto solid = asobmshow::bms_search::decideDownloadedArchive(
      "package.7z", std::string(64, '0'), true, nullptr,
      fakeArchiveReader({solidEntry}, {}));
  assert(solid.disposition ==
         asobmshow::bms_search::DirectArchiveDisposition::Unarchive);

  auto empty = asobmshow::bms_search::decideDownloadedArchive(
      "package.zip", "", true, nullptr, fakeArchiveReader({}, {}));
  assert(empty.disposition ==
         asobmshow::bms_search::DirectArchiveDisposition::Unarchive);

  auto failed = asobmshow::bms_search::decideDownloadedArchive(
      "package.zip", "", true, nullptr,
      fakeArchiveReader({}, {}, false, true));
  assert(failed.disposition ==
         asobmshow::bms_search::DirectArchiveDisposition::Unarchive);
}

void testDirectArchiveMd5AndNoHashBehavior() {
  const std::string chart = "#TITLE MD5\n";
  const std::vector<unsigned char> bytes(chart.begin(), chart.end());
  const archive_file::Entry entry{.path = "chart.bme",
                                  .directory = false,
                                  .size = bytes.size(),
                                  .solid = false};
  const archive_file::FileData file{.path = entry.path, .bytes = bytes};
  auto md5 = asobmshow::bms_search::decideDownloadedArchive(
      "package.zip", bms_parser::md5(chart), true, nullptr,
      fakeArchiveReader({entry}, {file}));
  assert(md5.disposition ==
         asobmshow::bms_search::DirectArchiveDisposition::KeepArchive);

  auto noHash = asobmshow::bms_search::decideDownloadedArchive(
      "package.zip", "not-a-hash", true, nullptr,
      fakeArchiveReader({entry}, {file}));
  assert(noHash.disposition ==
         asobmshow::bms_search::DirectArchiveDisposition::KeepArchive);
  assert(noHash.foundBmsFile);

  const archive_file::Entry asset{.path = "readme.txt",
                                  .directory = false,
                                  .size = 1,
                                  .solid = false};
  auto noBmsWithHash = asobmshow::bms_search::decideDownloadedArchive(
      "package.zip", std::string(32, '0'), true, nullptr,
      fakeArchiveReader({asset}, {}));
  assert(noBmsWithHash.disposition ==
         asobmshow::bms_search::DirectArchiveDisposition::HashMismatch);

  auto noBmsNoHash = asobmshow::bms_search::decideDownloadedArchive(
      "package.zip", "", true, nullptr, fakeArchiveReader({asset}, {}));
  assert(noBmsNoHash.disposition ==
         asobmshow::bms_search::DirectArchiveDisposition::KeepArchive);
  assert(!noBmsNoHash.foundBmsFile);
}

void testDirectArchiveNoHashUnreadableBmsFallsBack() {
  const archive_file::Entry entry{.path = "chart.bms",
                                  .directory = false,
                                  .size = 10,
                                  .solid = false};
  const auto decision = asobmshow::bms_search::decideDownloadedArchive(
      "package.zip", "not-a-hash", true, nullptr,
      fakeArchiveReader({entry}, {}, true, false));
  assert(decision.disposition ==
         asobmshow::bms_search::DirectArchiveDisposition::Unarchive);
  assert(decision.foundBmsFile);
}

asobmshow::bms_search::DownloadedArchiveWorkflowRequest workflowRequest(
    const FindBmsDownloadAttempt &attempt,
    const std::filesystem::path &downloadRoot) {
  return {.attempt = attempt,
          .downloadRoot = downloadRoot,
          .archiveName = "package.zip",
          .storageKey = "package",
          .archiveKey = std::string(64, 'a'),
          .options = {.skipUnarchivingForNonSolidArchives = true}};
}

void testWorkflowKeepsDirectArchiveWithoutExtraction() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto downloadRoot = testDownloadRoot(*attempt);
  cleanup.add(downloadRoot.parent_path());
  writeText(attempt->archivePath, "archive");
  bool extracted = false;
  std::optional<BmsSearchPendingArtifactKind> committedKind;
  asobmshow::bms_search::DownloadedArchiveWorkflowDependencies dependencies{
      .decideArchive =
          [](const std::filesystem::path &, const std::string &, bool,
             archive_file::PauseCallback) {
            return asobmshow::bms_search::DirectArchiveDecision{
                .disposition = asobmshow::bms_search::
                    DirectArchiveDisposition::KeepArchive,
                .foundBmsFile = true,
                .message = "Downloaded BMS archive."};
          },
      .extractArchive =
          [&extracted](const std::filesystem::path &,
                       const std::filesystem::path &, std::string &,
                       BmsSearchDownloadProgressCallback) {
            extracted = true;
            return false;
          },
      .decideExtracted = {},
      .commitArtifact =
          [&committedKind](const BmsSearchPendingArtifact &artifact,
                           std::string &) {
            committedKind = artifact.kind;
            return true;
          }};
  std::atomic_bool cancelled = false;
  std::vector<std::string> progress;
  BmsSearchResult result;
  assert(asobmshow::bms_search::processDownloadedArchive(
      workflowRequest(*attempt, downloadRoot), cancelled,
      [&progress](const BmsSearchDownloadProgress &update) {
        progress.push_back(update.message);
      },
      result, dependencies));
  assert(!extracted);
  assert(committedKind == BmsSearchPendingArtifactKind::Archive);
  assert(result.status == BmsSearchResult::Status::Downloaded);
  assert(result.outputPath == downloadRoot / "_archives/package.zip");
  assert(result.message == "Downloaded BMS archive.");
  assert(std::find(progress.begin(), progress.end(),
                   "Inspecting downloaded archive") != progress.end());
  assert(std::find(progress.begin(), progress.end(),
                   "Validating archive contents") != progress.end());
  assert(std::find(progress.begin(), progress.end(),
                   "Saving downloaded archive") != progress.end());
}

void testWorkflowStagesDirectArchiveMismatch() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto downloadRoot = testDownloadRoot(*attempt);
  cleanup.add(downloadRoot.parent_path());
  writeText(attempt->archivePath, "archive");
  bool committed = false;
  asobmshow::bms_search::DownloadedArchiveWorkflowDependencies dependencies{
      .decideArchive =
          [](const std::filesystem::path &, const std::string &, bool,
             archive_file::PauseCallback) {
            return asobmshow::bms_search::DirectArchiveDecision{
                .disposition = asobmshow::bms_search::
                    DirectArchiveDisposition::HashMismatch,
                .foundBmsFile = true};
          },
      .extractArchive = {},
      .decideExtracted = {},
      .commitArtifact =
          [&committed](const BmsSearchPendingArtifact &, std::string &) {
            committed = true;
            return true;
          }};
  std::atomic_bool cancelled = false;
  BmsSearchResult result;
  assert(asobmshow::bms_search::processDownloadedArchive(
      workflowRequest(*attempt, downloadRoot), cancelled, nullptr, result,
      dependencies));
  assert(!committed);
  assert(result.status == BmsSearchResult::Status::HashMismatch);
  assert(result.outputPath.empty());
  assert(result.pendingArtifact);
  assert(result.pendingArtifact->kind == BmsSearchPendingArtifactKind::Archive);
  assert(result.pendingArtifact->alternateDestinationPath ==
         downloadRoot / "package");
  assert(result.message.find("Keep Files or Delete Files") !=
         std::string::npos);
}

void testWorkflowCommitsFallbackExtractionMatch() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto downloadRoot = testDownloadRoot(*attempt);
  cleanup.add(downloadRoot.parent_path());
  writeText(attempt->archivePath, "archive");
  std::optional<BmsSearchPendingArtifactKind> committedKind;
  asobmshow::bms_search::DownloadedArchiveWorkflowDependencies dependencies{
      .decideArchive =
          [](const std::filesystem::path &, const std::string &, bool,
             archive_file::PauseCallback) {
            return asobmshow::bms_search::DirectArchiveDecision{};
          },
      .extractArchive =
          [](const std::filesystem::path &,
             const std::filesystem::path &destination, std::string &,
             BmsSearchDownloadProgressCallback) {
            writeText(destination / "chart.bms", "chart");
            return true;
          },
      .decideExtracted =
          [](const std::filesystem::path &, const std::string &) {
            return asobmshow::bms_search::ExtractedArchiveDecision{
                .disposition = asobmshow::bms_search::
                    ExtractedArchiveDisposition::Match,
                .foundBmsFile = true};
          },
      .commitArtifact =
          [&committedKind](const BmsSearchPendingArtifact &artifact,
                           std::string &) {
            committedKind = artifact.kind;
            return true;
          }};
  std::atomic_bool cancelled = false;
  std::vector<std::string> progress;
  BmsSearchResult result;
  assert(asobmshow::bms_search::processDownloadedArchive(
      workflowRequest(*attempt, downloadRoot), cancelled,
      [&progress](const BmsSearchDownloadProgress &update) {
        progress.push_back(update.message);
      },
      result, dependencies));
  assert(committedKind == BmsSearchPendingArtifactKind::ExtractedDirectory);
  assert(result.status == BmsSearchResult::Status::Downloaded);
  assert(result.outputPath == downloadRoot / "package");
  assert(result.message == "Downloaded and unarchived BMS archive.");
  assert(std::find(progress.begin(), progress.end(), "Unarchiving archive") !=
         progress.end());
}

void testWorkflowStagesFallbackExtractionMismatch() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto downloadRoot = testDownloadRoot(*attempt);
  cleanup.add(downloadRoot.parent_path());
  writeText(attempt->archivePath, "archive");
  bool committed = false;
  asobmshow::bms_search::DownloadedArchiveWorkflowDependencies dependencies{
      .decideArchive =
          [](const std::filesystem::path &, const std::string &, bool,
             archive_file::PauseCallback) {
            return asobmshow::bms_search::DirectArchiveDecision{};
          },
      .extractArchive =
          [](const std::filesystem::path &,
             const std::filesystem::path &destination, std::string &,
             BmsSearchDownloadProgressCallback) {
            writeText(destination / "wrong.bms", "wrong");
            return true;
          },
      .decideExtracted =
          [](const std::filesystem::path &, const std::string &) {
            return asobmshow::bms_search::ExtractedArchiveDecision{
                .disposition = asobmshow::bms_search::
                    ExtractedArchiveDisposition::HashMismatch,
                .foundBmsFile = true};
          },
      .commitArtifact =
          [&committed](const BmsSearchPendingArtifact &, std::string &) {
            committed = true;
            return true;
          }};
  std::atomic_bool cancelled = false;
  BmsSearchResult result;
  assert(asobmshow::bms_search::processDownloadedArchive(
      workflowRequest(*attempt, downloadRoot), cancelled, nullptr, result,
      dependencies));
  assert(!committed);
  assert(result.status == BmsSearchResult::Status::HashMismatch);
  assert(result.pendingArtifact);
  assert(result.pendingArtifact->kind ==
         BmsSearchPendingArtifactKind::ExtractedDirectory);
  assert(result.pendingArtifact->sourcePath == attempt->extractedPath);
  assert(result.pendingArtifact->alternateDestinationPath ==
         downloadRoot / "_archives/package.zip");
  assert(!std::filesystem::exists(attempt->archivePath));
}

void testWorkflowKeepsMismatchDecisionWhenArchiveCleanupFails() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto downloadRoot = testDownloadRoot(*attempt);
  cleanup.add(downloadRoot.parent_path());
  writeText(attempt->archivePath / "locked", "archive");
  asobmshow::bms_search::DownloadedArchiveWorkflowDependencies dependencies{
      .decideArchive =
          [](const std::filesystem::path &, const std::string &, bool,
             archive_file::PauseCallback) {
            return asobmshow::bms_search::DirectArchiveDecision{};
          },
      .extractArchive =
          [](const std::filesystem::path &,
             const std::filesystem::path &destination, std::string &,
             BmsSearchDownloadProgressCallback) {
            writeText(destination / "wrong.bms", "wrong");
            return true;
          },
      .decideExtracted =
          [](const std::filesystem::path &, const std::string &) {
            return asobmshow::bms_search::ExtractedArchiveDecision{
                .disposition = asobmshow::bms_search::
                    ExtractedArchiveDisposition::HashMismatch,
                .foundBmsFile = true};
          },
      .commitArtifact =
          [](const BmsSearchPendingArtifact &, std::string &) { return true; }};
  std::atomic_bool cancelled = false;
  BmsSearchResult result;
  assert(asobmshow::bms_search::processDownloadedArchive(
      workflowRequest(*attempt, downloadRoot), cancelled, nullptr, result,
      dependencies));
  assert(result.status == BmsSearchResult::Status::HashMismatch);
  assert(result.pendingArtifact);
  assert(result.pendingArtifact->kind ==
         BmsSearchPendingArtifactKind::ExtractedDirectory);
  assert(std::filesystem::exists(attempt->archivePath));
}

void testWorkflowRejectsInconclusiveExtractedValidation() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto downloadRoot = testDownloadRoot(*attempt);
  cleanup.add(downloadRoot.parent_path());
  writeText(attempt->archivePath, "archive");
  bool committed = false;
  asobmshow::bms_search::DownloadedArchiveWorkflowDependencies dependencies{
      .decideArchive =
          [](const std::filesystem::path &, const std::string &, bool,
             archive_file::PauseCallback) {
            return asobmshow::bms_search::DirectArchiveDecision{};
          },
      .extractArchive =
          [](const std::filesystem::path &,
             const std::filesystem::path &destination, std::string &,
             BmsSearchDownloadProgressCallback) {
            writeText(destination / "chart.bms", "chart");
            return true;
          },
      .decideExtracted =
          [](const std::filesystem::path &, const std::string &) {
            return asobmshow::bms_search::ExtractedArchiveDecision{
                .disposition = asobmshow::bms_search::
                    ExtractedArchiveDisposition::Inconclusive,
                .foundBmsFile = true,
                .message = "Could not read extracted chart."};
          },
      .commitArtifact =
          [&committed](const BmsSearchPendingArtifact &, std::string &) {
            committed = true;
            return true;
          }};
  std::atomic_bool cancelled = false;
  BmsSearchResult result;
  assert(!asobmshow::bms_search::processDownloadedArchive(
      workflowRequest(*attempt, downloadRoot), cancelled, nullptr, result,
      dependencies));
  assert(!committed);
  assert(result.status == BmsSearchResult::Status::DownloadFailed);
  assert(!result.pendingArtifact);
  assert(result.message == "Could not read extracted chart.");
}

void testExtractedDecisionMatchesSha256AndMd5() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const std::string chart = "#TITLE EXTRACTED\n#00111:01\n";
  const std::vector<unsigned char> bytes(chart.begin(), chart.end());
  writeText(attempt->extractedPath / "chart.bms", chart);

  const auto sha = asobmshow::bms_search::decideExtractedArchive(
      attempt->extractedPath, bms_parser::sha256(bytes));
  assert(sha.disposition ==
         asobmshow::bms_search::ExtractedArchiveDisposition::Match);
  assert(sha.foundBmsFile);
  const auto md5 = asobmshow::bms_search::decideExtractedArchive(
      attempt->extractedPath, bms_parser::md5(chart));
  assert(md5.disposition ==
         asobmshow::bms_search::ExtractedArchiveDisposition::Match);
}

void testExtractedDecisionDistinguishesMismatchAndInconclusive() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = asobmshow::bms_search::createFindBmsDownloadAttempt(
      "package.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  writeText(attempt->extractedPath / "chart.bms", "chart");

  const auto mismatch = asobmshow::bms_search::decideExtractedArchive(
      attempt->extractedPath, std::string(64, '0'));
  assert(mismatch.disposition ==
         asobmshow::bms_search::ExtractedArchiveDisposition::HashMismatch);
  const auto noHash = asobmshow::bms_search::decideExtractedArchive(
      attempt->extractedPath, "not-a-hash");
  assert(noHash.disposition ==
         asobmshow::bms_search::ExtractedArchiveDisposition::Match);
  assert(noHash.foundBmsFile);
  const auto missing = asobmshow::bms_search::decideExtractedArchive(
      attempt->root / "missing", std::string(64, '0'));
  assert(missing.disposition ==
         asobmshow::bms_search::ExtractedArchiveDisposition::Inconclusive);
}

void testPublicDownloadApiAcceptsOptions() {
  using FindMethod = BmsSearchResult (BmsSearchService::*)(
      const std::string &, const std::string &, const std::filesystem::path &,
      std::atomic_bool &, BmsSearchDownloadProgressCallback,
      const std::string &, const std::string &, BmsSearchDownloadOptions) const;
  using CandidateMethod = BmsSearchResult (BmsSearchService::*)(
      const BmsSearchCandidate &, const std::string &, const std::string &,
      const std::filesystem::path &, std::atomic_bool &,
      BmsSearchDownloadProgressCallback, BmsSearchDownloadOptions) const;
  static_assert(
      std::is_same_v<decltype(&BmsSearchService::findAndDownload), FindMethod>);
  static_assert(std::is_same_v<decltype(&BmsSearchService::downloadCandidate),
                               CandidateMethod>);
}

void testPendingMismatchCannotDismiss() {
  BmsSearchResult result;
  result.status = BmsSearchResult::Status::HashMismatch;
  result.pendingArtifact = BmsSearchPendingArtifact{};
  const auto pending = findBmsDialogPolicy(false, result);
  assert(!pending.canDismiss);
  assert(!pending.showCloseOrCancel);
  assert(pending.showPendingActions);
  assert(!pending.showNormalResultActions);

  const auto resolving = findBmsDialogPolicy(true, result);
  assert(!resolving.canDismiss);
  assert(!resolving.showCloseOrCancel);
  assert(!resolving.showPendingActions);

  result.pendingArtifact.reset();
  const auto resolved = findBmsDialogPolicy(false, result);
  assert(resolved.canDismiss);
  assert(resolved.showCloseOrCancel);
  assert(!resolved.showPendingActions);
}

void testDownloadFailureDetailPreservesCause() {
  BmsSearchResult result;
  result.status = BmsSearchResult::Status::DownloadFailed;
  result.message = "Could not install downloaded files: permission denied.";
  assert(findBmsDownloadFailureDetail(result) == result.message);

  result.message.clear();
  assert(findBmsDownloadFailureDetail(result) ==
         "Open the source or try again.");
}

void testFindBmsDownloadProgressDisplaysSizes() {
  assert(findBmsProgressDisplayText("Downloading archive", 19503513,
                                    46451917, true) ==
         "Downloading archive - 42% (18.6 MB / 44.3 MB)");
  assert(findBmsProgressDisplayText("Downloading archive", 12345678, 0,
                                    true) ==
         "Downloading archive (11.8 MB)");
  assert(findBmsProgressDisplayText("Extracting archive", 12345678,
                                    46451917, true) ==
         "Extracting archive");
}

} // namespace

int main() {
  testStorageNamesDistinguishSameNamedPackages();
  testStorageNamesPreserveLongAndCompoundExtensions();
  testExtractedCommitMergesTransactionally();
  testDeleteRemovesOnlyAttempt();
  testUnsafeStagingRootIsRefused();
  testCommitRestoresDestinationWhenSwapFails();
  testArchiveCommitAndResolution();
  testArchiveCommitRemovesExtractedAlternate();
  testExtractedCommitRemovesArchiveAlternate();
  testExtractedCommitRemovesSameKeyArchiveWithDifferentExtension();
#if !defined(_WIN32)
  testAlternateCleanupFailureDoesNotFailCommit();
#endif
  testFailedResolutionRetainsPendingArtifact();
  testAlreadyMissingAttemptCanBeDeleted();
  testUnsafeArchiveNameAndDownloadRootAreRefused();
  testUnsafeAlternateDestinationIsRefused();
  testDirectArchiveSha256MatchStaysPacked();
  testDirectArchiveDisabledDoesNotInspect();
  testDirectArchiveMismatchIsConfirmed();
  testDirectArchiveIncompleteReadFallsBack();
  testSolidEmptyAndFailedListingsFallBack();
  testDirectArchiveMd5AndNoHashBehavior();
  testDirectArchiveNoHashUnreadableBmsFallsBack();
  testWorkflowKeepsDirectArchiveWithoutExtraction();
  testWorkflowStagesDirectArchiveMismatch();
  testWorkflowCommitsFallbackExtractionMatch();
  testWorkflowStagesFallbackExtractionMismatch();
  testWorkflowKeepsMismatchDecisionWhenArchiveCleanupFails();
  testWorkflowRejectsInconclusiveExtractedValidation();
  testExtractedDecisionMatchesSha256AndMd5();
  testExtractedDecisionDistinguishesMismatchAndInconclusive();
  testPublicDownloadApiAcceptsOptions();
  testPendingMismatchCannotDismiss();
  testDownloadFailureDetailPreservesCause();
  testFindBmsDownloadProgressDisplaysSizes();
  return 0;
}
