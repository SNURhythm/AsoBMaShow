#pragma once

#if ASOBMSHOW_HAS_LIBARCHIVE
#include "ArchiveRAII.h"
#endif

namespace {

using namespace asobmshow::bms_search;

class VerificationAllocationGuard {
public:
  VerificationAllocationGuard() {
    verification_allocation_guard::rejected = 0;
    verification_allocation_guard::enabled = true;
  }
  ~VerificationAllocationGuard() {
    verification_allocation_guard::enabled = false;
    assert(verification_allocation_guard::rejected == 0);
  }
};

DownloadedArchiveWorkflowDependencies realVerificationWorkflow(
    ArchiveVerificationLimits limits = {}) {
  return {
      .decideArchive = [limits](const std::filesystem::path &path,
                                const std::string &key, bool packed,
                                archive_file::PauseCallback checkpoint) {
        return decideDownloadedArchive(path, key, packed, std::move(checkpoint),
                                        defaultArchiveReaderDependencies(), limits);
      },
      .extractArchive = [](const std::filesystem::path &path,
                            const std::filesystem::path &output, std::string &error,
                            BmsSearchDownloadProgressCallback progress,
                            ArchiveExtractionCancelled cancelled) {
        return extractDownloadedArchive(path, output, error, std::move(progress),
                                         std::move(cancelled));
      },
      .decideExtracted = [limits](const std::filesystem::path &path,
                                  const std::string &key,
                                  archive_file::PauseCallback checkpoint,
                                  ArchiveVerificationLimits remaining) {
        remaining.maxMemberBytes = std::min(remaining.maxMemberBytes, limits.maxMemberBytes);
        remaining.maxTotalBytes = std::min(remaining.maxTotalBytes, limits.maxTotalBytes);
        remaining.maxEntries = std::min(remaining.maxEntries, limits.maxEntries);
        return decideExtractedArchive(path, key, std::move(checkpoint), remaining);
      },
      .commitArtifact = [](const BmsSearchPendingArtifact &artifact,
                            std::string &error,
                            std::vector<std::filesystem::path> &removed) {
        return commitFindBmsPendingArtifact(artifact, error, {}, &removed);
      }};
}

void assertVerificationFailure(const BmsSearchResult &result,
                                const DownloadedArchiveWorkflowRequest &request) {
  assert(result.status == BmsSearchResult::Status::DownloadFailed);
  assert(!result.pendingArtifact);
  assert(result.outputPath.empty());
  assert(result.removedPaths.empty());
  assert(!std::filesystem::exists(request.downloadRoot / request.storageKey));
  assert(!std::filesystem::exists(request.downloadRoot / "_archives" / request.archiveName));
}

void testRealVerificationWorkflowLimitsAndControls() {
  const std::string chart = "abc";
  const std::string md5 = "900150983cd24fb0d6963f7d28e17f72";
  const std::string sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  for (const bool packed : {true, false}) {
    for (const std::string key : {md5, sha256, std::string(), std::string(32, '0')}) {
      CleanupPaths cleanup;
      std::string error;
      const auto attempt = createFindBmsDownloadAttempt("controls.zip", error);
      assert(attempt);
      cleanup.add(attempt->root);
      const auto library = testDownloadRoot(*attempt);
      cleanup.add(library.parent_path());
      writeExtractionZip(attempt->archivePath,
          {{"chart.bms", chart}, {"empty.bms", ""},
           {"__MACOSX/._chart.bms", "metadata"}});
      DownloadedArchiveWorkflowRequest request{
          .attempt = *attempt, .downloadRoot = library, .archiveName = "controls.zip",
          .storageKey = "controls", .archiveKey = key,
          .options = {.skipUnarchivingForNonSolidArchives = packed}};
      std::atomic_bool cancelled = false;
      BmsSearchResult result;
      VerificationAllocationGuard guard;
      assert(processDownloadedArchive(request, cancelled, {}, result, realVerificationWorkflow()));
      if (key == std::string(32, '0')) {
        assert(result.status == BmsSearchResult::Status::HashMismatch);
        assert(result.pendingArtifact);
        assert(result.outputPath.empty());
      } else {
        assert(result.status == BmsSearchResult::Status::Downloaded);
        assert(!result.pendingArtifact);
        assert(std::filesystem::exists(result.outputPath));
        if (!packed) assert(readText(result.outputPath / "chart.bms") == chart);
      }
    }
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
      CleanupPaths cleanup;
      std::string error;
      const auto attempt = createFindBmsDownloadAttempt("limits.zip", error);
      assert(attempt);
      cleanup.add(attempt->root);
      const auto library = testDownloadRoot(*attempt);
      cleanup.add(library.parent_path());
      ArchiveVerificationLimits limits{.maxMemberBytes = 128 * 1024,
                                        .maxTotalBytes = 192 * 1024, .maxEntries = 4};
      std::vector<ExtractionZipMember> members;
      if (scenario == 0) members = {{"chart.bms", chart, 17U * 1024 * 1024}};
      if (scenario == 1) members = {{"chart.bms", std::string(256 * 1024, 'a')}};
      if (scenario == 2) members = {{"chart.bms", std::string(256 * 1024, 'a'), 1}};
      if (scenario == 3) members = {{"one.bms", std::string(128 * 1024, 'a')},
                                    {"two.bms", std::string(128 * 1024, 'b')}};
      if (scenario == 4) {
        limits.maxEntries = 2;
        members = {{"folder/", ""}, {"sound.wav", ""}, {"chart.bms", chart}};
      }
      if (scenario == 5) {
        limits.maxEntries = 2;
        members = {{"chart.bms", chart}, {"../skipped", ""}, {"../also-skipped", ""}};
      }
      writeExtractionZip(attempt->archivePath, members);
      assert(std::filesystem::file_size(attempt->archivePath) < 16384);
      DownloadedArchiveWorkflowRequest request{
          .attempt = *attempt, .downloadRoot = library, .archiveName = "limits.zip",
          .storageKey = "limits", .archiveKey = sha256,
          .options = {.skipUnarchivingForNonSolidArchives = packed}};
      std::atomic_bool cancelled = false;
      BmsSearchResult result;
      auto dependencies = realVerificationWorkflow(limits);
      if (!packed && scenario == 5) {
        dependencies.extractArchive = [limits](const std::filesystem::path &path,
                                                const std::filesystem::path &output,
                                                std::string &failure,
                                                BmsSearchDownloadProgressCallback progress,
                                                ArchiveExtractionCancelled stop) {
          ArchiveExtractionLimits extractionLimits;
          extractionLimits.maxEntries = limits.maxEntries;
          return extractDownloadedArchive(path, output, failure, std::move(progress),
                                           std::move(stop), extractionLimits);
        };
      }
      VerificationAllocationGuard guard;
      assert(!processDownloadedArchive(request, cancelled, {}, result, dependencies));
      assertVerificationFailure(result, request);
    }
  }
}

