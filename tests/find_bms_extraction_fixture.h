#pragma once

#include "bms_search/ArchiveExtraction.h"
#include "bms_search/GoogleDriveDriver.h"
#include "downloads/DownloadJobs.h"
#include "downloads/DownloadImport.h"
#include "RAII.h"
#include "../bgfx/bimg/3rdparty/tinyexr/deps/miniz/miniz.h"

#include <iostream>
#include <limits>

namespace asobmshow::bms_search {

std::filesystem::path extractionDownloadFixture;
std::filesystem::path observedDownloadAttempt;
std::function<bool(const std::string &, std::string &)> downloadFixtureFailure;
long downloadFixtureHttpStatus = 0;

bool downloadUrlToFile(const std::string &url, const std::filesystem::path &path,
                       std::atomic_bool &, std::string &error,
                       BmsSearchDownloadProgressCallback,
                       BmsSearchDownloadRetryCallback, long *httpStatus) {
  observedDownloadAttempt = path.parent_path();
  const bool failed = downloadFixtureFailure && downloadFixtureFailure(url, error);
  if (httpStatus != nullptr) *httpStatus = downloadFixtureHttpStatus;
  if (failed) return false;
  std::filesystem::copy_file(extractionDownloadFixture, path);
  return true;
}

#include "find_bms_download_attempt_methods.inc"

}

