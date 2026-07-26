#include "FileChecksum.h"
#include "replay/BeatorajaReplayCodec.h"
#include "replay/BeatorajaReplayPath.h"
#include "replay/ReplayFileStore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

using Bytes = std::vector<std::byte>;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

template <typename T, typename U>
void expectEqual(const T &actual, const U &expected, std::string_view message) {
  expect(actual == expected, message);
}

class TempDirectory {
public:
  explicit TempDirectory(std::string_view label) {
    static std::atomic<unsigned long long> counter = 0;
    const auto nonce = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    path = std::filesystem::temp_directory_path() /
           ("asobmashow-replay-store-" + std::string(label) + "-" +
            std::to_string(nonce) + "-" + std::to_string(counter++));
    std::filesystem::create_directories(path);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }

  std::filesystem::path path;
};

constexpr std::string_view kShaA =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kShaB =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

replay::ReplayPlaybackData sampleReplay(std::string_view sha = kShaA) {
  replay::ReplayPlaybackData value;
  value.setup.chartMd5 = "0123456789abcdef0123456789abcdef";
  value.setup.chartSha256 = sha;
  value.setup.keyMode = 7;
  value.setup.playOption = "NORMAL";
  value.setup.playOption2 = "NORMAL";
  value.setup.playbackRulesetId = "asobmashow";
  value.setup.playbackRulesetRevision = 1;
  value.setup.initialLaneCoverPercent = 20;
  value.setup.laneCoverEnabled = true;
  const replay::LogicalControl firstLane{
      .kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 0};
  value.input = {
      {.songTimeMicros = 1000, .control = firstLane, .pressed = true},
      {.songTimeMicros = 2000, .control = firstLane, .pressed = false},
  };
  return value;
}

Bytes encode(const replay::ReplayPlaybackData &value,
             std::int64_t playedAt = 1'725'000'000'000LL) {
  replay::BeatorajaReplayCodec codec;
  std::string diagnostic;
  const auto encoded = codec.encodeChart(value, playedAt, diagnostic);
  expect(encoded.has_value(), "test replay encodes");
  return encoded.value_or(Bytes{});
}

replay::ReplayPathIdentity chartPath(int historyIndex = 0) {
  std::string diagnostic;
  const auto path = replay::pathForStem(kShaA, historyIndex, diagnostic);
  expect(path.has_value(), "test replay path is valid");
  return path.value_or(replay::ReplayPathIdentity{});
}

replay::ExpectedReplayIdentity
chartIdentity(std::string sha = std::string(kShaA), int longNoteMode = 0) {
  return {.stageSha256 = {std::move(sha)},
          .stageLongNoteModes = {longNoteMode},
          .course = false};
}

Bytes readBytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  const std::vector<char> raw(std::istreambuf_iterator<char>(input), {});
  Bytes result(raw.size());
  std::transform(raw.begin(), raw.end(), result.begin(),
                 [](char value) { return static_cast<std::byte>(value); });
  return result;
}

void writeBytes(const std::filesystem::path &path,
                std::span<const std::byte> contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(contents.data()),
               static_cast<std::streamsize>(contents.size()));
  expect(output.good(), "test bytes are written");
}

