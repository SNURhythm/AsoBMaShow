#include "bms_search/DownloadStaging.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using asobmshow::bms_search::FindBmsDownloadAttempt;

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

} // namespace

int main() {
  testExtractedCommitMergesTransactionally();
  testDeleteRemovesOnlyAttempt();
  testUnsafeStagingRootIsRefused();
  testCommitRestoresDestinationWhenSwapFails();
  testArchiveCommitAndResolution();
  testFailedResolutionRetainsPendingArtifact();
  testAlreadyMissingAttemptCanBeDeleted();
  testUnsafeArchiveNameAndDownloadRootAreRefused();
  return 0;
}