namespace {

void testGoogleDriveContinuationStaysOnSelectedFile() {
  using namespace asobmshow::bms_search;
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = createFindBmsDownloadAttempt("confirmation.html", error);
  assert(attempt); cleanup.add(attempt->root);
  const std::string original = "https://drive.google.com/uc?id=fixture-id&export=download";
  const auto write = [&](const std::string &host, const std::string &id) {
    std::ofstream(attempt->archivePath) << "<html><form action=\"https://" << host
        << "/download\"><input name=\"id\" value=\"" << id
        << "\"><input name=\"confirm\" value=\"yes\"></form></html>";
  };
  write("drive.usercontent.google.com", "fixture-id");
  const auto next = GoogleDriveDriver::warningContinuation(original, original, attempt->archivePath);
  assert(next && next->starts_with("https://drive.usercontent.google.com/download?") && next->find("id=fixture-id") != next->npos);
  write("untrusted.invalid", "fixture-id");
  assert(!GoogleDriveDriver::warningContinuation(original, original, attempt->archivePath));
  write("drive.usercontent.google.com", "another-file");
  assert(!GoogleDriveDriver::warningContinuation(original, original, attempt->archivePath));
  write("drive.usercontent.google.com", "fixture-id");
  assert(!GoogleDriveDriver::warningContinuation("https://untrusted.invalid/file", original, attempt->archivePath));
  std::filesystem::resize_file(attempt->archivePath, 64ull * 1024 * 1024 * 1024);
  assert(!GoogleDriveDriver::warningContinuation(original, original, attempt->archivePath));
}

void testResolvedDownloadHandoffDoesNotStartSceneOwnedTransfer() {
  using namespace asobmshow::bms_search;
  observedDownloadAttempt.clear();
  std::optional<downloads::Request> submitted;
  BmsSearchDownloadOptions options;
  options.skipUnarchivingForNonSolidArchives = true;
  options.storagePolicy.bga = storage_management::BgaAction::Delete;
  options.enqueueDownload = [&](const downloads::Request &request) {
    submitted = request;
    return "durable-job";
  };
  std::atomic_bool cancelled = false;
  BmsSearchResult result;
  assert(downloadAndExtractArchive("https://fixture.invalid/package.zip", "", std::string(64, 'a'),
      std::filesystem::temp_directory_path() / "library", cancelled, {}, options, result, "song.zip", "source-id"));
  assert(submitted);
  assert(submitted->expectedHash == std::string(64, 'a'));
  assert(submitted->skipUnarchiving);
  assert(submitted->storagePolicy.bga == storage_management::BgaAction::Delete);
  assert(submitted->storageIdentity == "source-id");
  assert(observedDownloadAttempt.empty());
  assert(result.queuedDownloadId == "durable-job");
  assert(result.outputPath.empty());
}

struct ExtractionZipMember {
  std::string name;
  std::string contents;
  std::optional<std::uint32_t> declaredSize;
};

void writeExtractionZip(const std::filesystem::path &path,
                        const std::vector<ExtractionZipMember> &members) {
  std::string records;
  std::string directory;
  auto appendInteger = [](std::string &bytes, std::uint32_t value,
                          unsigned width) {
    for (unsigned index = 0; index < width; ++index) {
      bytes.push_back(static_cast<char>(value >> (index * 8)));
    }
  };
  for (const auto &member : members) {
    std::string compressed(mz_compressBound(member.contents.size()), '\0');
    mz_ulong compressedSize = compressed.size();
    assert(mz_compress2(reinterpret_cast<unsigned char *>(compressed.data()),
                        &compressedSize,
                        reinterpret_cast<const unsigned char *>(member.contents.data()),
                        member.contents.size(), MZ_BEST_COMPRESSION) == MZ_OK);
    compressed = compressed.substr(2, compressedSize - 6);
    const auto checksum = mz_crc32(0,
        reinterpret_cast<const unsigned char *>(member.contents.data()),
        member.contents.size());
    const auto localOffset = records.size();
    appendInteger(records, 0x04034b50, 4);
    appendInteger(records, 20, 2);
    appendInteger(records, 0, 2);
    appendInteger(records, 8, 2);
    appendInteger(records, 0, 4);
    appendInteger(records, checksum, 4);
    appendInteger(records, compressed.size(), 4);
    appendInteger(records, member.declaredSize.value_or(member.contents.size()), 4);
    appendInteger(records, member.name.size(), 2);
    appendInteger(records, 0, 2);
    records += member.name;
    records += compressed;
    appendInteger(directory, 0x02014b50, 4);
    appendInteger(directory, 20, 2);
    appendInteger(directory, 20, 2);
    appendInteger(directory, 0, 2);
    appendInteger(directory, 8, 2);
    appendInteger(directory, 0, 4);
    appendInteger(directory, checksum, 4);
    appendInteger(directory, compressed.size(), 4);
    appendInteger(directory, member.declaredSize.value_or(member.contents.size()), 4);
    appendInteger(directory, member.name.size(), 2);
    appendInteger(directory, 0, 2);
    appendInteger(directory, 0, 2);
    appendInteger(directory, 0, 2);
    appendInteger(directory, 0, 2);
    appendInteger(directory, 0, 4);
    appendInteger(directory, localOffset, 4);
    directory += member.name;
  }
  const auto directoryOffset = records.size();
  records += directory;
  appendInteger(records, 0x06054b50, 4);
  appendInteger(records, 0, 4);
  appendInteger(records, members.size(), 2);
  appendInteger(records, members.size(), 2);
  appendInteger(records, directory.size(), 4);
  appendInteger(records, directoryOffset, 4);
  appendInteger(records, 0, 2);
  writeText(path, records);
}

void testExtractionLimitsAndCancellation() {
  using namespace asobmshow::bms_search;
  int failures = 0;
  auto check = [&](bool condition, const std::string &message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  };
  for (const auto extractor : {extractZipArchive, extractDownloadedArchive}) {
    CleanupPaths cleanup;
    std::string error;
    const auto attempt = createFindBmsDownloadAttempt("limits.zip", error);
    assert(attempt);
    cleanup.add(attempt->root);
    const auto output = attempt->extractedPath;
    auto reset = [&] {
      std::filesystem::remove_all(output);
      error.clear();
    };
    ArchiveExtractionLimits limits{.maxEntryBytes = 128 * 1024,
                                   .maxTotalBytes = 256 * 1024,
                                   .maxEntries = 10,
                                   .reservedFreeBytes = 0};
    writeExtractionZip(attempt->archivePath, {{"chart.bms", "#TITLE Test\n"},
                                              {"sound.wav", std::string(65536, 'a')}});
    check(extractor(attempt->archivePath, output, error, {}, {}, limits),
          "valid compressed archive extracts");
    check(readText(output / "sound.wav") == std::string(65536, 'a'),
          "extraction preserves member bytes");
    reset();
    limits.maxEntryBytes = 32768;
    check(!extractor(attempt->archivePath, output, error, {}, {}, limits),
          "declared member budget rejects expansion");
    check(!std::filesystem::exists(output / "sound.wav"),
          "oversized declared member is rejected before opening output");
    reset();
    limits.maxEntryBytes = 128 * 1024;
    limits.maxTotalBytes = 65536;
    check(!extractor(attempt->archivePath, output, error, {}, {}, limits),
          "aggregate budget includes previous members");
    reset();
    limits.maxTotalBytes = 256 * 1024;
    writeExtractionZip(attempt->archivePath, {{"sound.wav", std::string(65536, 'a'), 1}});
    for (const bool totalLimit : {false, true}) {
      limits.maxEntryBytes = totalLimit ? 128 * 1024 : 16384;
      limits.maxTotalBytes = totalLimit ? 16384 : 256 * 1024;
      check(!extractor(attempt->archivePath, output, error, {}, {}, limits),
            "understated metadata cannot admit oversized streamed content");
      std::error_code sizeError;
      const auto size = std::filesystem::file_size(output / "sound.wav", sizeError);
      check(sizeError || size <= 16384,
            "streamed budgets reject bytes before writing despite understated metadata");
      reset();
    }
    limits.maxEntryBytes = 128 * 1024;
    limits.maxTotalBytes = 256 * 1024;
    limits.maxEntries = 2;
    writeExtractionZip(attempt->archivePath,
        {{"empty/", ""}, {"../unsafe", ""}, {"chart.bms", "#TITLE Test\n"}});
    check(!extractor(attempt->archivePath, output, error, {}, {}, limits),
          "entry budget counts empty directories and unsafe paths");
    reset();
    limits.maxEntries = 10;
    limits.maxEntryBytes = 4 * 1024 * 1024;
    limits.maxTotalBytes = 4 * 1024 * 1024;
    writeExtractionZip(attempt->archivePath, {{"sound.wav", std::string(2 * 1024 * 1024, 'a')}});
    check(std::filesystem::file_size(attempt->archivePath) < 16384,
          "cancellation fixture is a highly compressed member");
    bool cancellationObserved = false;
    const bool extracted = extractor(attempt->archivePath, output, error, {}, [&] {
      std::error_code sizeError;
      const auto size = std::filesystem::file_size(output / "sound.wav", sizeError);
      cancellationObserved = !sizeError && size >= 65536;
      return cancellationObserved;
    }, limits);
    check(!extracted && cancellationObserved,
          "cancellation reaches decompression within a member");
    check(std::filesystem::file_size(output / "sound.wav") <= 128 * 1024,
          "cancelled member stops after bounded output");
    reset();
    check(!extractor(attempt->archivePath, output, error, {}, [] { return true; }, limits),
          "pre-cancelled extraction fails before output");
    check(!std::filesystem::exists(output / "sound.wav"),
          "pre-cancelled extraction creates no member");
    reset();
    limits.reservedFreeBytes = std::numeric_limits<std::uint64_t>::max();
    check(!extractor(attempt->archivePath, output, error, {}, {}, limits),
          "storage reserve cannot wrap into usable space");
    check(!std::filesystem::exists(output / "sound.wav"),
          "insufficient storage fails before member output");
    reset();
    limits.reservedFreeBytes = 0;
    writeExtractionZip(attempt->archivePath, {{"chart.bms", "#TITLE Test\n"}});
    auto corrupted = readText(attempt->archivePath);
    corrupted[14] ^= 1;
    const auto directoryOffset = corrupted.find(std::string("PK\1\2", 4));
    assert(directoryOffset != std::string::npos);
    corrupted[directoryOffset + 16] ^= 1;
    writeText(attempt->archivePath, corrupted);
    check(!extractor(attempt->archivePath, output, error, {}, {}, limits),
          "streaming extraction still rejects corrupt CRCs");
    reset();
    writeExtractionZip(attempt->archivePath, {{"chart.bms", "#TITLE Test\n"}});
    writeText(output / "chart.bms" / "user-owned.txt", "preserve");
    check(!extractor(attempt->archivePath, output, error, {}, {}, limits),
          "output-open failure rejects extraction");
    check(readText(output / "chart.bms" / "user-owned.txt") == "preserve",
          "backend failure never removes caller-owned output");
  }
  assert(failures == 0);
}

#if ASOBMSHOW_HAS_LIBARCHIVE
void testUnknownSizeStreamIsBounded() {
  using namespace asobmshow::bms_search;
  CleanupPaths cleanup;
  std::string error;
  const auto attempt = createFindBmsDownloadAttempt("stream.gz", error);
  assert(attempt);
  cleanup.add(attempt->root);
  const std::string contents(128 * 1024, 'a');
  std::string compressed(mz_compressBound(contents.size()), '\0');
  mz_ulong compressedSize = compressed.size();
  assert(mz_compress2(reinterpret_cast<unsigned char *>(compressed.data()),
                      &compressedSize,
                      reinterpret_cast<const unsigned char *>(contents.data()),
                      contents.size(), MZ_BEST_COMPRESSION) == MZ_OK);
  std::string gzip("\x1f\x8b\x08\0\0\0\0\0\0\xff", 10);
  gzip += compressed.substr(2, compressedSize - 6);
  const auto checksum = mz_crc32(0,
      reinterpret_cast<const unsigned char *>(contents.data()), contents.size());
  for (const std::uint32_t value : {static_cast<std::uint32_t>(checksum),
                                   static_cast<std::uint32_t>(contents.size())}) {
    for (unsigned index = 0; index < 4; ++index) {
      gzip.push_back(static_cast<char>(value >> (index * 8)));
    }
  }
  writeText(attempt->archivePath, gzip);
  for (const bool totalLimit : {false, true}) {
    ArchiveExtractionLimits limits{.maxEntryBytes = totalLimit ? 262144U : 32768U,
                                   .maxTotalBytes = totalLimit ? 32768U : 262144U,
                                   .maxEntries = 10,
                                   .reservedFreeBytes = 0};
    assert(!extractDownloadedArchive(attempt->archivePath, attempt->extractedPath,
                                      error, {}, {}, limits));
    std::uint64_t written = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(attempt->extractedPath)) {
      if (entry.is_regular_file()) {
        written += entry.file_size();
      }
    }
    assert(written <= 32768);
    assert(error.find("limit") != std::string::npos);
    std::filesystem::remove_all(attempt->extractedPath);
  }
}
#endif