void testRealVerificationCancellation() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = createFindBmsDownloadAttempt("cancel.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto library = testDownloadRoot(*attempt);
  cleanup.add(library.parent_path());
  const std::string contents(256 * 1024, 'a');
  writeExtractionZip(attempt->archivePath, {{"chart.bms", contents}});
  writeText(attempt->extractedPath / "chart.bms", contents);
  for (const std::string key : {std::string(32, '0'), std::string(64, '0')}) {
    for (const bool packed : {true, false}) {
      std::size_t checkpoints = 0;
      auto decide = [&](archive_file::PauseCallback checkpoint) {
        if (packed) {
          return decideDownloadedArchive(attempt->archivePath, key, true,
                   std::move(checkpoint), defaultArchiveReaderDependencies()).disposition ==
                 DirectArchiveDisposition::HashMismatch;
        }
        return decideExtractedArchive(attempt->extractedPath, key, std::move(checkpoint)).disposition ==
               ExtractedArchiveDisposition::HashMismatch;
      };
      assert(decide({}));
      assert(decide([&] { ++checkpoints; return true; }));
      assert(checkpoints > 10);
      for (std::size_t cancelAt = 1; cancelAt <= checkpoints; ++cancelAt) {
        std::size_t observed = 0;
        std::atomic_bool cancelled = false;
        DownloadedArchiveWorkflowRequest request{
            .attempt = *attempt, .downloadRoot = library, .archiveName = "cancel.zip",
            .storageKey = "cancel", .archiveKey = key,
            .options = {.skipUnarchivingForNonSolidArchives = packed}};
        auto dependencies = realVerificationWorkflow();
        const auto checkCancellation = [&] {
          if (++observed >= cancelAt) cancelled.store(true);
          return !cancelled.load();
        };
        if (packed) {
          dependencies.decideArchive = [&](const std::filesystem::path &path,
                                            const std::string &hash, bool skip,
                                            archive_file::PauseCallback checkpoint) {
            return decideDownloadedArchive(path, hash, skip,
                [&] { return checkCancellation() && checkpoint(); }, defaultArchiveReaderDependencies());
          };
        } else {
          dependencies.decideExtracted = [&](const std::filesystem::path &path,
                                              const std::string &hash,
                                              archive_file::PauseCallback checkpoint,
                                              ArchiveVerificationLimits limits) {
            return decideExtractedArchive(path, hash,
                [&] { return checkCancellation() && checkpoint(); }, limits);
          };
        }
        BmsSearchResult result;
        VerificationAllocationGuard guard;
        const bool completed = processDownloadedArchive(request, cancelled, {}, result, dependencies);
        if (completed) {
          std::cerr << "Cancellation missed: packed=" << packed << " at=" << cancelAt
                    << " observed=" << observed << " calibrated=" << checkpoints << '\n';
        }
        assert(!completed);
        assert(observed == cancelAt);
        assertVerificationFailure(result, request);
      }
    }
  }
  for (const bool packed : {true, false}) {
    DownloadedArchiveWorkflowRequest request{
        .attempt = *attempt, .downloadRoot = library, .archiveName = "cancel.zip",
        .storageKey = "cancel", .archiveKey = "",
        .options = {.skipUnarchivingForNonSolidArchives = packed}};
    std::atomic_bool cancelled = false;
    BmsSearchResult result;
    auto dependencies = realVerificationWorkflow();
    if (!packed) {
      const auto realDecision = dependencies.decideExtracted;
      dependencies.decideExtracted = [&, realDecision](const std::filesystem::path &path,
                                          const std::string &key,
                                          archive_file::PauseCallback checkpoint,
                                          ArchiveVerificationLimits limits) {
        const auto decision = realDecision(path, key, checkpoint, limits);
        assert(decision.disposition == ExtractedArchiveDisposition::Match);
        cancelled.store(true);
        return decision;
      };
    }
    assert(!processDownloadedArchive(request, cancelled,
        [&](const BmsSearchDownloadProgress &progress) {
          if (packed && progress.message == "Saving downloaded archive") cancelled.store(true);
        }, result, dependencies));
    assert(cancelled.load());
    assertVerificationFailure(result, request);
  }
}

