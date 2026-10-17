#include "../src/storage/StorageFileTransaction.h"
#include "StorageTestSupport.h"
#include <nlohmann/json.hpp>

#include <iostream>
#include <iterator>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <sys/wait.h>
#include <unistd.h>
#endif
#endif

using namespace storage_management;
using storage_test::require;
namespace fs = std::filesystem;
namespace {
struct Crash {};
struct Fixture {
  storage_test::TempDirectory temp;
  StoragePlatformPaths paths;
  AssetCandidate candidate;
  EncodeResult encoded;
  Fixture(bool samePath = false, bool deletion = false) {
    temp.write("library/asset.mpg", std::string(1024, 's'));
    temp.write("private/encoded.mp4", std::string(128, 'v'));
    paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root / "library"});
    candidate.source = temp.root / "library/asset.mpg";
    candidate.destination = samePath ? candidate.source : temp.root / "library/asset.mp4";
    if (samePath) {
      fs::rename(candidate.source, temp.root / "library/asset.mp4");
      candidate.source = candidate.destination = temp.root / "library/asset.mp4";
    }
    candidate.kind = asobmshow::chart_assets::MediaKind::Video;
    candidate.operation = deletion ? AssetOperation::Delete : AssetOperation::CompressH264;
    std::string error;
    candidate.identity = *ReadFileIdentity(candidate.source, error);
    encoded.outcome = FileOutcome::Converted;
    encoded.outputPath = temp.root / "private/encoded.mp4";
  }
};
std::string read(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}
void requireNoRecoveryFiles(const Fixture &f) {
  for (const auto &entry : fs::directory_iterator(f.paths.journalRoot))
    require(!entry.is_regular_file(), "completed transactions remove their journal");
  for (const auto &entry : fs::directory_iterator(f.candidate.source.parent_path()))
    require(!entry.path().filename().string().starts_with(".asobmashow-storage-"),
            "completed transactions remove sibling installation files and backups");
}
#if defined(__APPLE__) && TARGET_OS_OSX
void testSandboxedRecovery(const char *executable) {
  storage_test::TempDirectory temp;
  fs::create_directories(temp.root / "system/app/private");
  fs::create_directories(temp.root / "system/app/library");
  const auto profile = "(version 1) (allow default) (deny file-read-metadata (literal \"" +
      (temp.root / "system").string() + "\"))";
  const auto child = fork();
  require(child >= 0, "start sandbox recovery subprocess");
  if (!child) {
    execl("/usr/bin/sandbox-exec", "sandbox-exec", "-p", profile.c_str(), executable,
          "--sandbox-recovery", (temp.root / "system/app").c_str(), nullptr);
    _exit(127);
  }
  int status = 0;
  require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "startup recovery works when system ancestors deny metadata access");
}
#endif
void testAbandonedCandidatesAfterRestart() {
  constexpr auto job = "12345678-1234-4234-8234-123456789abc";
  for (const bool journal : {false, true}) {
    Fixture f;
    const auto candidate = f.paths.temporaryRoot / job / "candidate.mp4";
    f.temp.write(fs::relative(candidate, f.temp.root), "abandoned encoded data");
    f.temp.write(fs::relative(f.paths.temporaryRoot / "unknown/data", f.temp.root), "foreign data");
    if (journal) {
      FileTransactionDependencies dependencies;
      dependencies.faultPoint = [](std::string_view point) { if (point == "published") throw Crash{}; };
      try { StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded); }
      catch (const Crash &) {}
    }
    const auto report = StorageFileTransaction(f.paths).Recover();
    require(!report.recoveryRequired && !report.cleanupPending && report.reclaimedBytes == 0,
            "abandoned scratch cleanup is not counted as reclaimed library media");
    require(!fs::exists(candidate.parent_path()), "restart removes abandoned private candidate jobs");
    require(read(f.candidate.source) == std::string(1024, 's') &&
                read(f.paths.temporaryRoot / "unknown/data") == "foreign data",
            "restart preserves original media and unrecognized private entries");
  }
  Fixture failed;
  const auto candidate = failed.paths.temporaryRoot / job / "candidate.flac";
  failed.temp.write(fs::relative(candidate, failed.temp.root), "unfinished encode");
  FileTransactionDependencies dependencies;
  dependencies.remove = [](const fs::path &, std::string &error) { error = "injected cleanup failure"; return false; };
  const auto report = StorageFileTransaction(failed.paths, dependencies).Recover();
  require(report.recoveryRequired && report.cleanupPending && fs::exists(candidate),
          "failed candidate cleanup is retained and reported for retry");
  require(!StorageFileTransaction(failed.paths).Recover().recoveryRequired && !fs::exists(candidate),
          "next recovery retries abandoned candidate cleanup");

  Fixture unsettled;
  unsettled.temp.write(fs::relative(unsettled.paths.temporaryRoot / job / "candidate.mp4", unsettled.temp.root), "retained");
  unsettled.temp.write(fs::relative(unsettled.paths.journalRoot / "invalid.json", unsettled.temp.root), "invalid");
  require(StorageFileTransaction(unsettled.paths).Recover().recoveryRequired &&
              fs::exists(unsettled.paths.temporaryRoot / job / "candidate.mp4"),
          "unresolved journals retain candidate state");

  Fixture cancelled;
  cancelled.temp.write(fs::relative(cancelled.paths.temporaryRoot / job / "candidate.ogg", cancelled.temp.root), "retained");
  require(StorageFileTransaction(cancelled.paths).Recover([] { return false; }).cancelled &&
              fs::exists(cancelled.paths.temporaryRoot / job / "candidate.ogg"),
          "cancellation leaves abandoned candidate available for later cleanup");

  Fixture unknown;
  unknown.temp.write(fs::relative(unknown.paths.temporaryRoot / job / "candidate.mp4", unknown.temp.root), "candidate");
  unknown.temp.write(fs::relative(unknown.paths.temporaryRoot / job / "unknown", unknown.temp.root), "foreign");
  StorageFileTransaction(unknown.paths).Recover();
  require(fs::exists(unknown.paths.temporaryRoot / job / "candidate.mp4") &&
              read(unknown.paths.temporaryRoot / job / "unknown") == "foreign",
          "unrecognized job contents prevent partial cleanup");

  Fixture linked;
  linked.temp.write("foreign/candidate.mp4", "never follow links");
  fs::create_directories(linked.paths.temporaryRoot);
  std::error_code ec;
  fs::create_directory_symlink(linked.temp.root / "foreign", linked.paths.temporaryRoot / job, ec);
  if (!ec) {
    StorageFileTransaction(linked.paths).Recover();
    require(read(linked.temp.root / "foreign/candidate.mp4") == "never follow links" &&
                fs::is_symlink(fs::symlink_status(linked.paths.temporaryRoot / job)),
            "abandoned cleanup never follows or removes unrecognized symlinks");
  }
}
void testSuccessAndCancellation() {
  for (const bool samePath : {false, true}) {
    Fixture f(samePath);
    StorageFileTransaction transaction(f.paths, {});
    const auto result = transaction.Replace(f.candidate, f.encoded, {});
    require(result.outcome == FileOutcome::Converted && result.reclaimedBytes == 896,
            "successful replacement accounts for actual removed source bytes");
    require(read(f.candidate.destination) == std::string(128, 'v'), "validated output installed");
    if (!samePath) require(!fs::exists(f.candidate.source), "old extension retired");
    requireNoRecoveryFiles(f);
  }
  Fixture f(false, true);
  StorageFileTransaction transaction(f.paths, {});
  const auto cancelled = transaction.Delete(f.candidate, [] { return false; });
  require(cancelled.outcome == FileOutcome::Cancelled && fs::exists(f.candidate.source),
          "cancellation before commit retains source");
  int checkpoints = 0;
  const auto hashingCancelled = transaction.Delete(f.candidate, [&] { return ++checkpoints == 1; });
  require(hashingCancelled.outcome == FileOutcome::Cancelled && fs::exists(f.candidate.source),
          "cancellation during source verification is reported as cancellation");
  const auto deleted = transaction.Delete(f.candidate, {});
  require(deleted.outcome == FileOutcome::Deleted && deleted.reclaimedBytes == 1024 &&
              !fs::exists(f.candidate.source), "deletion reclaims bytes after unlink");
  requireNoRecoveryFiles(f);
}
void testRestartAtEveryBoundary() {
  for (const bool samePath : {false, true}) {
    for (const auto point : {"copying", "copied", "prepared", "published-before-journal",
                             "published", "retired-before-journal", "retired", "committed"}) {
      Fixture f(samePath);
      FileTransactionDependencies dependencies;
      dependencies.faultPoint = [point](std::string_view reached) { if (reached == point) throw Crash{}; };
      try {
        StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded, {});
        require(false, "injected crash point reached");
      } catch (const Crash &) {}
      const auto recovered = StorageFileTransaction(f.paths, {}).Recover({});
      require(!recovered.recoveryRequired, "restart resolves valid journal");
      if (std::string_view(point) == "committed") {
        require(read(f.candidate.destination) == std::string(128, 'v'), "committed replacement survives restart");
        require(recovered.reclaimedBytes == 896, "recovery counts bytes only after backup cleanup");
      } else {
        require(read(f.candidate.source) == std::string(1024, 's'), "uncommitted replacement rolls back");
        if (!samePath) require(!fs::exists(f.candidate.destination), "uncommitted destination removed");
      }
      requireNoRecoveryFiles(f);
    }
  }
  for (const auto point : {"prepared", "retired", "committed"}) {
    Fixture f(false, true);
    FileTransactionDependencies dependencies;
    dependencies.faultPoint = [point](std::string_view reached) { if (reached == point) throw Crash{}; };
    try { StorageFileTransaction(f.paths, dependencies).Delete(f.candidate, {}); }
    catch (const Crash &) {}
    const auto recovered = StorageFileTransaction(f.paths, {}).Recover({});
    require(!recovered.recoveryRequired, "deletion journal recovers");
    require(fs::exists(f.candidate.source) != (std::string_view(point) == "committed"),
            "only committed deletions survive restart");
    requireNoRecoveryFiles(f);
  }
}
void testFailuresNeverClobberAndCleanupAccounting() {
  {
    Fixture f;
    FileTransactionDependencies dependencies;
    dependencies.faultPoint = [&](std::string_view point) {
      if (point == "committed") f.temp.write("library/asset.mp4", "unexpected output edit");
    };
    const auto result = StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded, {});
    require(result.reason.code == StorageReason::RecoveryRequired && result.reclaimedBytes == 0,
            "changed output before backup cleanup requires recovery, not an ordinary warning");
  }
  for (const bool samePath : {false, true}) {
    Fixture f(samePath);
    auto dependencies = DefaultFileTransactionDependencies();
    const auto rename = dependencies.renameNoReplace;
    dependencies.renameNoReplace = [rename, &f](const fs::path &from, const fs::path &to, std::string &error) {
      if (from.extension() == ".install") { error = "injected rename failure"; return false; }
      return rename(from, to, error);
    };
    const auto result = StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded, {});
    require(result.outcome == FileOutcome::Failed && read(f.candidate.source) == std::string(1024, 's'),
            "failed installation rename restores the original for either path strategy");
  }
  {
    Fixture f;
    f.temp.write("private/foreign", "unrelated file must remain intact");
    FileTransactionDependencies dependencies;
    dependencies.faultPoint = [&](std::string_view point) {
      if (point != "copying") return;
      for (const auto &entry : fs::directory_iterator(f.candidate.source.parent_path())) {
        if (entry.path().extension() != ".install") continue;
        fs::remove(entry.path());
        fs::create_symlink(f.temp.root / "private/foreign", entry.path());
      }
    };
    const auto result = StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded, {});
    require(result.outcome == FileOutcome::Failed &&
                read(f.temp.root / "private/foreign") == "unrelated file must remain intact",
            "installation copy never follows a replaced staging symlink");
  }
  {
    Fixture f;
    FileTransactionDependencies dependencies;
    dependencies.faultPoint = [&](std::string_view point) {
      if (point == "published") f.temp.write("library/asset.mpg", "edit after publication");
    };
    const auto result = StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded, {});
    require(result.outcome == FileOutcome::Failed &&
                read(f.candidate.source) == "edit after publication",
            "source is rechecked immediately before retirement");
  }
  for (const bool deletion : {false, true}) {
    Fixture f(false, deletion);
    bool cancel = false;
    FileTransactionDependencies dependencies;
    dependencies.faultPoint = [&](std::string_view point) { if (point == "retired") cancel = true; };
    StorageFileTransaction transaction(f.paths, dependencies);
    const auto result = deletion ? transaction.Delete(f.candidate, [&] { return !cancel; }) :
        transaction.Replace(f.candidate, f.encoded, [&] { return !cancel; });
    require(result.outcome == FileOutcome::Cancelled && read(f.candidate.source) == std::string(1024, 's'),
            "cancellation after retirement but before commitment restores source");
    requireNoRecoveryFiles(f);
  }
  {
    Fixture f;
    FileTransactionDependencies dependencies;
    dependencies.faultPoint = [&](std::string_view point) {
      if (point == "prepared") f.temp.write("library/asset.mp4", "external destination");
    };
    const auto result = StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded, {});
    require(result.outcome == FileOutcome::Failed && result.reclaimedBytes == 0 &&
                read(f.candidate.source) == std::string(1024, 's') &&
                read(f.candidate.destination) == "external destination",
            "a destination appearing at commit is never overwritten or removed");
  }
  {
    Fixture f;
    FileTransactionDependencies dependencies;
    dependencies.faultPoint = [&](std::string_view point) {
      if (point == "prepared") f.temp.write("library/asset.mpg", "external edit");
    };
    const auto result = StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded, {});
    require(result.outcome == FileOutcome::Failed && read(f.candidate.source) == "external edit",
            "source changed before commit is preserved");
  }
  {
    Fixture f;
    FileTransactionDependencies dependencies;
    dependencies.copyAndSync = [](const fs::path &, const fs::path &, const FileIdentity &, Checkpoint, std::string &error) {
      error = "disk full"; return false;
    };
    const auto result = StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded, {});
    require(result.outcome == FileOutcome::Failed && fs::exists(f.candidate.source),
            "copy/disk-full failure leaves original intact");
  }
  {
    Fixture f(true);
    auto dependencies = DefaultFileTransactionDependencies();
    const auto remove = dependencies.remove;
    dependencies.remove = [remove](const fs::path &path, std::string &error) {
      if (path.extension() == ".backup") { error = "busy backup"; return false; }
      return remove(path, error);
    };
    const auto result = StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded, {});
    require(result.outcome == FileOutcome::Converted && result.reclaimedBytes == 0 &&
                result.reason.code == StorageReason::CleanupFailed,
            "retained original backup is not reported as reclaimed space");
    const auto recovered = StorageFileTransaction(f.paths, {}).Recover({});
    require(!recovered.recoveryRequired && recovered.reclaimedBytes == 896,
            "later recovery accounts for successful cleanup");
    requireNoRecoveryFiles(f);
  }
}
void testForeignIdentityAndCorruptJournalArePreserved() {
  Fixture f(true);
  FileTransactionDependencies dependencies;
  dependencies.faultPoint = [](std::string_view point) { if (point == "committed") throw Crash{}; };
  try { StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded, {}); }
  catch (const Crash &) {}
  for (const auto &entry : fs::directory_iterator(f.candidate.source.parent_path())) {
    if (entry.path().extension() == ".backup") {
      fs::remove(entry.path());
      std::ofstream(entry.path()) << "unrelated replacement file";
    }
  }
  const auto recovered = StorageFileTransaction(f.paths, {}).Recover({});
  require(recovered.recoveryRequired, "foreign backup identity blocks cleanup");
  bool foundForeign = false;
  for (const auto &entry : fs::directory_iterator(f.candidate.source.parent_path()))
    if (entry.path().extension() == ".backup") foundForeign = read(entry.path()) == "unrelated replacement file";
  require(foundForeign, "recovery never deletes an unrelated file at a recorded path");
  std::ofstream(f.paths.journalRoot / "corrupt.json") << "not JSON";
  require(StorageFileTransaction(f.paths, {}).Recover({}).recoveryRequired,
          "corrupt records remain visible as recovery-required");
  require(fs::exists(f.paths.journalRoot / "corrupt.json"), "corrupt record preserved");

  Fixture outside;
  dependencies.faultPoint = [](std::string_view point) { if (point == "prepared") throw Crash{}; };
  try { StorageFileTransaction(outside.paths, dependencies).Replace(outside.candidate, outside.encoded, {}); }
  catch (const Crash &) {}
  for (const auto &entry : fs::directory_iterator(outside.paths.journalRoot)) {
    if (entry.path().extension() != ".json") continue;
    nlohmann::json record;
    { std::ifstream input(entry.path()); input >> record; }
    record["source"] = (outside.temp.root / "private/encoded.mp4").string();
    std::ofstream(entry.path()) << record;
  }
  require(StorageFileTransaction(outside.paths, {}).Recover({}).recoveryRequired &&
              read(outside.encoded.outputPath) == std::string(128, 'v'),
          "out-of-authorized-root journal cannot mutate its target");

  Fixture malformed;
  dependencies.faultPoint = [](std::string_view point) { if (point == "committed") throw Crash{}; };
  try { StorageFileTransaction(malformed.paths, dependencies).Replace(malformed.candidate, malformed.encoded, {}); }
  catch (const Crash &) {}
  for (const auto &entry : fs::directory_iterator(malformed.paths.journalRoot)) {
    if (entry.path().extension() != ".json") continue;
    nlohmann::json record;
    { std::ifstream input(entry.path()); input >> record; }
    record["phase"] = 4.5;
    std::ofstream(entry.path()) << record;
  }
  require(StorageFileTransaction(malformed.paths, {}).Recover({}).recoveryRequired,
          "malformed journal state must not be coerced into a committed decision");
}
}
int main(int argc, char **argv) {
  try {
#if defined(__APPLE__) && TARGET_OS_OSX
    if (argc == 3 && std::string_view(argv[1]) == "--sandbox-recovery") {
      const fs::path root = argv[2];
      std::error_code ec;
      fs::symlink_status(root.parent_path(), ec);
      require(ec == std::errc::operation_not_permitted || ec == std::errc::permission_denied,
              "fixture denies inspection of the system ancestor");
      const auto paths = MakeStoragePlatformPaths(root / "private", {root / "library"}, true);
      const auto recovered = StorageFileTransaction(paths).Recover();
      require(!recovered.recoveryRequired, "fresh writable app storage is not an interrupted operation");
      std::ofstream(root / "library/asset.mpg") << std::string(128, 'v');
      InventoryRequest request;
      request.groups = {{"sandbox", root / "library"}};
      request.policy.bga = BgaAction::Delete;
      request.pathAccessRoots = paths.pathAccessRoots;
      const auto inventory = BuildInventory(request);
      std::string error;
      require(inventory.complete && inventory.candidates.size() == 1 && RevalidateInventory(inventory, {}, error),
              "inventory and revalidation stay within the granted subtree");
      FileTransactionDependencies dependencies;
      dependencies.faultPoint = [](std::string_view point) { if (point == "retired") throw Crash{}; };
      try { StorageFileTransaction(paths, dependencies).Delete(inventory.candidates.front()); }
      catch (const Crash &) {}
      require(!StorageFileTransaction(paths).Recover().recoveryRequired &&
                  read(root / "library/asset.mpg") == std::string(128, 'v'),
              "interrupted mutation recovers beneath inaccessible system parents");
      fs::create_directories(root / "outside");
      std::ofstream(root / "outside/keep.mpg") << "outside";
      fs::create_directory_symlink(root / "outside", root / "library/linked");
      require(!IsSafeLocalPath(root / "library/linked/keep.mpg", error, paths.pathAccessRoots),
              "anchoring never permits symlinks beneath authorized folders");
      require(!IsSafeLocalPath(root / "outside/keep.mpg", error, paths.pathAccessRoots),
              "anchoring does not authorize unrelated subtrees");
      fs::remove(paths.journalRoot);
      fs::create_directory_symlink(root / "outside", paths.journalRoot);
      const auto unsafe = StorageFileTransaction(paths).Recover();
      require(unsafe.recoveryRequired && !unsafe.files.empty() &&
                  unsafe.files.front().source == paths.journalRoot &&
                  unsafe.files.front().reason.detail.find("Symlink") != std::string::npos &&
                  read(root / "outside/keep.mpg") == "outside",
              "unsafe private journal retains files and reports the failing path");
      return 0;
    }
    testSandboxedRecovery(argv[0]);
#endif
    testAbandonedCandidatesAfterRestart();
    testSuccessAndCancellation();
    testRestartAtEveryBoundary();
    testFailuresNeverClobberAndCleanupAccounting();
    testForeignIdentityAndCorruptJournalArePreserved();
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
  }
}