void testDownloadAttemptExtractionCleanup() {
  using namespace asobmshow::bms_search;
  CleanupPaths cleanup;
  std::string error;
  const auto fixture = createFindBmsDownloadAttempt("fixture.zip", error);
  assert(fixture);
  cleanup.add(fixture->root);
  const auto library = fixture->root / "library";
  writeText(library / "user-owned.txt", "preserve");
  extractionDownloadFixture = fixture->archivePath;
  for (const bool cancel : {false, true}) {
    writeExtractionZip(fixture->archivePath,
        {{"previous.wav", std::string(65536, 'b')},
         {"sound.wav", std::string(2 * 1024 * 1024, 'a'),
          cancel ? std::nullopt : std::optional<std::uint32_t>(3U * 1024 * 1024 * 1024)}});
    std::atomic_bool cancelled = false;
    bool observedPartialOutput = false;
    BmsSearchResult result;
    const bool downloaded = downloadAndExtractArchive(
        "https://fixture.invalid/song.zip", "", "", library, cancelled,
        [&](const BmsSearchDownloadProgress &progress) {
          if (cancel && progress.message == "Extracting sound.wav") {
            observedPartialOutput = readText(
                observedDownloadAttempt / "extracted" / "previous.wav") ==
                std::string(65536, 'b');
            cancelled.store(true);
          }
        }, {.skipUnarchivingForNonSolidArchives = false}, result);
    assert(!downloaded);
    assert(result.status == BmsSearchResult::Status::DownloadFailed);
    assert(!result.pendingArtifact);
    assert(!observedDownloadAttempt.empty());
    assert(!std::filesystem::exists(observedDownloadAttempt));
    assert(readText(library / "user-owned.txt") == "preserve");
    if (cancel) {
      assert(observedPartialOutput);
      assert(result.message == "Lookup cancelled.");
    } else {
      assert(result.message.find("extract") != std::string::npos);
    }
  }
}