void testExtractedVerificationActualGrowth() {
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = createFindBmsDownloadAttempt("growth.zip", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const auto path = attempt->extractedPath / "chart.bms";
  const std::string grown(192 * 1024, 'a');
  for (const bool aggregate : {false, true}) {
    writeText(path, "a");
    unsigned checkpoints = 0;
    VerificationAllocationGuard guard;
    const auto decision = decideExtractedArchive(attempt->extractedPath, "", [&] {
      if (++checkpoints == 3) writeText(path, grown);
      return true;
    }, {.maxMemberBytes = aggregate ? 256U * 1024 : 128U * 1024,
        .maxTotalBytes = aggregate ? 128U * 1024 : 256U * 1024,
        .maxEntries = 2});
    assert(checkpoints > 3);
    assert(decision.disposition == ExtractedArchiveDisposition::Inconclusive);
    assert(decision.message.find("limit") != std::string::npos);
  }
}

void testOwnedAttemptVerificationCleanup() {
  CleanupPaths cleanup;
  std::string error;
  const auto fixture = createFindBmsDownloadAttempt("owned-verification.zip", error);
  assert(fixture);
  cleanup.add(fixture->root);
  const auto library = fixture->root / "library";
  writeText(library / "user-owned.txt", "preserve");
  extractionDownloadFixture = fixture->archivePath;
  writeExtractionZip(fixture->archivePath, {{"chart.bms", std::string(17U * 1024 * 1024, 'a')}});
  for (const bool packed : {true, false}) {
    std::atomic_bool cancelled = false;
    BmsSearchResult result;
    VerificationAllocationGuard guard;
    assert(!downloadAndExtractArchive("https://fixture.invalid/song.zip", "", "",
        library, cancelled, {}, {.skipUnarchivingForNonSolidArchives = packed}, result));
    assert(result.status == BmsSearchResult::Status::DownloadFailed);
    assert(result.message.find("limit") != std::string::npos);
    assert(!result.pendingArtifact);
    assert(result.outputPath.empty());
    assert(result.removedPaths.empty());
    assert(!std::filesystem::exists(observedDownloadAttempt));
    assert(readText(library / "user-owned.txt") == "preserve");
    for (const auto &entry : std::filesystem::recursive_directory_iterator(library)) {
      assert(entry.is_directory() || entry.path().filename() == "user-owned.txt");
    }
  }
}

#if ASOBMSHOW_HAS_LIBARCHIVE
void testRealVerificationCodecFallback() {
  for (const bool sevenZip : {false, true}) {
    CleanupPaths cleanup;
    std::string error;
    const auto name = sevenZip ? "solid.7z" : "bzip.zip";
    const auto attempt = createFindBmsDownloadAttempt(name, error);
    assert(attempt);
    cleanup.add(attempt->root);
    const auto library = testDownloadRoot(*attempt);
    cleanup.add(library.parent_path());
    auto writer = makeArchiveWriteHandle();
    assert(writer);
    if (sevenZip) {
      assert(archive_write_set_format_7zip(writer.get()) == ARCHIVE_OK);
    } else {
      assert(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK);
      assert(archive_write_set_format_option(writer.get(), "zip", "compression", "bzip2") == ARCHIVE_OK);
    }
    assert(archive_write_open_filename(writer.get(), attempt->archivePath.string().c_str()) == ARCHIVE_OK);
    for (const auto member : {"one.bms", "two.bms"}) {
      std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(
          archive_entry_new(), archive_entry_free);
      archive_entry_set_pathname(entry.get(), member);
      archive_entry_set_filetype(entry.get(), AE_IFREG);
      archive_entry_set_perm(entry.get(), 0644);
      archive_entry_set_size(entry.get(), 3);
      assert(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
      assert(archive_write_data(writer.get(), "abc", 3) == 3);
      assert(archive_write_finish_entry(writer.get()) == ARCHIVE_OK);
    }
    assert(archive_write_close(writer.get()) == ARCHIVE_OK);
    writer.reset();
    const auto direct = decideDownloadedArchive(attempt->archivePath, "", true, {}, defaultArchiveReaderDependencies());
    if (sevenZip) assert(direct.disposition == DirectArchiveDisposition::Unarchive);
    else assert(direct.disposition == DirectArchiveDisposition::KeepArchive);
    DownloadedArchiveWorkflowRequest request{
        .attempt = *attempt, .downloadRoot = library, .archiveName = name,
        .storageKey = "codec", .archiveKey = "900150983cd24fb0d6963f7d28e17f72",
        .options = {.skipUnarchivingForNonSolidArchives = true}};
    std::atomic_bool cancelled = false;
    BmsSearchResult result;
    assert(processDownloadedArchive(request, cancelled, {}, result, realVerificationWorkflow()));
    assert(result.status == BmsSearchResult::Status::Downloaded);
    assert(!result.pendingArtifact);
    assert(std::filesystem::exists(result.outputPath));
    if (sevenZip) assert(readText(result.outputPath / "one.bms") == "abc");
  }
}
#endif

int testRealVerification() {
  testRealVerificationWorkflowLimitsAndControls();
  testRealVerificationCancellation();
  testExtractedVerificationActualGrowth();
  testOwnedAttemptVerificationCleanup();
#if ASOBMSHOW_HAS_LIBARCHIVE
  testRealVerificationCodecFallback();
#endif
  return 0;
}

}