void testFinalizeInspectLoadRetryCopyAndRemove() {
  TempDirectory profile("happy");
  replay::ReplayFileStore store(profile.path);
  replay::BeatorajaReplayCodec codec;
  const Bytes encoded = encode(sampleReplay());
  const auto finalized =
      store.finalize(chartPath(), encoded, codec, chartIdentity(), "attempt_1");
  expect(finalized.metadata.has_value(), "valid replay finalizes");
  expect(!finalized.existingIdenticalFile,
         "first finalization installs a new file");
  expect(finalized.diagnostic.empty(),
         "successful finalization has no diagnostic");
  if (!finalized.metadata) {
    return;
  }

  const auto finalPath = profile.path / finalized.metadata->relativePath;
  expect(std::filesystem::is_regular_file(finalPath),
         "final replay is a regular file");
  expectEqual(readBytes(finalPath), encoded,
              "final replay bytes equal encoded bytes");
  expectEqual(finalized.metadata->compressedSize,
              static_cast<std::uint64_t>(encoded.size()),
              "metadata stores compressed size");
  expectEqual(
      finalized.metadata->sha256,
      file_checksum::sha256(std::string_view(
          reinterpret_cast<const char *>(encoded.data()), encoded.size())),
      "metadata stores compressed-file SHA-256");
#ifndef _WIN32
  struct stat status{};
  expect(::stat(finalPath.c_str(), &status) == 0 &&
             (status.st_mode & 0777) == 0600,
         "new replay file has owner-only permissions");
#endif

  const auto inspection = store.inspect(*finalized.metadata);
  expectEqual(inspection.state, replay::ReplayFileState::Available,
              "installed replay inspects as available");
  expectEqual(inspection.metadata, finalized.metadata,
              "inspection returns validated metadata");
  const auto loaded = store.load(*finalized.metadata, codec);
  expect(loaded.chart.has_value(), "installed replay loads through codec");
  if (loaded.chart) {
    expectEqual(loaded.chart->setup.chartSha256, std::string(kShaA),
                "loaded replay identity matches chart");
  }

  const auto retry =
      store.finalize(chartPath(), encoded, codec, chartIdentity(), "attempt_2");
  expectEqual(retry.metadata, finalized.metadata,
              "idempotent retry returns identical metadata");
  expect(retry.existingIdenticalFile,
         "idempotent retry identifies existing identical file");

  std::string diagnostic;
  expect(store.copyToBeatorajaSlot(*finalized.metadata, kShaA, 1, diagnostic),
         "replay copies to visible Beatoraja slot");
  const auto slotPath =
      profile.path / "replay" / (std::string(kShaA) + "_1.brd");
  expectEqual(readBytes(slotPath), encoded,
              "slot copy preserves exact bytes without conversion");
  expect(!store.copyToBeatorajaSlot(*finalized.metadata, kShaA, -1, diagnostic),
         "negative visible slot is rejected");
  expect(!store.copyToBeatorajaSlot(*finalized.metadata, kShaA, 4, diagnostic),
         "slot outside Beatoraja's visible 0..3 range is rejected");

  expect(store.remove(*finalized.metadata, diagnostic),
         "validated replay removes safely");
  expect(!std::filesystem::exists(finalPath),
         "removed replay final path is gone");
}

void testDifferentCollisionFailsClosed() {
  TempDirectory profile("collision");
  replay::ReplayFileStore store(profile.path);
  replay::BeatorajaReplayCodec codec;
  const Bytes first = encode(sampleReplay(), 1000);
  const Bytes second = encode(sampleReplay(), 2000);
  const auto installed =
      store.finalize(chartPath(), first, codec, chartIdentity(), "first");
  expect(installed.metadata.has_value(), "collision fixture installs");
  const auto collision =
      store.finalize(chartPath(), second, codec, chartIdentity(), "second");
  expect(!collision.metadata,
         "different bytes at an existing replay path fail closed");
  expect(!collision.diagnostic.empty(),
         "different-byte collision reports a diagnostic");
  expectEqual(readBytes(profile.path / chartPath().relativePath), first,
              "different-byte collision never overwrites final replay");
}

void testFinalizationRejectsWrongLongNoteIdentity() {
  TempDirectory profile("wrong-ln-mode");
  replay::ReplayFileStore store(profile.path);
  replay::BeatorajaReplayCodec codec;
  auto replay = sampleReplay();
  replay.setup.longNoteMode = 2;
  const Bytes encoded = encode(replay);

  const auto outcome = store.finalize(chartPath(), encoded, codec,
                                      chartIdentity(std::string(kShaA), 3),
                                      "wrong_mode");
  expect(!outcome.metadata.has_value(),
         "finalization rejects a BRD with the wrong application LN mode");
  expect(!outcome.diagnostic.empty(),
         "wrong replay LN identity reports a diagnostic");
}

void testExplicitSlotCopyReplacesOccupiedReplay() {
  TempDirectory profile("slot-replacement");
  replay::ReplayFileStore store(profile.path);
  replay::BeatorajaReplayCodec codec;
  const Bytes first = encode(sampleReplay(), 1000);
  const Bytes second = encode(sampleReplay(), 2000);
  const auto firstReplay =
      store.finalize(chartPath(0), first, codec, chartIdentity(), "first");
  const auto secondReplay =
      store.finalize(chartPath(4), second, codec, chartIdentity(), "second");
  expect(firstReplay.metadata.has_value() && secondReplay.metadata.has_value(),
         "occupied slot replacement fixtures finalize");
  if (!firstReplay.metadata || !secondReplay.metadata) {
    return;
  }

  std::string diagnostic;
  expect(store.copyToBeatorajaSlot(*firstReplay.metadata, kShaA, 1, diagnostic),
         "first replay occupies a visible Beatoraja slot");
  expect(
      store.copyToBeatorajaSlot(*secondReplay.metadata, kShaA, 1, diagnostic),
      "explicit copy replaces an occupied visible Beatoraja slot");
  expectEqual(
      readBytes(profile.path / "replay" / (std::string(kShaA) + "_1.brd")),
      second, "occupied slot replacement installs the selected replay bytes");
}