void testDurableDownloadImportsExistingFileWithoutTransport() {
  using namespace asobmshow::bms_search;
  CleanupPaths cleanup;
  std::string error;
  const auto fixture = createFindBmsDownloadAttempt("fixture.zip", error);
  assert(fixture);
  cleanup.add(fixture->root);
  writeExtractionZip(fixture->archivePath, {{"chart.bms", "#TITLE test\n#BPM 120\n"}});
  downloads::Job job;
  job.id = "41b74c17434d423aa40f9399b2f0382c";
  job.request.url = job.request.displayUrl = "https://fixture.invalid/package.zip";
  job.request.archiveName = "song.zip";
  job.request.libraryRoot = testDownloadRoot(*fixture).parent_path();
  cleanup.add(job.request.libraryRoot);
  job.request.skipUnarchiving = true;
  const auto privateJobRoot = findBmsStagingBasePath().parent_path() / job.id;
  std::filesystem::create_directories(privateJobRoot);
  cleanup.add(privateJobRoot);
  const auto retained = privateJobRoot / "archive.1.zip";
  std::filesystem::rename(fixture->archivePath, retained);
  std::atomic_bool cancelled = false;
  int prepared = 0;
  observedDownloadAttempt.clear();
  const auto result = downloads::importArchiveJob(job, retained, privateJobRoot,
      cancelled, {}, [&](const BmsSearchPendingArtifact &) {
        assert(std::filesystem::hard_link_count(retained) == 1);
        ++prepared; return true;
      });
  if (result.status != BmsSearchResult::Status::Downloaded) std::cerr << result.message << '\n';
  assert(result.status == BmsSearchResult::Status::Downloaded);
  assert(!result.outputPath.empty() && std::filesystem::is_regular_file(result.outputPath));
  assert(std::filesystem::exists(retained)); // Retained until durable result delivery.
  assert(std::filesystem::hard_link_count(retained) == 1);
  assert(observedDownloadAttempt.empty());
  assert(prepared == 1);
}

