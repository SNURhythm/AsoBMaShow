#include "../src/storage/StorageOptimizer.h"
#include "../src/storage/StorageMeasurement.h"
#include "../src/storage/StorageMeasurementState.h"
#include "StorageTestSupport.h"
#include "../src/bms_search/DownloadStorageOptimization.h"

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

using namespace storage_management;
using storage_test::require;
using storage_test::TempDirectory;
void storageLibraryIntegration();
void storageArchiveTests(const char *executable);
void storageArchiveParallelTests();
void storageArchiveTraversalBenchmark(std::size_t count);
void storageArchiveCompactionBenchmark(std::size_t count, std::size_t payloadBytes, bool deleteFirst);
void storageArchiveDiscoverySandboxTest(const std::filesystem::path &root);
int storageDirectoryInventoryTests();
namespace fs = std::filesystem;
namespace {
OptimizationRequest request(const fs::path &root) {
  OptimizationRequest result;
  result.policy.audio = AudioAction::LossyVorbis;
  result.policy.bga = BgaAction::CompressH264;
  result.inventory.policy = result.policy;
  result.inventory.groups = {{"group", root}};
  return result;
}
OptimizerDependencies dependencies(TempDirectory &temp, std::vector<std::string> &events) {
  auto result = DefaultOptimizerDependencies(MakeStoragePlatformPaths(temp.root / "private", {temp.root / "charts"}));
  const auto eventMutex = std::make_shared<std::mutex>();
  result.executionLimits.audioWorkers = 1;
  result.probeCodecs = [] {
    StorageCodecCapabilities caps;
    caps.flac.available = caps.vorbis.available = caps.x264.available = caps.aac.available = caps.mp4.available = true;
    return caps;
  };
  result.encodeAudio = result.encodeVideo = [&, eventMutex](const AssetCandidate &candidate, const StoragePolicy &,
      const fs::path &output, Checkpoint check, ProgressCallback progress) {
    require(!candidate.sourceHash.empty(), "source hash is captured before the encoder reads media");
    { std::lock_guard lock(*eventMutex); events.push_back("encode:" + candidate.source.filename().string()); }
    if (check && !check()) return EncodeResult{FileOutcome::Cancelled, {}, {StorageReason::Cancelled, "cancel"}};
    std::ofstream file(output, std::ios::binary); file << "tiny"; file.close();
    if (progress) progress({StoragePhase::Verify, candidate.source});
    { std::lock_guard lock(*eventMutex); events.push_back("verify:" + candidate.source.filename().string()); }
    return EncodeResult{FileOutcome::Converted, output, {}};
  };
  const auto factory = result.transactions;
  result.transactions = [&, factory, eventMutex](const StoragePlatformPaths &paths, StorageCommitPrecondition beforeCommit) {
    auto transactions = factory(paths, std::move(beforeCommit));
    const auto replace = transactions.replace;
    transactions.replace = [&, replace, eventMutex](const AssetCandidate &candidate, const EncodeResult &encoded, Checkpoint check) {
      { std::lock_guard lock(*eventMutex); events.push_back("commit:" + candidate.source.filename().string()); }
      require(fs::exists(encoded.outputPath), "validated temporary output reaches transaction intact");
      return replace(candidate, encoded, check);
    };
    return transactions;
  };
  return result;
}
void reviewedSelectionRetainsPrivateSandboxBoundary() {
  TempDirectory temp;
  temp.write("charts/song/sound.wav", std::string(100, 'a'));
  temp.write("charts/song/movie.mpg", std::string(100, 'v'));
  const auto libraryRoot = temp.root / "charts";
  const auto privateRoot = temp.root / "private";
  fs::create_directory(privateRoot);
  auto input = request(libraryRoot / "song");
  input.policy.bga = BgaAction::Delete; input.inventory.policy = input.policy;
  // Manual review knows the registered library, while platform dependencies
  // additionally supply the application's private sandbox boundary.
  input.inventory.pathAccessRoots = {libraryRoot};
  input.preflight = BuildStoragePreflight(input.inventory);
  require(input.preflight->complete && input.preflight->eligibleCount == 2,
          "manual review admits replacement and deletion within the registered library");
  std::vector<std::string> events;
  auto deps = dependencies(temp, events);
  deps.paths = MakeStoragePlatformPaths(privateRoot, {libraryRoot}, true);
  const auto factory = DefaultOptimizerDependencies(deps.paths).transactions;
  bool checkedLocalPaths = false;
  deps.transactions = [&](const StoragePlatformPaths &paths, StorageCommitPrecondition beforeCommit) {
    if (beforeCommit) {
      require(std::find(paths.pathAccessRoots.begin(), paths.pathAccessRoots.end(), libraryRoot) != paths.pathAccessRoots.end() &&
                  std::find(paths.pathAccessRoots.begin(), paths.pathAccessRoots.end(), privateRoot) != paths.pathAccessRoots.end(),
              "local transactions retain both source and private sandbox access boundaries");
      require(paths.allowedRoots == std::vector<fs::path>{libraryRoot},
              "private sandbox access does not grant source mutation authority");
      checkedLocalPaths = true;
    }
    return factory(paths, std::move(beforeCommit));
  };
  const auto report = StorageOptimizer(deps).Run(std::move(input));
  require(checkedLocalPaths && report.converted == 1 && report.deleted == 1 && report.reclaimedBytes == 196 &&
              !report.failed && !report.skipped && !report.recoveryRequired && !report.cleanupPending && !report.cancelled,
          "reviewed sandboxed selection completes real replacement and deletion transactions");
  require(!fs::exists(libraryRoot / "song/sound.wav") && !fs::exists(libraryRoot / "song/movie.mpg") &&
              fs::file_size(libraryRoot / "song/sound.ogg") == 4,
          "sandboxed transactions install encoded audio and remove the selected video");
  require(fs::is_empty(deps.paths.temporaryRoot) && fs::is_empty(deps.paths.journalRoot),
          "sandboxed transactions settle private temporary files and journals");
}
void growingOutputsRespectSharedScratch() {
  for (const bool pollDuringEncode : {true, false}) {
    TempDirectory temp;
    temp.write("charts/a/a.wav", std::string(1000, 'a'));
    temp.write("charts/b/b.wav", std::string(1000, 'b'));
    std::vector<std::string> events;
    auto deps = dependencies(temp, events); deps.executionLimits.audioWorkers = 2;
    auto now = std::chrono::steady_clock::time_point{};
    deps.now = [&] { now += std::chrono::milliseconds(101); return now; };
    deps.availableSpace = [](const fs::path &) { return std::optional<std::uint64_t>(5 * 1024 * 1024); };
    std::mutex mutex; std::condition_variable changed;
    unsigned entered = 0; bool release = false, growthRejected = false;
    deps.encodeAudio = [&](const AssetCandidate &asset, const StoragePolicy &, const fs::path &output, Checkpoint check, auto) {
      { std::unique_lock lock(mutex); ++entered; changed.notify_all();
        require(changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered == 2; }), "both owners reserve their encoding space"); }
      if (asset.source.filename() == "a.wav") {
        { std::ofstream file(output, std::ios::binary); file.seekp(7 * 512 * 1024 - 1); file.put('x'); }
        if (pollDuringEncode) growthRejected = !check();
      } else {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, std::chrono::seconds(2), [&] { return release; }), "growing owner settles while peer retains scratch");
        std::ofstream(output) << "tiny";
      }
      // Also exercise an encoder that returns success after its last checkpoint.
      return EncodeResult{FileOutcome::Converted, output, {}};
    };
    auto input = request(temp.root / "charts");
    input.policy.audio = AudioAction::LosslessFlac; input.inventory.policy = input.policy;
    const auto report = StorageOptimizer(deps).Run(input, {}, [&](const StorageProgress &progress) {
      if (progress.filesDone && (progress.phase == StoragePhase::Encode || progress.phase == StoragePhase::Commit)) {
        std::lock_guard lock(mutex); release = true; changed.notify_all();
      }
    });
    require((!pollDuringEncode || growthRejected) && report.converted == 1 && report.skipped == 1 &&
        !report.failed && !report.cancelled && !report.recoveryRequired,
        "growing output cannot consume another owner's shared scratch reservation");
    require(std::any_of(report.files.begin(), report.files.end(), [](const FileResult &file) {
      return file.source.filename() == "a.wav" && file.reason.code == StorageReason::InsufficientSpace;
    }) && fs::exists(temp.root / "charts/a/a.wav") && !fs::exists(temp.root / "charts/a/a.flac") &&
        fs::file_size(temp.root / "charts/b/b.flac") == 4 && fs::is_empty(deps.paths.temporaryRoot),
        "over-budget output is discarded before commit and the peer completes normally");
  }
}
void losslessGrowthOffsetsSavingsAcrossDirectories() {
  for (const auto finalSize : {155u, 205u}) {
    TempDirectory temp;
    for (const auto path : {"charts/a/one.wav", "charts/a/two.wav", "charts/b/one.wav", "charts/b/two.wav"})
      temp.write(path, std::string(100, 's'));
    std::vector<std::string> events;
    auto deps = dependencies(temp, events);
    deps.transactions = DefaultOptimizerDependencies(deps.paths).transactions;
    auto now = std::chrono::steady_clock::time_point{};
    deps.now = [&] { now += std::chrono::milliseconds(101); return now; };
    deps.encodeAudio = [&](const AssetCandidate &candidate, const StoragePolicy &, const fs::path &output, auto, auto) {
      const auto size = candidate.source.filename() == "one.wav" ? 40u :
          (candidate.source.parent_path().filename() == "a" ? 140u : finalSize);
      std::ofstream(output, std::ios::binary) << std::string(size, 'f');
      return EncodeResult{FileOutcome::Converted, output, {}};
    };
    auto input = request(temp.root / "charts");
    input.policy.audio = AudioAction::LosslessFlac; input.inventory.policy = input.policy;
    StorageProgress lastWork;
    const auto report = StorageOptimizer(deps).Run(std::move(input), {}, [&](const StorageProgress &value) {
      if (value.phase != StoragePhase::Complete) lastWork = value;
    });
    const auto savings = finalSize == 155 ? 25u : 0u;
    require(report.converted == 4 && report.reclaimedBytes == savings &&
                report.addedBytes == (finalSize == 205 ? 25u : 0u) && !report.failed,
            "lossless growth offsets savings within and across directory reports");
    require(lastWork.filesDone == 4 && lastWork.reclaimedBytes == savings &&
                lastWork.addedBytes == (finalSize == 205 ? 25u : 0u),
            "worker progress can decrease reclaimed bytes after larger lossless replacements");
    require(report.groupDeltas.size() == 1 && report.groupDeltas.front().bytesBefore == 400 &&
                report.groupDeltas.front().bytesAfter == (finalSize == 155 ? 375u : 425u),
            "group measurement records the actual net size after lossless growth");
  }
}
void independentDirectoriesUseDistinctOutputs() {
  TempDirectory temp;
  temp.write("charts/a/one.wav", std::string(100, 'a'));
  temp.write("charts/b/two.wav", std::string(100, 'b'));
  std::vector<std::string> events;
  auto deps = dependencies(temp, events); deps.executionLimits.audioWorkers = 2;
  deps.transactions = DefaultOptimizerDependencies(deps.paths).transactions;
  std::mutex mutex; std::condition_variable changed; int entered = 0;
  std::set<fs::path> outputs;
  deps.encodeAudio = [&](const auto &, const auto &, const fs::path &output, auto, auto) {
    { std::unique_lock lock(mutex); outputs.insert(output); ++entered; changed.notify_all();
      require(changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered == 2; }), "independent encoders overlap"); }
    std::ofstream(output) << "tiny";
    return EncodeResult{FileOutcome::Converted, output, {}};
  };
  auto report = StorageOptimizer(deps).Run(request(temp.root / "charts"));
  require(report.converted == 2 && !report.recoveryRequired && outputs.size() == 2,
      "parallel directories commit distinct temporary outputs");
  require(report.groupDeltas.size() == 1 && report.groupDeltas[0].bytesBefore == 200 && report.groupDeltas[0].bytesAfter == 8,
      "parallel directory deltas aggregate once into their parent");
}
void slowVideoDoesNotBlockFollowingFiles() {
  for (const int mode : {0, 1, 2}) {
    const bool cancel = mode == 1, changedSource = mode == 2;
    TempDirectory temp;
    for (const auto *name : {"a.mpg", "b.mpg", "c.wav"}) temp.write(fs::path("charts") / name, std::string(100, 's'));
    std::vector<std::string> events;
    auto deps = dependencies(temp, events);
    const auto factory = DefaultOptimizerDependencies(deps.paths).transactions;
    std::mutex mutex; std::condition_variable changed;
    bool release = false; unsigned laterCommits = 0;
    std::atomic<bool> cancelled = false;
    deps.encodeAudio = deps.encodeVideo = [&](const AssetCandidate &asset, const StoragePolicy &,
        const fs::path &output, Checkpoint check, ProgressCallback) {
      require(!asset.sourceHash.empty(), "parallel encoder receives hashed source evidence");
      if (asset.source.filename() == "a.mpg") {
        std::unique_lock lock(mutex); changed.wait(lock, [&] { return release; });
      }
      if (!check()) return EncodeResult{FileOutcome::Cancelled, {}, {StorageReason::Cancelled, "cancel"}};
      std::ofstream(output) << "tiny";
      return EncodeResult{FileOutcome::Converted, output, {}};
    };
    deps.transactions = [&, factory](const auto &paths, auto beforeCommit) {
      auto transactions = factory(paths, beforeCommit);
      const auto replace = transactions.replace;
      transactions.replace = [&, replace](const auto &asset, const auto &encoded, auto check) {
        auto result = replace(asset, encoded, check);
        if (asset.source.filename() != "a.mpg" && result.outcome == FileOutcome::Converted) {
          std::lock_guard lock(mutex); ++laterCommits; changed.notify_all();
        }
        return result;
      };
      return transactions;
    };
    auto run = std::async(std::launch::async, [&] {
      return StorageOptimizer(deps).Run(request(temp.root / "charts"), [&] { return !cancelled; });
    });
    bool progressed;
    { std::unique_lock lock(mutex);
      progressed = changed.wait_for(lock, std::chrono::seconds(2), [&] { return laterCommits == 2; });
      if (changedSource) temp.write("charts/a.mpg", "external edit while encoding");
      cancelled = cancel; release = true; changed.notify_all(); }
    const auto report = run.get();
    require(progressed, "later video and audio commit while the first same-directory video is held");
    require(report.converted == (mode ? 2 : 3) && !report.failed && !report.recoveryRequired &&
        report.cancelled == cancel && report.reclaimedBytes == (mode ? 192 : 288),
        "parallel commits retain exact accounting and cancellation drains the held video");
    require(fs::exists(temp.root / "charts/a.mpg") == (mode != 0) && fs::file_size(temp.root / "charts/b.mp4") == 4 &&
        fs::file_size(temp.root / "charts/c.ogg") == 4 && fs::is_empty(deps.paths.temporaryRoot) && fs::is_empty(deps.paths.journalRoot),
        "parallel outputs commit independently and all staging is removed after joining");
  }
}
void recoveryStopsAnAlreadyEncodedPeerBeforeCommit() {
  TempDirectory temp;
  temp.write("charts/a.wav", std::string(100, 'a')); temp.write("charts/b.wav", std::string(100, 'b'));
  std::vector<std::string> events;
  auto deps = dependencies(temp, events); deps.executionLimits.audioWorkers = 2;
  auto now = std::chrono::steady_clock::time_point{};
  deps.now = [&] { return now += std::chrono::milliseconds(101); };
  std::mutex mutex; std::condition_variable changed;
  bool peerEntered = false, recoveryReported = false;
  deps.encodeAudio = [&](const AssetCandidate &asset, const StoragePolicy &, const fs::path &output, auto, auto) {
    { std::unique_lock lock(mutex);
      if (asset.source.filename() == "b.wav") { peerEntered = true; changed.notify_all(); }
      require(changed.wait_for(lock, std::chrono::seconds(2), [&] {
        return asset.source.filename() == "a.wav" ? peerEntered : recoveryReported;
      }), "both encoders reach the interrupted commit boundary"); }
    std::ofstream(output) << "tiny";
    return EncodeResult{FileOutcome::Converted, output, {}};
  };
  const auto factory = DefaultOptimizerDependencies(deps.paths).transactions;
  deps.transactions = [factory](const auto &paths, auto beforeCommit) {
    auto operations = factory(paths, beforeCommit); const auto replace = operations.replace;
    operations.replace = [replace](const auto &asset, const auto &encoded, auto check) {
      auto result = replace(asset, encoded, check);
      result.reason = {StorageReason::RecoveryRequired, "retained uncertain transaction"}; return result;
    };
    return operations;
  };
  const auto report = StorageOptimizer(deps).Run(request(temp.root / "charts"), {}, [&](const StorageProgress &progress) {
    if (progress.filesDone == 1) { std::lock_guard lock(mutex); recoveryReported = true; changed.notify_all(); }
  });
  require(report.recoveryRequired && !report.failed && report.converted == 1 && report.reclaimedBytes == 96 &&
      fs::exists(temp.root / "charts/b.wav") && !fs::exists(temp.root / "charts/b.ogg"),
      "encoded work stopped by peer recovery is not counted as a committed conversion");
}
void subsetRefreshNeverEnumeratesAncestorSiblings() {
  for (bool externalEdit : {false, true}) {
    TempDirectory temp;
    temp.write("charts/selected/a.wav", std::string(100, 'a'));
    temp.write("charts/other/b.wav", std::string(100, 'b'));
    InventoryRequest browser;
    browser.groups = {{"parent", temp.root / "charts"}, {"selected", temp.root / "charts/selected"},
                      {"other", temp.root / "charts/other"}};
    auto measured = MeasureStorageGroups(browser);
    require(measured.complete, "browser measurements cover nested groups");
    StorageMeasurementRows rows;
    for (auto &row : measured.groups) { row.generation = 7; rows[row.groupId] = row; }
    auto selected = request(temp.root / "charts/selected");
    selected.inventory.groups[0].groupId = "selected";
    selected.measurementBaselines = measured.groups;
    selected.preflight = BuildStoragePreflight(selected.inventory);
    std::vector<std::string> events;
    auto deps = dependencies(temp, events);
    if (externalEdit) {
      const auto encode = deps.encodeAudio;
      deps.encodeAudio = [&, encode](const auto &asset, const auto &policy, const auto &output, auto check, auto progress) {
        auto result = encode(asset, policy, output, check, progress);
        temp.write("charts/selected/new.wav", "external");
        return result;
      };
    }
    const auto report = StorageOptimizer(deps).Run(std::move(selected));
    const auto refresh = ApplyStorageReportMeasurements(rows, browser.groups, report);
    if (!externalEdit) require(rows.at("parent").totalBytes == 104 && rows.at("selected").totalBytes == 4 && refresh.empty(),
        "exact committed deltas update cached ancestor estimates without a new walk");
    else require(refresh == std::set<std::string>{"selected"} && !rows.at("parent").totalBytes,
        "uncertain ancestor stays unavailable instead of recursively scanning siblings");
    InventoryRequest followup;
    for (const auto &group : browser.groups) if (refresh.contains(group.groupId)) followup.groups.push_back(group);
    StorageScanDependencies scan;
    scan.enumerate = [&](const auto &path, const auto &root, auto mode, auto check, auto visit, auto &stamp, auto *metrics, auto &error) {
      require(IsWithinFolder(path, temp.root / "charts/selected"), "subset followup never enumerates ancestor or sibling directories");
      return VisitStorageDirectory(path, root, mode, check, visit, stamp, metrics, error);
    };
    require(MeasureStorageGroups(followup, {}, {}, nullptr, scan).complete, "scoped uncertain rows can be remeasured");
    require(rows.at("other").totalBytes == 100, "unrelated completed row remains cached");
  }
}
void extractedDownloadUsesWholePackage() {
  TempDirectory temp;
  temp.write("attempt/extracted/chart.bms", "#TITLE Original\n");
  temp.write("attempt/extracted/nested/sabun.bme", "#TITLE Sabun\n");
  temp.write("attempt/extracted/nested/unused.mpg", "video");
  temp.write("attempt/extracted/other.wav", "unsupported audio");
  temp.write("attempt/extracted/banner.png", "image");
  std::string error;
  const auto chart = temp.root / "attempt/extracted/chart.bms";
  const auto before = HashStorageFile(chart, {}, error);
  auto report = asobmshow::bms_search::optimizeExtractedDownload(temp.root / "attempt/extracted",
      {.audio = AudioAction::LosslessFlac, .bga = BgaAction::Delete}, {}, {});
  require(report.deleted == 1 && report.failed + report.skipped >= 1, "nested unreferenced video optimized; unsupported audio retained");
  require(!report.recoveryRequired && !report.cleanupPending && !report.cancelled, "staging journals are settled before promotion");
  require(fs::exists(temp.root / "attempt/extracted/other.wav") && fs::exists(temp.root / "attempt/extracted/banner.png"), "failed audio and images retained");
  require(HashStorageFile(chart, {}, error) == before && fs::exists(temp.root / "attempt/extracted/nested/sabun.bme"), "all chart paths and content preserved");
  require(!fs::exists(temp.root / "attempt/extracted/storage-management"), "private journals stay outside promoted package");
}
void wholeFolderOrchestration() {
  TempDirectory temp;
  temp.write("charts/leader.bms", "#WAV01 lead.wav\n#BMP01 lead.mpg\n");
  temp.write("charts/sabun.pms", "#WAV01 nested/sabun.flac\n#BMP01 nested/sabun.avi\n");
  for (const auto &name : {"lead.wav", "lead.mpg", "nested/sabun.flac", "nested/sabun.avi", "extra.wav", "nested/unused.m1v"})
    temp.write(fs::path("charts") / name, std::string(100, 's'));
  temp.write("charts/banner.png", "image"); temp.write("charts/readme.txt", "readme");
  std::string error;
  const auto chartHash = HashStorageFile(temp.root / "charts/leader.bms", {}, error);
  const auto sabunHash = HashStorageFile(temp.root / "charts/sabun.pms", {}, error);
  std::vector<std::string> events;
  auto input = request(temp.root / "charts");
  input.inventory.groups.push_back({"nested", temp.root / "charts/nested"});
  input.inventory.groups.push_back({"duplicate", temp.root / "charts"});
  input.preflight = BuildStoragePreflight(input.inventory);
  auto deps = dependencies(temp, events);
  const auto report = StorageOptimizer(deps).Run(input, {}, {});
  require(report.converted == 6 && report.reclaimedBytes == 576 && !report.failed && !report.cancelled,
          "folder union converts representative, sabun, nested and unreferenced resources once");
  require(events.size() == 18, "each unique asset has one encode, verify and commit");
  for (const auto *name : {"lead.wav", "lead.mpg", "sabun.flac", "sabun.avi", "extra.wav", "unused.m1v"}) {
    const auto encoded = std::find(events.begin(), events.end(), std::string("encode:") + name);
    const auto verified = std::find(events.begin(), events.end(), std::string("verify:") + name);
    const auto committed = std::find(events.begin(), events.end(), std::string("commit:") + name);
    require(encoded < verified && verified < committed && committed != events.end(),
            "installation always follows encode and validation for that source");
  }
  require(HashStorageFile(temp.root / "charts/leader.bms", {}, error) == chartHash &&
              HashStorageFile(temp.root / "charts/sabun.pms", {}, error) == sabunHash,
          "chart identity is unchanged across folder-wide optimization");
  require(fs::file_size(temp.root / "charts/banner.png") == 5 && fs::file_size(temp.root / "charts/readme.txt") == 6,
          "images and non-media are preserved");
  require(report.changedRoots == std::vector<fs::path>{temp.root / "charts"}, "refresh roots are deduplicated unions");
  require(fs::is_empty(deps.paths.temporaryRoot), "finished job removes its private temporary directory");
}
void deletionAndUnavailableGroups() {
  TempDirectory temp;
  for (const auto &name : {"movie.mpg", "movie.mp4", "nested/unused.avi", "sound.wav", "movie.png", "nested/stage.bmp"})
    temp.write(fs::path("charts") / name, std::string(50, 'a'));
  std::vector<std::string> events;
  auto input = request(temp.root / "charts");
  input.policy.audio = AudioAction::Keep; input.policy.bga = BgaAction::Delete;
  input.inventory.policy = input.policy;
  input.inventory.groups.push_back({"archive", temp.root / "archive.zip", false});
  input.inventory.groups.push_back({"missing", temp.root / "missing"});
  input.preflight = BuildStoragePreflight(input.inventory);
  require(input.preflight->complete && input.preflight->eligibleCount == 3,
          "mixed selection review enables confirmation for eligible videos");
  require(input.preflight->EligibleRoots() == std::vector<fs::path>{temp.root / "charts"},
          "review admission excludes unavailable roots while retaining nested media");
  input.preflight->complete = false;
  const auto interrupted = StorageOptimizer(dependencies(temp, events)).Run(input);
  require(interrupted.failed == 1 && events.empty() && fs::exists(temp.root / "charts/movie.mpg") &&
              fs::exists(temp.root / "charts/nested/unused.avi"),
          "interrupted preflight cannot apply its partial eligible count");
  input.preflight->complete = true;
  const auto report = StorageOptimizer(dependencies(temp, events)).Run(input, {}, {});
  require(report.deleted == 3 && report.reclaimedBytes == 150 && events.empty(),
          "delete policy removes every video alternative without using encoders");
  require(fs::exists(temp.root / "charts/sound.wav") && fs::exists(temp.root / "charts/movie.png") &&
              fs::exists(temp.root / "charts/nested/stage.bmp"), "video deletion preserves all audio and images");
  bool unavailable = false;
  for (const auto &file : report.files)
    if (file.reason.code == StorageReason::UnsupportedSource) {
      unavailable = true; require(!file.bytesBefore && !file.bytesAfter, "unavailable archive sizes stay unknown");
    }
  require(unavailable, "unsupported group reports its extract/import reason");
}
void staleEvidenceNeverBecomesFreshByRescanning() {
  for (int change = 0; change < 4; ++change) {
    TempDirectory temp;
    temp.write("charts/a.wav", std::string(100, 'a')); temp.write("charts/b.wav", std::string(100, 'b'));
    temp.write("charts/chart.bms", "original chart");
    std::vector<std::string> events;
    auto input = request(temp.root / "charts");
    input.preflight = BuildStoragePreflight(input.inventory);
    auto deps = dependencies(temp, events);
    if (change == 0) temp.write("charts/b.wav", "external edit before run");
    else {
      auto factory = deps.transactions;
      deps.transactions = [&, factory](const StoragePlatformPaths &paths, StorageCommitPrecondition beforeCommit) {
        auto result = factory(paths, std::move(beforeCommit)); auto replace = result.replace;
        result.replace = [&, replace](const AssetCandidate &candidate, const EncodeResult &encoded, Checkpoint check) {
          auto committed = replace(candidate, encoded, check);
          if (candidate.source.filename() == "a.wav") {
            if (change == 1) temp.write("charts/b.wav", "external edit between commits");
            if (change == 2) temp.write("charts/chart.bms", "chart changed between commits");
            if (change == 3) temp.write("charts/b.flac", "new resolver alternative");
          }
          return committed;
        };
        return result;
      };
    }
    const auto report = StorageOptimizer(deps).Run(input, {}, {});
    require(report.converted == (change ? 1 : 0) && fs::exists(temp.root / "charts/b.wav") &&
                !fs::exists(temp.root / "charts/b.ogg"),
            "preflight refresh accounts only for owned mutations, never external edits or new collisions");
    require(std::find(events.begin(), events.end(), "encode:b.wav") == events.end(), "stale next input never reaches encoder");
    require(report.skipped || report.failed, "stale evidence is reported explicitly");
  }
}
void changedDirectoryIsSkippedWithoutAcceptingNewAssets() {
  TempDirectory temp;
  temp.write("charts/root.wav", std::string(100, 'r'));
  temp.write("charts/a/a.wav", std::string(100, 'a'));
  temp.write("charts/b/b.wav", std::string(100, 'b'));
  auto input = request(temp.root / "charts");
  input.preflight = BuildStoragePreflight(input.inventory);
  temp.write("charts/a/new.wav", std::string(100, 'n'));
  std::vector<std::string> events;
  const auto report = StorageOptimizer(dependencies(temp, events)).Run(std::move(input));
  require(report.converted == 2 && report.skipped && fs::exists(temp.root / "charts/a/a.wav") &&
      fs::exists(temp.root / "charts/a/new.wav") && !fs::exists(temp.root / "charts/b/b.wav"),
      "a stale directory stays skipped while an unchanged sibling can finish");
  require(report.groupDeltas.size() == 1 && report.groupDeltas.front().needsRefresh,
      "uncertain child changes invalidate the ancestor delta even after a committed parent change");
}
void unrelatedFoldersDoNotMultiplyCommitChecks() {
  TempDirectory temp;
  auto input = request(temp.root / "charts/active");
  temp.write("charts/active/a.wav", std::string(100, 'a'));
  for (int i = 0; i < 80; ++i) {
    const auto path = fs::path("charts") / ("other" + std::to_string(i));
    temp.write(path / "chart.bms", "chart");
    input.inventory.groups.push_back({std::to_string(i), temp.root / path});
  }
  input.preflight = BuildStoragePreflight(input.inventory);
  std::vector<std::string> events;
  auto deps = dependencies(temp, events);
  std::atomic<int> checkpoints = 0;
  int commitChecks = 0;
  const auto encoder = deps.encodeAudio;
  deps.encodeAudio = [&](const auto &asset, const auto &policy, const auto &output, auto check, auto progress) {
    auto encoded = encoder(asset, policy, output, check, progress);
    checkpoints = 0;
    return encoded;
  };
  const auto factory = deps.transactions;
  deps.transactions = [&, factory](const auto &paths, auto beforeCommit) {
    auto transaction = factory(paths, std::move(beforeCommit));
    auto replace = transaction.replace;
    transaction.replace = [&, replace](const auto &asset, const auto &encoded, auto check) {
      commitChecks = checkpoints;
      return replace(asset, encoded, check);
    };
    return transaction;
  };
  const auto report = StorageOptimizer(deps).Run(std::move(input), [&] { ++checkpoints; return true; });
  require(report.converted == 1 && commitChecks < 40,
      "unrelated selected chart folders do not add per-file commit checks");
}
void committedDeltasSurviveDetailLimitAndRetainedBackups() {
  TempDirectory temp;
  for (int i = 0; i < 105; ++i) temp.write(fs::path("charts") / (std::to_string(i) + ".wav"), std::string(100, 'a'));
  temp.write("charts/nested/other.wav", std::string(100, 'b'));
  auto input = request(temp.root / "charts");
  input.preflight = BuildStoragePreflight(input.inventory);
  input.measurementBaselines = input.preflight->groups;
  input.measurementBaselines.front().generation = 7;
  std::vector<std::string> events;
  StorageEvidenceMetrics metrics;
  auto deps = dependencies(temp, events); deps.evidenceMetrics = &metrics;
  const auto report = StorageOptimizer(deps).Run(std::move(input));
  require(report.converted == 106 && report.files.size() == 100 && report.groupDeltas.size() == 1,
      "every committed file contributes a delta beyond capped details");
  const auto &delta = report.groupDeltas.front();
  require(delta.bytesBefore == 10600 && delta.bytesAfter == 424 && delta.audioBefore == 10600 &&
      delta.audioAfter == 424 && delta.baselineBytes == 10600 && delta.baselineGeneration == 7 &&
      !delta.needsRefresh && !delta.nextSourceIdentity.empty(), "nested physical changes roll up exactly once");
  require(metrics.membershipEnumerations >= 106 && metrics.membershipEntries >= 105 * 105,
      "local membership checks remain instrumented rather than silently removed");
}
void ancestorChartsStayProtectedWithoutBlockingIndependentRoots() {
  TempDirectory temp;
  temp.write("charts/a/chart.bms", "original chart");
  temp.write("charts/a/nested/a.wav", std::string(100, 'a'));
  temp.write("charts/b/b.wav", std::string(100, 'b'));
  auto input = request(temp.root / "charts/a");
  input.inventory.groups.push_back({"independent", temp.root / "charts/b"});
  input.preflight = BuildStoragePreflight(input.inventory);
  std::vector<std::string> events;
  auto deps = dependencies(temp, events); const auto encode = deps.encodeAudio;
  deps.encodeAudio = [&, encode](const auto &asset, const auto &policy, const auto &output, auto check, auto progress) {
    auto result = encode(asset, policy, output, check, progress);
    if (asset.source.filename() == "a.wav") temp.write("charts/a/chart.bms", "external chart edit");
    return result;
  };
  const auto report = StorageOptimizer(deps).Run(std::move(input));
  require(report.converted == 1 && report.skipped == 1 && fs::exists(temp.root / "charts/a/nested/a.wav") &&
      !fs::exists(temp.root / "charts/b/b.wav"), "ancestor chart edits protect nested assets while unrelated roots proceed");
}
void descendantChartsStayProtectedWithoutBlockingIndependentRoots() {
  for (const bool duringEncode : {false, true}) {
    TempDirectory temp;
    temp.write("charts/a/shared.wav", std::string(100, 'a'));
    temp.write("charts/a/sub/nested/chart.bms", "#WAV01 ../../shared.wav\n");
    temp.write("charts/b/independent.wav", std::string(100, 'b'));
    auto input = request(temp.root / "charts/a");
    input.inventory.groups.push_back({"independent", temp.root / "charts/b"});
    input.preflight = BuildStoragePreflight(input.inventory);
    const auto parentModified = fs::last_write_time(temp.root / "charts/a");
    const auto editChart = [&] {
      temp.write("charts/a/sub/nested/chart.bms", "#WAV01 ../../shared.wav\n#TITLE External edit\n");
      require(fs::last_write_time(temp.root / "charts/a") == parentModified,
          "in-place descendant chart edits leave the candidate directory stamp unchanged");
    };
    if (!duringEncode) editChart();
    std::vector<std::string> events;
    auto deps = dependencies(temp, events); const auto encode = deps.encodeAudio;
    deps.encodeAudio = [&, encode](const auto &asset, const auto &policy, const auto &output, auto check, auto progress) {
      auto result = encode(asset, policy, output, check, progress);
      if (duringEncode && asset.source.filename() == "shared.wav") editChart();
      return result;
    };
    const auto report = StorageOptimizer(deps).Run(std::move(input));
    require(report.converted == 1 && report.skipped == 1 && fs::exists(temp.root / "charts/a/shared.wav") &&
        !fs::exists(temp.root / "charts/a/shared.ogg") && fs::exists(temp.root / "charts/b/independent.ogg"),
        "descendant chart edits after review or during encoding retain parent media while independent roots proceed");
  }
}
void descendantChartEvidenceSharesOneSpillBudget() {
  TempDirectory temp;
  temp.write("charts/shared.wav", std::string(100, 'a'));
  for (const auto &folder : {fs::path("charts/sub % one"), fs::path(u8"charts/서브")})
    for (const auto &name : {"a.bms", "b.bme", "c.pms"}) temp.write(folder / name, "#WAV01 ../shared.wav\n");
  const auto input = request(temp.root / "charts");
  auto preflight = BuildStoragePreflight(input.inventory);
  const auto scratch = temp.root / "scratch";
  StorageEvidenceMetrics metrics;
  std::string error;
  {
    StorageDirectoryEvidence evidence(preflight.directories.at(temp.root / "charts"), preflight,
        {.records = 4, .scratchRoot = scratch}, &metrics);
    require(evidence.Load({}, error), "descendant chart evidence loads with a small aggregate memory budget");
    require(std::distance(fs::directory_iterator(scratch), fs::directory_iterator{}) == 1,
        "charts across small directories share one spill file instead of separate unbounded stores");
    const auto entry = evidence.Candidates().Next({}, error);
    require(entry.has_value(), "shared parent source remains an eligible candidate");
    StorageDiagnostic reason;
    const auto candidate = StorageCandidateForEntry(*entry, evidence.Directory(), input.policy, {}, reason);
    require(candidate && evidence.Candidate(*candidate, {}, error) && evidence.Candidate(*candidate, {}, error),
        "spilled chart keys preserve distinct nested paths and Unicode names");
    require(metrics.chartChecks == 12 && metrics.membershipEnumerations == 0,
        "candidate checks read only captured chart identities without subtree membership enumeration");
    temp.write(fs::path(u8"charts/서브") / "b.bme", "#WAV01 ../shared.wav\n#TITLE Edited\n");
    require(!evidence.Candidate(*candidate, {}, error), "spilled descendant chart identities still reject later edits");
  }
  require(fs::is_empty(scratch), "chart evidence releases its spill file when the active directory finishes");
}
void partialFailureAndAccounting() {
  TempDirectory temp;
  for (const auto &name : {"a.wav", "b.wav", "c.wav"}) temp.write(fs::path("charts") / name, std::string(100, 'a'));
  std::vector<std::string> events;
  auto deps = dependencies(temp, events);
  const auto encode = deps.encodeAudio;
  deps.encodeAudio = [encode](const AssetCandidate &candidate, const StoragePolicy &policy, const fs::path &output,
                              Checkpoint check, ProgressCallback progress) {
    if (candidate.source.filename() == "a.wav") return EncodeResult{FileOutcome::Failed, {}, {StorageReason::ValidationFailed, "corrupt"}};
    if (candidate.source.filename() == "b.wav") return EncodeResult{FileOutcome::Skipped, {}, {StorageReason::NotSmaller, "caps not applied"}};
    return encode(candidate, policy, output, check, progress);
  };
  const auto report = StorageOptimizer(deps).Run(request(temp.root / "charts"), {}, {});
  require(report.failed == 1 && report.skipped == 1 && report.converted == 1 && report.reclaimedBytes == 96,
          "ordinary failure and non-smaller output do not stop later candidates or count attempted savings");
  for (const auto &file : report.files)
    if (file.outcome != FileOutcome::Converted) require(!file.bytesAfter && !file.reclaimedBytes, "uninstalled output has no installed-byte claim");
}
void revalidateAtTheActualCommitBoundary() {
  TempDirectory temp; temp.write("charts/a.wav", std::string(100, 'a'));
  std::vector<std::string> events;
  auto deps = dependencies(temp, events);
  deps.transactions = [&](const StoragePlatformPaths &paths, StorageCommitPrecondition beforeCommit) {
    auto ops = DefaultFileTransactionDependencies();
    ops.beforeCommit = std::move(beforeCommit);
    ops.faultPoint = [&](std::string_view point) {
      if (point == "prepared") temp.write("charts/a.mp3", "new fallback alternative during installation copy");
    };
    auto transaction = std::make_shared<StorageFileTransaction>(paths, ops);
    return OptimizerTransactions{
        .replace = [transaction](const AssetCandidate &asset, const EncodeResult &encoded, Checkpoint check) { return transaction->Replace(asset, encoded, check); },
        .erase = [transaction](const AssetCandidate &asset, Checkpoint check) { return transaction->Delete(asset, check); },
        .recover = [transaction](Checkpoint check, const StorageRecoveryObserver &observer) { return transaction->Recover(check, observer); }};
  };
  const auto report = StorageOptimizer(deps).Run(request(temp.root / "charts"), {}, {});
  require(!report.converted && !report.reclaimedBytes && fs::exists(temp.root / "charts/a.wav") && !fs::exists(temp.root / "charts/a.ogg"),
          "a resolver collision appearing during installation copy aborts before the durable commit");
}
void unsafeChartEvidenceRetainsAffectedScopes() {
  TempDirectory temp;
  temp.write("charts/linked/chart.bms", "#WAV01 sound.wav\n");
  temp.write("charts/linked/nested/sound.wav", std::string(100, 'a'));
  temp.write("charts/independent/sound.wav", std::string(100, 'b'));
  fs::create_hard_link(temp.root / "charts/linked/chart.bms", temp.root / "outside.bms");
  auto input = request(temp.root / "charts");
  input.preflight = BuildStoragePreflight(input.inventory);
  temp.write("outside.bms", "#WAV01 other.wav\n");
  std::vector<std::string> events;
  const auto report = StorageOptimizer(dependencies(temp, events)).Run(std::move(input));
  require(report.converted == 1 && !report.recoveryRequired &&
              fs::exists(temp.root / "charts/linked/nested/sound.wav") &&
              fs::exists(temp.root / "charts/independent/sound.ogg"),
          "unverifiable ancestor chart blocks its scope while independent media can convert");
}
void exceptionRecoveryClassification() {
  for (int stage = 0; stage != 3; ++stage) {
    TempDirectory temp;
    temp.write("charts/a.wav", std::string(100, 'a'));
    std::vector<std::string> events;
    auto deps = dependencies(temp, events);
    const auto factory = deps.transactions;
    std::atomic<bool> cancel = false;
    deps.transactions = [&](const StoragePlatformPaths &paths, StorageCommitPrecondition beforeCommit) {
      if (stage == 0 && beforeCommit) throw std::runtime_error("transaction setup failed");
      auto transactions = factory(paths, std::move(beforeCommit));
      if (stage == 1) transactions.recover = [](Checkpoint, const StorageRecoveryObserver &) -> StorageReport {
        throw std::runtime_error("recovery interrupted");
      };
      if (stage == 2) transactions.replace = [&](const AssetCandidate &, const EncodeResult &, Checkpoint check) -> FileResult {
        cancel = true; check();
        throw std::runtime_error("commit interrupted after cancellation");
      };
      return transactions;
    };
    const auto report = StorageOptimizer(deps).Run(request(temp.root / "charts"), [&] { return !cancel; });
    require(report.failed == 1 && report.recoveryRequired == (stage != 0),
            "only interrupted recovery or commit requires recovery; setup failure releases maintenance");
    require(!report.files.empty() && report.files.back().reason.code ==
                (stage == 0 ? StorageReason::IoError : StorageReason::RecoveryRequired),
            "cancellation cannot mask uncertain filesystem state");
    require(fs::exists(temp.root / "charts/a.wav"), "exception fixture retains its original source");
  }
}
void cleanupFailureAndFatalRecovery() {
  for (bool fatal : {false, true}) {
    TempDirectory temp;
    temp.write("charts/a.wav", std::string(100, 'a')); temp.write("charts/b.wav", std::string(100, 'b'));
    std::vector<std::string> events;
    auto deps = dependencies(temp, events);
    deps.transactions = [fatal](const StoragePlatformPaths &paths, StorageCommitPrecondition beforeCommit) {
      auto ops = DefaultFileTransactionDependencies();
      ops.beforeCommit = std::move(beforeCommit);
      if (!fatal) {
        const auto remove = ops.remove;
        ops.remove = [remove](const fs::path &path, std::string &error) {
          if (path.extension() == ".backup") { error = "retained backup"; return false; }
          return remove(path, error);
        };
      }
      auto transaction = std::make_shared<StorageFileTransaction>(paths, ops);
      OptimizerTransactions result;
      result.recover = [transaction](Checkpoint check, const StorageRecoveryObserver &observer) { return transaction->Recover(check, observer); };
      result.replace = [transaction, fatal](const AssetCandidate &candidate, const EncodeResult &encoded, Checkpoint check) {
        auto file = transaction->Replace(candidate, encoded, check);
        if (fatal) file.reason = {StorageReason::RecoveryRequired, "inconsistent retained state"};
        return file;
      };
      result.erase = [transaction](const AssetCandidate &candidate, Checkpoint check) { return transaction->Delete(candidate, check); };
      return result;
    };
    const auto report = StorageOptimizer(deps).Run(request(temp.root / "charts"), {}, {});
    if (fatal) require(report.recoveryRequired && report.converted == 1 && fs::exists(temp.root / "charts/b.wav"),
                       "recovery-required result stops further mutation");
    else {
      require(report.converted == 2 && !report.reclaimedBytes && !report.recoveryRequired && report.cleanupPending,
              "known retained backups are tracked while cleanup warnings claim no reclaimed bytes");
      require(report.groupDeltas.size() == 1 && report.groupDeltas[0].bytesBefore == 200 &&
          report.groupDeltas[0].bytesAfter == 208 && report.groupDeltas[0].audioAfter == 8,
          "retained recovery backups count as physical bytes rather than reclaimed media");
      const auto recovered = StorageFileTransaction(deps.paths).Recover();
      require(!recovered.recoveryRequired && recovered.reclaimedBytes == 192 &&
                  StorageFileTransaction(deps.paths).Recover().reclaimedBytes == 0,
              "later cleanup counts actual reclaimed bytes exactly once");
    }
  }
}
void reviewedCleanupAdvancesOnlyOwnedEvidence() {
  for (int edit = 0; edit != 7; ++edit) {
    TempDirectory temp;
    temp.write("charts/chart.bms", "#WAV01 song/b.wav\n");
    temp.write("charts/song/a.wav", std::string(100, 'a'));
    std::vector<std::string> events;
    auto deps = dependencies(temp, events);
    bool cleanup = false;
    deps.transactions = [&](const StoragePlatformPaths &paths, StorageCommitPrecondition beforeCommit) {
      auto ops = DefaultFileTransactionDependencies();
      ops.beforeCommit = std::move(beforeCommit);
      ops.faultPoint = [&](std::string_view point) {
        if (edit == 4 && !cleanup && point == "published")
          throw std::runtime_error("interrupted before retiring the source");
      };
      const auto remove = ops.remove;
      ops.remove = [&, remove](const fs::path &path, std::string &error) {
        if (edit == 5 && cleanup && path.extension() == ".json") { error = "journal cleanup failed"; return false; }
        if (path.extension() == ".backup") {
          if (!cleanup) { error = "retained backup"; return false; }
          if (edit == 2) temp.write("charts/chart.bms", "#WAV01 changed-during-recovery.wav\n");
          if (edit == 3) temp.write("charts/song/b.wav", "changed during recovery");
        }
        return remove(path, error);
      };
      const auto sync = ops.syncDirectory;
      ops.syncDirectory = [&, sync](const fs::path &path, std::string &error) {
        if (edit == 6 && cleanup && path == temp.root / "charts/song") { error = "backup removal flush failed"; return false; }
        return sync(path, error);
      };
      auto transaction = std::make_shared<StorageFileTransaction>(paths, ops);
      return OptimizerTransactions{
        .replace = [transaction](const AssetCandidate &asset, const EncodeResult &encoded, Checkpoint check) {
          return transaction->Replace(asset, encoded, check);
        },
        .erase = [transaction](const AssetCandidate &asset, Checkpoint check) { return transaction->Delete(asset, check); },
        .recover = [transaction](Checkpoint check, const StorageRecoveryObserver &observer) { return transaction->Recover(check, observer); },
      };
    };
    const auto first = StorageOptimizer(deps).Run(request(temp.root / "charts"));
    if (edit == 4) require(first.recoveryRequired && fs::exists(temp.root / "charts/song/a.ogg"),
                          "fixture retains an uncommitted output that hides its colliding source during review");
    else require(first.converted == 1 && first.cleanupPending && !first.recoveryRequired,
                 "fixture retains a committed backup for the next reviewed operation");
    temp.write("charts/song/b.wav", std::string(100, 'b'));
    auto input = request(temp.root / "charts");
    input.preflight = BuildStoragePreflight(input.inventory);
    require(input.preflight->eligibleCount == (edit == 4 ? 2 : 1),
            "review includes remaining media and potential reuse of an uncommitted destination");
    StorageMeasurementRows measurements;
    measurements.emplace("group", input.preflight->groups.front());
    const auto selections = input.inventory.groups;
    if (edit == 1) temp.write("charts/song/b.wav", "changed before recovery");
    cleanup = true;
    const auto report = StorageOptimizer(deps).Run(std::move(input));
    if (edit >= 5) require(report.recoveryRequired && !fs::is_empty(deps.paths.journalRoot),
                          "partial cleanup failure retains the journal and recovery requirement");
    else require(!report.recoveryRequired && fs::is_empty(deps.paths.journalRoot),
                 "review freshness never prevents owned journal cleanup");
    if (edit == 0) {
      require(report.converted == 1 && report.reclaimedBytes == 192 &&
                  !fs::exists(temp.root / "charts/song/b.wav") && fs::exists(temp.root / "charts/song/b.ogg"),
              "owned backup cleanup preserves reviewed authority for remaining media");
    } else if (edit == 4) {
      require(!report.converted && report.skipped && fs::exists(temp.root / "charts/song/a.wav") &&
                  fs::exists(temp.root / "charts/song/b.wav") && !fs::exists(temp.root / "charts/song/a.ogg"),
              "rollback cannot change reviewed destination reuse into a new encoding operation");
    } else if (edit < 5) {
      require(!report.converted && report.skipped && report.reclaimedBytes == 96 &&
                  fs::exists(temp.root / "charts/song/b.wav") && !fs::exists(temp.root / "charts/song/b.ogg"),
              "recovery does not adopt external media or ancestor chart edits into reviewed evidence");
    }
    ApplyStorageReportMeasurements(measurements, selections, report);
    require(!measurements.at("group").totalBytes,
            "recovery invalidates display totals that still include the removed backup");
  }
}
void cancellationCapabilitiesAndNoOp() {
  TempDirectory temp; temp.write("charts/a.wav", std::string(100, 'a'));
  std::vector<std::string> events; auto deps = dependencies(temp, events);
  auto input = request(temp.root / "charts");
  auto noOp = input; noOp.policy = {};
  require(StorageOptimizer(deps).Run(noOp, {}, {}).files.empty() && events.empty(), "no-op does not inventory or encode");
  auto report = StorageOptimizer(deps).Run(input, [] { return false; }, {});
  require(report.cancelled && events.empty(), "cancel before admission leaves every source alone");
  const auto probe = deps.probeCodecs;
  deps.probeCodecs = [probe] { auto caps = probe(); caps.vorbis.available = false; return caps; };
  report = StorageOptimizer(deps).Run(input, {}, {});
  require(report.skipped == 1 && report.files.front().reason.code == StorageReason::CodecUnavailable && events.empty(),
          "missing requested codec is reported without substituting an encoder");
  deps.probeCodecs = probe;
  deps.availableSpace = [](const fs::path &) { return std::optional<std::uint64_t>{0}; };
  report = StorageOptimizer(deps).Run(input, {}, {});
  require(report.skipped == 1 && report.files.front().reason.code == StorageReason::InsufficientSpace && events.empty(),
          "space is checked before encoding");
  deps = dependencies(temp, events);
  const auto encode = deps.encodeAudio; std::atomic<bool> cancel = false;
  deps.encodeAudio = [&, encode](const AssetCandidate &asset, const StoragePolicy &policy, const fs::path &path,
                                Checkpoint check, ProgressCallback progress) {
    auto encoded = encode(asset, policy, path, check, progress); cancel = true; return encoded;
  };
  report = StorageOptimizer(deps).Run(input, [&] { return !cancel; }, {});
  require(report.cancelled && !report.converted && fs::exists(temp.root / "charts/a.wav") && fs::is_empty(deps.paths.temporaryRoot),
          "cancel after verification stops before commit and removes only job-owned staging");
  deps = dependencies(temp, events);
  deps.transactions = [](const StoragePlatformPaths &, StorageCommitPrecondition) {
    OptimizerTransactions result;
    result.recover = [](Checkpoint, const StorageRecoveryObserver &) {
      StorageReport report; report.recoveryRequired = true; report.failed = 123;
      report.files.push_back({.outcome = FileOutcome::Failed, .reason = {StorageReason::RecoveryRequired, "bounded recovery details"}});
      return report;
    };
    return result;
  };
  report = StorageOptimizer(deps).Run(input, {}, {});
  require(report.recoveryRequired && report.failed == 123, "recovery failure totals survive a bounded detail list");
}
void boundedReportsAndProgress() {
  TempDirectory temp;
  for (int i = 0; i < 105; ++i) temp.write(fs::path("charts") / (std::to_string(i) + ".wav"), "media");
  std::vector<std::string> events; auto deps = dependencies(temp, events);
  deps.now = [] { return std::chrono::steady_clock::time_point{}; };
  deps.encodeAudio = [](const AssetCandidate &asset, const StoragePolicy &, const fs::path &, Checkpoint, ProgressCallback progress) {
    for (int i = 0; i < 100; ++i) progress({StoragePhase::Encode, asset.source});
    return EncodeResult{FileOutcome::Skipped, {}, {StorageReason::NotSmaller, "original retained"}};
  };
  int notifications = 0;
  const auto report = StorageOptimizer(deps).Run(request(temp.root / "charts"), {}, [&](const StorageProgress &) { ++notifications; });
  require(report.skipped == 105 && report.files.size() == 100, "bounded details retain full outcome counters");
  require(notifications <= 4, "preparation and inventory transitions do not unthrottle codec callbacks");
}
void existingDestinationsAreValidatedBeforeRetirement() {
  for (const auto operation : {AssetOperation::LosslessFlac, AssetOperation::LossyVorbis, AssetOperation::CompressH264}) {
    for (int mode = 0; mode < 3; ++mode) {
      TempDirectory temp;
      const auto sourceName = operation == AssetOperation::CompressH264 ? "clip.wmv" : "sound.wav";
      const auto destinationName = operation == AssetOperation::CompressH264 ? "clip.mp4" :
          operation == AssetOperation::LosslessFlac ? "sound.flac" : "sound.ogg";
      temp.write(fs::path("charts") / sourceName, "redundant source");
      temp.write(fs::path("charts") / destinationName, "existing media bytes retained verbatim");
      std::vector<std::string> events; auto deps = dependencies(temp, events);
      deps.probeCodecs = [] { return StorageCodecCapabilities{}; };
      deps.availableSpace = [](const auto &) { return std::optional<std::uint64_t>(0); };
      unsigned validations = 0;
      deps.validateExisting = [&](const AssetCandidate &asset, Checkpoint) {
        ++validations;
        require(asset.destination.filename() == destinationName, "validate the retained target, not the source");
        if (mode == 1) return StorageDiagnostic{StorageReason::ValidationFailed, "invalid destination"};
        if (mode == 2) temp.write(fs::path("charts") / destinationName, "external edit during validation");
        return StorageDiagnostic{};
      };
      auto input = request(temp.root / "charts");
      input.policy.audio = operation == AssetOperation::LosslessFlac ? AudioAction::LosslessFlac : AudioAction::LossyVorbis;
      const auto result = StorageOptimizer(deps).Run(input);
      require(validations == 1 && events.empty(), "reuse needs validation but no encoder or encoding scratch space");
      require(fs::exists(temp.root / "charts" / destinationName), "retained destination always survives");
      if (!mode) {
        require(result.deleted == 1 && result.reclaimedBytes == std::string("redundant source").size() &&
                    !fs::exists(temp.root / "charts" / sourceName), "valid existing destination retires only redundant source");
      } else require(!result.deleted && fs::exists(temp.root / "charts" / sourceName),
                     "invalid or changing destination preserves source");
    }
  }
}
void cancellableSingleConversionPermit() {
  StorageConversionPermit permit;
  auto held = permit.Acquire({}); require(held.has_value(), "first conversion obtains permit");
  std::mutex mutex; std::condition_variable changed;
  bool waiting = false, done = false, cancel = false, acquired = false;
  int checkpoints = 0;
  std::thread worker([&] {
    auto lease = permit.Acquire([&] {
      std::unique_lock lock(mutex);
      if (++checkpoints >= 2) { waiting = true; changed.notify_all(); changed.wait(lock, [&] { return cancel; }); }
      return !cancel;
    });
    std::lock_guard lock(mutex); acquired = lease.has_value(); done = true; changed.notify_all();
  });
  {
    std::unique_lock lock(mutex); changed.wait(lock, [&] { return waiting || done; });
    const bool exclusive = waiting && !done;
    cancel = true; changed.notify_all(); lock.unlock(); permit.Notify(); worker.join();
    require(exclusive && !acquired, "second conversion waits and can cancel without entering encoder");
  }
  held.reset(); require(permit.Acquire({}).has_value(), "released permit can be acquired again");
}
}
int main(int argc, char **argv) {
  try {
    if (argc == 5 && std::string_view(argv[1]) == "--archive-compaction-benchmark") {
      storageArchiveCompactionBenchmark(std::stoul(argv[2]), std::stoul(argv[3]), std::stoi(argv[4]) != 0); return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--archive-traversal-benchmark") {
      storageArchiveTraversalBenchmark(std::stoul(argv[2])); return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--directory-inventory") return storageDirectoryInventoryTests();
    if (argc > 1 && std::string_view(argv[1]) == "--archive") { storageArchiveTests(argv[0]); return 0; }
    if (argc > 1 && std::string_view(argv[1]) == "--archive-parallel") { storageArchiveParallelTests(); return 0; }
    if (argc == 3 && std::string_view(argv[1]) == "--archive-discovery-sandbox") {
      storageArchiveDiscoverySandboxTest(argv[2]); return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--commit-cost") { unrelatedFoldersDoNotMultiplyCommitChecks(); return 0; }
    if (argc > 1 && std::string_view(argv[1]) == "--subset-refresh") { subsetRefreshNeverEnumeratesAncestorSiblings(); return 0; }
    if (argc > 1 && std::string_view(argv[1]) == "--parallel") { growingOutputsRespectSharedScratch(); recoveryStopsAnAlreadyEncodedPeerBeforeCommit(); slowVideoDoesNotBlockFollowingFiles(); independentDirectoriesUseDistinctOutputs(); return 0; }
    if (argc > 1 && std::string_view(argv[1]) == "--sandbox-boundaries") { reviewedSelectionRetainsPrivateSandboxBoundary(); return 0; }
    unsafeChartEvidenceRetainsAffectedScopes(); exceptionRecoveryClassification();
    reviewedSelectionRetainsPrivateSandboxBoundary();
    losslessGrowthOffsetsSavingsAcrossDirectories();
    independentDirectoriesUseDistinctOutputs();
    growingOutputsRespectSharedScratch();
    slowVideoDoesNotBlockFollowingFiles();
    recoveryStopsAnAlreadyEncodedPeerBeforeCommit();
    storageLibraryIntegration();
    extractedDownloadUsesWholePackage(); wholeFolderOrchestration(); deletionAndUnavailableGroups(); staleEvidenceNeverBecomesFreshByRescanning();
    changedDirectoryIsSkippedWithoutAcceptingNewAssets(); unrelatedFoldersDoNotMultiplyCommitChecks();
    subsetRefreshNeverEnumeratesAncestorSiblings();
    ancestorChartsStayProtectedWithoutBlockingIndependentRoots(); descendantChartsStayProtectedWithoutBlockingIndependentRoots();
    descendantChartEvidenceSharesOneSpillBudget();
    committedDeltasSurviveDetailLimitAndRetainedBackups();
    partialFailureAndAccounting(); revalidateAtTheActualCommitBoundary(); cleanupFailureAndFatalRecovery();
    reviewedCleanupAdvancesOnlyOwnedEvidence(); cancellationCapabilitiesAndNoOp();
    boundedReportsAndProgress(); existingDestinationsAreValidatedBeforeRetirement(); cancellableSingleConversionPermit();
  } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
  return 0;
}