void testInjectedDurabilityFaults() {
  replay::BeatorajaReplayCodec codec;
  const Bytes encoded = encode(sampleReplay());
  for (const std::string_view stage :
       {"write", "file-sync", "close", "rename", "directory-sync", "read-back",
        "decode", "hash"}) {
    TempDirectory profile(std::string("fault-") + std::string(stage));
    replay::ReplayFileStore faulty(
        profile.path,
        {.failAt = [stage](std::string_view point) { return point == stage; }});
    const auto outcome = faulty.finalize(chartPath(), encoded, codec,
                                         chartIdentity(), "fault_attempt");
    expect(!outcome.metadata, "injected durability fault rejects finalization");
    expect(!outcome.diagnostic.empty(),
           "injected durability fault reports a diagnostic");
    const bool finalExists =
        std::filesystem::exists(profile.path / chartPath().relativePath);
    const bool preRename = stage == "write" || stage == "file-sync" ||
                           stage == "close" || stage == "rename";
    expect(finalExists != preRename,
           "only post-rename faults may expose a reusable final file");
    if (!preRename) {
      if (stage == "directory-sync") {
        replay::ReplayFileStore stillFaulty(
            profile.path,
            {.failAt = [](std::string_view point) {
              return point == "directory-sync";
            }});
        const auto blockedRetry = stillFaulty.finalize(
            chartPath(), encoded, codec, chartIdentity(), "blocked_retry");
        expect(!blockedRetry.metadata,
               "existing replay retry still requires directory durability");
      }
      replay::ReplayFileStore retryStore(profile.path);
      const auto retry = retryStore.finalize(chartPath(), encoded, codec,
                                             chartIdentity(), "retry");
      expect(retry.metadata.has_value(),
             "post-rename failure leaves a reusable final replay");
      expect(retry.existingIdenticalFile,
             "post-rename retry recognizes identical final replay");
    }
  }
}