int testVerificationAllocationGuard() {
  using namespace asobmshow::bms_search;
  CleanupPaths cleanup;
  std::string error;
  const auto fixture = createFindBmsDownloadAttempt("verification.zip", error);
  assert(fixture);
  cleanup.add(fixture->root);
  writeExtractionZip(fixture->archivePath, {{"chart.bms", "#TITLE Test\n", 17U * 1024 * 1024}});
  writeText(fixture->extractedPath / "chart.bms", std::string(17U * 1024 * 1024, 'a'));
  const ArchiveVerificationLimits limits{.maxMemberBytes = 16U * 1024 * 1024,
                                         .maxTotalBytes = 256U * 1024 * 1024};
  int failures = 0;
  for (const bool packed : {true, false}) {
    verification_allocation_guard::rejected = 0;
    verification_allocation_guard::enabled = true;
    bool rejectedCleanly = false;
    try {
      if (packed) {
        const auto decision = decideDownloadedArchive(fixture->archivePath, "", true, {}, defaultArchiveReaderDependencies(), limits);
        rejectedCleanly = decision.disposition != DirectArchiveDisposition::KeepArchive && decision.message.find("limit") != std::string::npos;
      } else {
        const auto decision = decideExtractedArchive(fixture->extractedPath, std::string(32, '0'), {}, limits);
        rejectedCleanly = decision.disposition == ExtractedArchiveDisposition::Inconclusive && decision.message.find("limit") != std::string::npos;
      }
    } catch (const std::bad_alloc &) {
    }
    verification_allocation_guard::enabled = false;
    if (!rejectedCleanly || verification_allocation_guard::rejected) {
      std::cerr << "FAIL: " << (packed ? "packed" : "extracted")
                << " verification must reject before large allocation; attempted="
                << verification_allocation_guard::rejected << '\n';
      ++failures;
    }
  }
  return failures ? 1 : 0;
}

}
