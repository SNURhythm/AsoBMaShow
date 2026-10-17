#include "../src/storage/StorageOptimizer.h"
#include "StorageTestSupport.h"

#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

using namespace storage_management;
using storage_test::require;
using storage_test::TempDirectory;
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
  result.probeCodecs = [] {
    StorageCodecCapabilities caps;
    caps.flac.available = caps.vorbis.available = caps.x264.available = caps.aac.available = caps.mp4.available = true;
    return caps;
  };
  result.encodeAudio = result.encodeVideo = [&](const AssetCandidate &candidate, const StoragePolicy &,
      const fs::path &output, Checkpoint check, ProgressCallback progress) {
    require(!candidate.sourceHash.empty(), "source hash is captured before the encoder reads media");
    events.push_back("encode:" + candidate.source.filename().string());
    if (check && !check()) return EncodeResult{FileOutcome::Cancelled, {}, {StorageReason::Cancelled, "cancel"}};
    std::ofstream file(output, std::ios::binary); file << "tiny"; file.close();
    if (progress) progress({StoragePhase::Verify, candidate.source});
    events.push_back("verify:" + candidate.source.filename().string());
    return EncodeResult{FileOutcome::Converted, output, {}};
  };
  const auto factory = result.transactions;
  result.transactions = [&, factory](const StoragePlatformPaths &paths, StorageCommitPrecondition beforeCommit) {
    auto transactions = factory(paths, std::move(beforeCommit));
    const auto replace = transactions.replace;
    transactions.replace = [&, replace](const AssetCandidate &candidate, const EncodeResult &encoded, Checkpoint check) {
      events.push_back("commit:" + candidate.source.filename().string());
      require(fs::exists(encoded.outputPath), "validated temporary output reaches transaction intact");
      return replace(candidate, encoded, check);
    };
    return transactions;
  };
  return result;
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
  input.preflight = BuildInventory(input.inventory);
  auto deps = dependencies(temp, events);
  const auto report = StorageOptimizer(deps).Run(input, {}, {});
  require(report.converted == 6 && report.reclaimedBytes == 576 && !report.failed && !report.cancelled,
          "folder union converts representative, sabun, nested and unreferenced resources once");
  require(events.size() == 18, "each unique asset has one encode, verify and commit");
  for (std::size_t i = 0; i < events.size(); i += 3) {
    require(events[i].starts_with("encode:") && events[i + 1].starts_with("verify:") && events[i + 2].starts_with("commit:") &&
                events[i].substr(7) == events[i + 1].substr(7) && events[i].substr(7) == events[i + 2].substr(7),
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
    input.preflight = BuildInventory(input.inventory);
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
        .recover = [transaction](Checkpoint check) { return transaction->Recover(check); }};
  };
  const auto report = StorageOptimizer(deps).Run(request(temp.root / "charts"), {}, {});
  require(!report.converted && !report.reclaimedBytes && fs::exists(temp.root / "charts/a.wav") && !fs::exists(temp.root / "charts/a.ogg"),
          "a resolver collision appearing during installation copy aborts before the durable commit");
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
      result.recover = [transaction](Checkpoint check) { return transaction->Recover(check); };
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
      const auto recovered = StorageFileTransaction(deps.paths).Recover();
      require(!recovered.recoveryRequired && recovered.reclaimedBytes == 192 &&
                  StorageFileTransaction(deps.paths).Recover().reclaimedBytes == 0,
              "later cleanup counts actual reclaimed bytes exactly once");
    }
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
  const auto encode = deps.encodeAudio; bool cancel = false;
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
    result.recover = [](Checkpoint) {
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
  require(notifications <= 3, "progress notifications are throttled independently of codec callback count");
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
int main() {
  try {
    wholeFolderOrchestration(); deletionAndUnavailableGroups(); staleEvidenceNeverBecomesFreshByRescanning();
    partialFailureAndAccounting(); revalidateAtTheActualCommitBoundary(); cleanupFailureAndFatalRecovery(); cancellationCapabilitiesAndNoOp();
    boundedReportsAndProgress(); cancellableSingleConversionPermit();
  } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
  return 0;
}