void testUnsafePathsAndLinks() {
  replay::BeatorajaReplayCodec codec;
  const Bytes encoded = encode(sampleReplay());

  TempDirectory traversal("traversal");
  replay::ReplayFileStore traversalStore(traversal.path);
  replay::ReplayPathIdentity unsafePath{
      .stem = std::string(kShaA),
      .historyIndex = 0,
      .relativePath = "../outside.brd",
  };
  expect(!traversalStore
              .finalize(unsafePath, encoded, codec, chartIdentity(), "unsafe")
              .metadata,
         "finalization rejects traversal relative path");
  unsafePath.relativePath = traversal.path / "absolute.brd";
  expect(!traversalStore
              .finalize(unsafePath, encoded, codec, chartIdentity(), "unsafe")
              .metadata,
         "finalization rejects absolute path");

  TempDirectory inconsistent("inconsistent-identity");
  replay::ReplayFileStore inconsistentStore(inconsistent.path);
  std::string pathDiagnostic;
  const auto otherChartPath = replay::pathForStem(kShaB, 0, pathDiagnostic);
  expect(otherChartPath.has_value(), "alternate canonical chart path is valid");
  if (otherChartPath) {
    replay::ReplayPathIdentity inconsistentIdentity = chartPath();
    inconsistentIdentity.relativePath = otherChartPath->relativePath;
    const auto outcome = inconsistentStore.finalize(
        inconsistentIdentity, encoded, codec, chartIdentity(), "inconsistent");
    expect(!outcome.metadata,
           "finalization rejects a safe path inconsistent with its stem and "
           "history index");
    expect(!std::filesystem::exists(inconsistent.path /
                                    otherChartPath->relativePath),
           "inconsistent replay identity writes no aliased final file");
  }

  replay::ReplayFileMetadata unsafeMetadata{
      .relativePath = "../outside.brd",
      .sha256 = std::string(64, '0'),
      .compressedSize = 1,
  };
  expectEqual(traversalStore.inspect(unsafeMetadata).state,
              replay::ReplayFileState::Unsafe,
              "inspection rejects traversal metadata");
  unsafeMetadata.relativePath = traversal.path / "absolute.brd";
  expectEqual(traversalStore.inspect(unsafeMetadata).state,
              replay::ReplayFileState::Unsafe,
              "inspection rejects absolute metadata");

#ifndef _WIN32
  TempDirectory symlinkFile("symlink-file");
  std::filesystem::create_directories(symlinkFile.path / "replay");
  const auto outsideFile = symlinkFile.path / "outside.brd";
  writeBytes(outsideFile, encoded);
  std::filesystem::create_symlink(outsideFile,
                                  symlinkFile.path / chartPath().relativePath);
  replay::ReplayFileStore symlinkFileStore(symlinkFile.path);
  expect(!symlinkFileStore
              .finalize(chartPath(), encoded, codec, chartIdentity(), "link")
              .metadata,
         "finalization rejects symlink final file");
  replay::ReplayFileMetadata linkedMetadata{
      .relativePath = chartPath().relativePath,
      .sha256 = std::string(64, '0'),
      .compressedSize = encoded.size(),
  };
  expectEqual(symlinkFileStore.inspect(linkedMetadata).state,
              replay::ReplayFileState::Unsafe,
              "inspection rejects symlink replay file");

  TempDirectory symlinkParent("symlink-parent");
  const auto outsideDirectory = symlinkParent.path / "outside";
  std::filesystem::create_directory(outsideDirectory);
  std::filesystem::create_directory_symlink(outsideDirectory,
                                            symlinkParent.path / "replay");
  replay::ReplayFileStore symlinkParentStore(symlinkParent.path);
  expect(!symlinkParentStore
              .finalize(chartPath(), encoded, codec, chartIdentity(), "parent")
              .metadata,
         "finalization rejects symlink replay parent");
  expect(!std::filesystem::exists(outsideDirectory /
                                  chartPath().relativePath.filename()),
         "symlink parent rejection writes nothing outside profile replay dir");

  TempDirectory hardLink("hard-link");
  std::filesystem::create_directory(hardLink.path / "replay");
  const auto hardLinkSource = hardLink.path / "outside.brd";
  writeBytes(hardLinkSource, encoded);
  std::error_code hardLinkError;
  std::filesystem::create_hard_link(
      hardLinkSource, hardLink.path / chartPath().relativePath, hardLinkError);
  if (!hardLinkError) {
    replay::ReplayFileStore hardLinkStore(hardLink.path);
    expect(!hardLinkStore
                .finalize(chartPath(), encoded, codec, chartIdentity(), "hard")
                .metadata,
           "finalization rejects hard-link replacement");
    expectEqual(hardLinkStore.inspect(linkedMetadata).state,
                replay::ReplayFileState::Unsafe,
                "inspection rejects replay with multiple hard links");
  }
#endif
}

void testMissingHashMismatchCorruptAndIdentityMismatch() {
  replay::BeatorajaReplayCodec codec;
  const Bytes encoded = encode(sampleReplay());
  TempDirectory profile("inspection");
  replay::ReplayFileStore store(profile.path);

  replay::ReplayFileMetadata missing{
      .relativePath = chartPath().relativePath,
      .sha256 = std::string(64, '0'),
      .compressedSize = encoded.size(),
  };
  expectEqual(store.inspect(missing).state, replay::ReplayFileState::Missing,
              "missing replay inspects as missing");

  const auto installed =
      store.finalize(chartPath(), encoded, codec, chartIdentity(), "inspect");
  expect(installed.metadata.has_value(), "inspection fixture installs");
  if (installed.metadata) {
    Bytes changed = encoded;
    changed.back() ^= std::byte{0x01};
    writeBytes(profile.path / installed.metadata->relativePath, changed);
    expectEqual(store.inspect(*installed.metadata).state,
                replay::ReplayFileState::Corrupt,
                "hash mismatch inspects as corrupt");
    const auto loaded = store.load(*installed.metadata, codec);
    expect(!loaded.chart && !loaded.course,
           "hash-mismatched replay does not reach codec playback");
    changed.push_back(std::byte{0x00});
    writeBytes(profile.path / installed.metadata->relativePath, changed);
    expectEqual(store.inspect(*installed.metadata).state,
                replay::ReplayFileState::Corrupt,
                "oversized replay inspects as corrupt without allocation");
    std::string diagnostic;
    expect(!store.removeIfMatches(*installed.metadata, diagnostic),
           "ownership cleanup rejects changed replay bytes");
    expectEqual(readBytes(profile.path / installed.metadata->relativePath),
                changed,
                "ownership cleanup restores changed replay bytes in place");
    expect(store.remove(*installed.metadata, diagnostic),
           "user can delete a corrupt but safely contained replay file");
    expect(!std::filesystem::exists(profile.path /
                                    installed.metadata->relativePath),
           "corrupt replay deletion removes its final path");
  }

  TempDirectory corrupt("corrupt");
  replay::ReplayFileStore corruptStore(corrupt.path);
  const Bytes garbage = {std::byte{0x01}, std::byte{0x02}};
  const auto corruptOutcome = corruptStore.finalize(chartPath(), garbage, codec,
                                                    chartIdentity(), "corrupt");
  expect(!corruptOutcome.metadata, "undecodable final replay is rejected");
  expect(std::filesystem::exists(corrupt.path / chartPath().relativePath),
         "post-rename decode failure leaves reusable evidence");

  TempDirectory identity("identity");
  replay::ReplayFileStore identityStore(identity.path);
  const Bytes wrongChart = encode(sampleReplay(kShaB));
  const auto identityOutcome = identityStore.finalize(
      chartPath(), wrongChart, codec, chartIdentity(), "identity");
  expect(!identityOutcome.metadata,
         "decoded chart identity mismatch is rejected");
}

void testOversizedMetadataIsRejectedBeforeFileAccess() {
  TempDirectory profile("oversized-metadata");
  replay::ReplayFileStore store(profile.path);
  const replay::ReplayFileMetadata oversized{
      .relativePath = chartPath().relativePath,
      .sha256 = std::string(64, 'a'),
      .compressedSize = replay::ReplayCodecLimits::kMaximumCompressedBytes + 1,
      .codecVersion = replay::BeatorajaReplayCodec::kCodecVersion,
  };

  expectEqual(store.inspect(oversized).state, replay::ReplayFileState::Corrupt,
              "oversized replay metadata is corrupt even when its file is "
              "missing");
  replay::BeatorajaReplayCodec codec;
  const auto loaded = store.load(oversized, codec);
  expect(!loaded.chart && !loaded.course &&
             loaded.diagnostic.find("size limit") != std::string::npos,
         "oversized replay metadata is rejected before allocation");
  const auto staged = store.stageVerifiedSnapshot(oversized);
  expectEqual(staged.state, replay::ReplayFileState::Corrupt,
              "oversized replay metadata cannot be staged for sharing");
  std::string diagnostic;
  expect(!store.removeIfMatches(oversized, diagnostic) &&
             diagnostic.find("invalid") != std::string::npos,
         "oversized replay metadata cannot enter ownership cleanup");
}

void testOwnedCleanupDoesNotDeleteConcurrentReplacement() {
  TempDirectory profile("cleanup-replacement");
  replay::BeatorajaReplayCodec codec;
  const Bytes encoded = encode(sampleReplay(), 1000);
  replay::ReplayFileStore installer(profile.path);
  const auto installed = installer.finalize(chartPath(), encoded, codec,
                                            chartIdentity(), "cleanup_fixture");
  expect(installed.metadata.has_value(), "cleanup race fixture installs");
  if (!installed.metadata) {
    return;
  }

  const auto finalPath = profile.path / installed.metadata->relativePath;
  const Bytes replacement = encode(sampleReplay(), 2000);
  bool replacementInstalled = false;
  replay::ReplayFileStore cleanup(profile.path,
                                  {.failAt = [&](std::string_view point) {
                                    if (point == "remove-after-quarantine") {
                                      writeBytes(finalPath, replacement);
                                      replacementInstalled = true;
                                    }
                                    return false;
                                  }});

  std::string diagnostic;
  expect(cleanup.removeIfMatches(*installed.metadata, diagnostic),
         "owned replay cleanup succeeds after quarantining its exact bytes");
  expect(replacementInstalled,
         "cleanup test replaces the public path after quarantine");
  expectEqual(readBytes(finalPath), replacement,
              "owned replay cleanup preserves a concurrent replacement");
}

void testStaleTemporaryCleanup() {
  TempDirectory profile("stale");
  std::filesystem::create_directory(profile.path / "replay");
  const auto stale =
      profile.path / "replay" / ("." + std::string(kShaA) + ".brd.old.tmp");
  const auto staleShare =
      profile.path / "replay" /
      ("." + std::string(kShaA) + ".brd.share-deadbeef-7.tmp");
  const auto unrelated = profile.path / "replay" / "keep.tmp";
  writeBytes(stale, Bytes{std::byte{0x01}});
  writeBytes(staleShare, Bytes{std::byte{0x02}});
  writeBytes(unrelated, Bytes{std::byte{0x02}});
  std::error_code error;
  std::filesystem::last_write_time(
      stale,
      std::filesystem::file_time_type::clock::now() - std::chrono::hours(2),
      error);
  expect(!error, "stale test file timestamp is set");
  std::filesystem::last_write_time(
      staleShare,
      std::filesystem::file_time_type::clock::now() - std::chrono::hours(2),
      error);
  expect(!error, "stale share snapshot timestamp is set");
  expect(
      replay::isPrivateReplayTemporaryFilename(staleShare.filename().string()),
      "share snapshots use the recognized private replay temporary shape");
  replay::ReplayFileStore store(profile.path);
  store.removeStaleTemporaryFiles(std::chrono::system_clock::now() -
                                  std::chrono::hours(1));
  expect(!std::filesystem::exists(stale),
         "stale private replay temporary file is removed");
  expect(!std::filesystem::exists(staleShare),
         "stale private replay share snapshot is removed at startup");
  expect(std::filesystem::exists(unrelated),
         "unrelated temporary-looking file is retained");
}

void testVerifiedShareSnapshotLifecycleAndTokenFailure() {
  TempDirectory profile("share-snapshot");
  replay::BeatorajaReplayCodec codec;
  const Bytes encoded = encode(sampleReplay());
  replay::ReplayFileStore store(profile.path);
  const auto installed = store.finalize(chartPath(), encoded, codec,
                                        chartIdentity(), "share_fixture");
  expect(installed.metadata.has_value(), "share snapshot fixture installs");
  if (!installed.metadata.has_value()) {
    return;
  }

  auto staged = store.stageVerifiedSnapshot(*installed.metadata);
  expectEqual(staged.state, replay::ReplayFileState::Available,
              "verified share snapshot stages");
  expect(staged.snapshot.has_value() &&
             staged.snapshot->sourceLifetime != nullptr,
         "verified share snapshot retains an ownership lifetime");
  if (staged.snapshot.has_value()) {
    const auto snapshotPath = staged.snapshot->sourcePath;
    expect(std::filesystem::exists(snapshotPath),
           "verified share snapshot exists while its owner is retained");
    staged.snapshot.reset();
    expect(!std::filesystem::exists(snapshotPath),
           "verified share snapshot is deleted when its owner is released");
  }

  auto replaced = store.stageVerifiedSnapshot(*installed.metadata);
  expect(replaced.snapshot.has_value(),
         "replacement-preservation share snapshot stages");
  if (replaced.snapshot.has_value()) {
    const auto snapshotPath = replaced.snapshot->sourcePath;
    const Bytes replacement{std::byte{0x61}, std::byte{0x62}};
    writeBytes(snapshotPath, replacement);
    replaced.snapshot.reset();
    expect(std::filesystem::exists(snapshotPath) &&
               readBytes(snapshotPath) == replacement,
           "share lifetime cleanup preserves a replaced snapshot path");
  }

  replay::ReplayFileStore failingStore(
      profile.path, {.failAt = [](std::string_view point) {
        return point == "share-token-generation";
      }});
  const auto failed = failingStore.stageVerifiedSnapshot(*installed.metadata);
  expectEqual(failed.state, replay::ReplayFileState::IoFailure,
              "share token generation failure is reported as I/O failure");
  expect(!failed.snapshot.has_value() && !failed.diagnostic.empty(),
         "share token generation failure returns no export source and a "
         "diagnostic");
}

} // namespace

int main() {
  testFinalizeInspectLoadRetryCopyAndRemove();
  testDifferentCollisionFailsClosed();
  testFinalizationRejectsWrongLongNoteIdentity();
  testExplicitSlotCopyReplacesOccupiedReplay();
  testInjectedDurabilityFaults();
  testUnsafePathsAndLinks();
  testMissingHashMismatchCorruptAndIdentityMismatch();
  testOversizedMetadataIsRejectedBeforeFileAccess();
  testOwnedCleanupDoesNotDeleteConcurrentReplacement();
  testStaleTemporaryCleanup();
  testVerifiedShareSnapshotLifecycleAndTokenFailure();
  if (failures != 0) {
    std::cerr << failures << " replay file store test(s) failed\n";
    return 1;
  }
  std::cout << "Replay file store tests passed\n";
  return 0;
}
