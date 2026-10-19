#include "../src/storage/StorageFileTransaction.h"
#include "StorageTestSupport.h"
#include <nlohmann/json.hpp>

#include <iostream>
#include <algorithm>
#include <iterator>
#include <array>
#include <condition_variable>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#endif
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
void testAbandonedScanCleanup() {
  Fixture fixture;
  const auto root = fixture.paths.scanRoot;
  const std::string id = "12345678-1234-4234-8234-123456789abc";
  for (const auto &[prefix, database] : std::vector<std::pair<std::string, std::string>>{
      {"", "review.sqlite"}, {"storage-directory-", "directory.sqlite"}, {"storage-scan-", "frontier.sqlite"}}) {
    const auto directory = root / (prefix + id);
    fixture.temp.write(directory / database, "abandoned database");
    fixture.temp.write(directory / (database + "-journal"), "abandoned journal");
  }
  const auto unknown = root / "storage-directory-old";
  fixture.temp.write(unknown / "directory.sqlite", "unknown directory");
  const auto mixed = root / "12345678-1234-4234-8234-123456789abd";
  fixture.temp.write(mixed / "review.sqlite", "recognized database");
  fixture.temp.write(mixed / "keep.txt", "unrelated file");
  const auto linked = root / "12345678-1234-4234-8234-123456789abe";
  fs::create_directory(linked);
  std::error_code linkError;
  fs::create_symlink(fixture.candidate.source, linked / "review.sqlite", linkError);
  const auto hardlinked = root / "12345678-1234-4234-8234-123456789abf";
  fs::create_directory(hardlinked);
  fs::create_hard_link(fixture.candidate.source, hardlinked / "review.sqlite");
  fixture.temp.write(fixture.paths.journalRoot / ("archive-" + id) / "manifest.sqlite", "recovery evidence");
  std::string error;
  require(!CleanupAbandonedStorageScans(fixture.paths, error, [] { return false; }) &&
      fs::exists(root / id / "review.sqlite"), "quit preserves scratch for a later startup");
  require(CleanupAbandonedStorageScans(fixture.paths, error), "startup sweeps abandoned scan databases");
  require(!fs::exists(root / id) && !fs::exists(root / ("storage-directory-" + id)) &&
      !fs::exists(root / ("storage-scan-" + id)), "all three owned scan formats are reclaimed");
  require(fs::exists(unknown / "directory.sqlite") && fs::exists(mixed / "review.sqlite") &&
      (linkError || fs::is_symlink(linked / "review.sqlite")) && fs::exists(hardlinked / "review.sqlite") &&
      read(fixture.candidate.source) == std::string(1024, 's') &&
      read(fixture.paths.journalRoot / ("archive-" + id) / "manifest.sqlite") == "recovery evidence",
      "unknown files, links, source media and durable recovery evidence are preserved");
  require(CleanupAbandonedStorageScans(fixture.paths, error), "scratch cleanup is idempotent");
  fixture.paths.scanRoot = linkError ? fixture.candidate.source : linked / "review.sqlite";
  require(!CleanupAbandonedStorageScans(fixture.paths, error), "unsafe scratch root is rejected");
}
void testReservationIncludesPendingRecoveryRoots() {
  Fixture fixture;
  const auto selected = fixture.temp.root / "other-library";
  fs::create_directory(selected);
  const auto archive = fixture.temp.root / "registered.zip";
  fixture.temp.write("registered.zip", "registered archive");
  fixture.paths.allowedRoots.push_back(selected);
  fixture.paths.allowedRoots.push_back(archive);
  fixture.paths.allowedRoots.push_back(fixture.temp.root / "missing-library");
  std::string error;
  require(StorageReservationRoots(fixture.paths, {selected}, error) == std::vector<fs::path>{selected},
          "absent journals reserve only the requested folder");
  fs::create_directories(fixture.paths.journalRoot);
  require(StorageReservationRoots(fixture.paths, {selected}, error) == std::vector<fs::path>{selected},
          "empty journals do not block writers in unrelated folders");
  auto dependencies = DefaultFileTransactionDependencies();
  const auto remove = dependencies.remove;
  dependencies.remove = [remove](const fs::path &path, std::string &error) {
    if (path.extension() == ".backup") { error = "Injected cleanup failure"; return false; }
    return remove(path, error);
  };
  const auto result = StorageFileTransaction(fixture.paths, dependencies).Replace(fixture.candidate, fixture.encoded);
  require(result.outcome == FileOutcome::Converted && result.reason.code == StorageReason::CleanupFailed,
          "committed cleanup-only recovery can remain after maintenance admission reopens");
  auto roots = StorageReservationRoots(fixture.paths, {selected}, error);
  require(roots.has_value(), "pending cleanup roots can be inspected");
  std::ranges::sort(*roots);
  roots->erase(std::unique(roots->begin(), roots->end()), roots->end());
  require(*roots == std::vector<fs::path>{fixture.temp.root / "library", selected, archive},
          "new folder job reserves all existing registered recovery roots including archives");
  fixture.temp.write("not-a-directory", "unreadable journal inventory");
  auto invalid = fixture.paths;
  invalid.journalRoot = fixture.temp.root / "not-a-directory";
  require(!StorageReservationRoots(invalid, {selected}, error) && !error.empty(),
          "journal inspection errors reject maintenance rather than omit recovery reservations");
#ifndef _WIN32
  fs::create_symlink("loop", fixture.temp.root / "loop");
  invalid = fixture.paths;
  invalid.allowedRoots.push_back(fixture.temp.root / "loop");
  require(!StorageReservationRoots(invalid, {selected}, error) && !error.empty(),
          "registered root stat errors reject recovery admission");
#endif
}
void testConcurrentCommitAndCancellation() {
  storage_test::TempDirectory temp;
  const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root / "library"});
  std::array<AssetCandidate, 2> candidates;
  std::array<EncodeResult, 2> encoded;
  for (std::size_t i = 0; i < 2; ++i) {
    const auto folder = "library/" + std::to_string(i);
    temp.write(folder + "/asset.mpg", std::string(1024, 's'));
    temp.write("private/output" + std::to_string(i) + ".mp4", std::string(128, 'v'));
    auto &candidate = candidates[i];
    candidate.source = temp.root / folder / "asset.mpg";
    candidate.destination = temp.root / folder / "asset.mp4";
    candidate.kind = asobmshow::chart_assets::MediaKind::Video;
    candidate.operation = AssetOperation::CompressH264;
    std::string error;
    candidate.identity = *ReadFileIdentity(candidate.source, error);
    encoded[i] = {FileOutcome::Converted, temp.root / "private" / ("output" + std::to_string(i) + ".mp4")};
  }
  std::mutex mutex; std::condition_variable ready; int prepared = 0;
  std::array<FileResult, 2> results;
  std::array<std::exception_ptr, 2> failures;
  std::vector<std::jthread> threads;
  for (std::size_t i = 0; i < 2; ++i) threads.emplace_back([&, i] {
    try {
      bool cancelled = false;
      FileTransactionDependencies dependencies;
      dependencies.faultPoint = [&](std::string_view point) {
        if (point == "prepared") {
          std::unique_lock lock(mutex); ++prepared; ready.notify_all();
          require(ready.wait_for(lock, std::chrono::seconds(2), [&] { return prepared == 2; }),
                  "independent durable transactions overlap before commit");
        }
        if (i == 0 && point == "retired") cancelled = true;
      };
      results[i] = StorageFileTransaction(paths, dependencies).Replace(candidates[i], encoded[i], [&] { return !cancelled; });
    } catch (...) { failures[i] = std::current_exception(); }
  });
  threads.clear();
  for (auto failure : failures) if (failure) std::rethrow_exception(failure);
  require(results[0].outcome == FileOutcome::Cancelled && read(candidates[0].source) == std::string(1024, 's') &&
              !fs::exists(candidates[0].destination), "cancelled transaction restores only its original");
  require(results[1].outcome == FileOutcome::Converted && !fs::exists(candidates[1].source) &&
              read(candidates[1].destination) == std::string(128, 'v'), "other concurrent transaction remains committed");
  const auto recovered = StorageFileTransaction(paths).Recover();
  require(!recovered.recoveryRequired && !recovered.failed && fs::is_empty(paths.journalRoot),
          "both workers settle shared journal storage before recovery");
}
void testRecoveryAfterContainerRelocation() {
  for (const std::string_view point : {"copying", "retired", "committed", "foreign", "traversal"}) {
    storage_test::TempDirectory temp;
    const auto oldRoot = temp.root / "old-container", newRoot = temp.root / "new-container";
    temp.write("old-container/Documents/library/asset.mpg", std::string(1024, 's'));
    temp.write("old-container/Library/encoded.mp4", std::string(128, 'v'));
    auto paths = MakeStoragePlatformPaths(oldRoot / "Library", {oldRoot / "Documents"}, true);
    std::string error;
    AssetCandidate candidate{.source = oldRoot / "Documents/library/asset.mpg",
        .destination = oldRoot / "Documents/library/asset.mp4", .operation = AssetOperation::CompressH264};
    candidate.identity = *ReadFileIdentity(candidate.source, error);
    FileTransactionDependencies dependencies;
    dependencies.faultPoint = [&](std::string_view reached) {
      if (reached == (point == "foreign" || point == "traversal" ? "retired" : point)) throw Crash{};
    };
    try {
      StorageFileTransaction(paths, dependencies).Replace(candidate,
          {.outcome = FileOutcome::Converted, .outputPath = oldRoot / "Library/encoded.mp4"});
      require(false, "relocation fixture reaches an interrupted transaction");
    } catch (const Crash &) {}
    if (point == "traversal") {
      const auto journal = fs::directory_iterator(paths.journalRoot)->path();
      auto record = nlohmann::json::parse(read(journal));
      record["source"] = (oldRoot / "Documents/library/../library/asset.mpg").string();
      std::ofstream(journal) << record;
    }
    fs::rename(oldRoot, newRoot);
    paths = MakeStoragePlatformPaths(newRoot / "Library", {newRoot / "Documents"}, true);
    paths.resolveRecoveryPath = [=](const fs::path &path) {
      return IsWithinFolder(path, oldRoot / "Documents") ?
          (newRoot / path.lexically_relative(oldRoot)).lexically_normal() : path;
    };
    const auto source = newRoot / "Documents/library/asset.mpg";
    const auto destination = newRoot / "Documents/library/asset.mp4";
    if (point == "foreign") temp.write("new-container/Documents/library/asset.mpg", "foreign source");
    const auto recovered = StorageFileTransaction(paths).Recover();
    if (point == "foreign") {
      require(recovered.recoveryRequired && read(source) == "foreign source" && !fs::is_empty(paths.journalRoot),
          "relocation never overwrites a foreign file at the restored source path");
      continue;
    }
    if (point == "traversal") {
      require(recovered.recoveryRequired && !fs::exists(source) && read(destination) == std::string(128, 'v') &&
          !fs::is_empty(paths.journalRoot), "relocation cannot authorize a journal containing parent traversal");
      continue;
    }
    require(!recovered.recoveryRequired && fs::is_empty(paths.journalRoot),
        "recovery resolves journals after the containing directory moves");
    require(point == "committed" ? !fs::exists(source) && read(destination) == std::string(128, 'v') :
        read(source) == std::string(1024, 's') && !fs::exists(destination),
        "relocated recovery preserves the durable commit or restores the source");
    require(std::distance(fs::directory_iterator(source.parent_path()), fs::directory_iterator{}) == 1,
        "relocated recovery removes owned backups and installation files");
  }
}
void testJournalHierarchyIsDurableBeforeMutation() {
  for (bool recoverFirst : {false, true}) {
    Fixture fixture;
    const auto privateRoot = fixture.temp.root / "new/private";
    auto paths = MakeStoragePlatformPaths(privateRoot, {fixture.temp.root / "library"});
    std::vector<fs::path> synced;
    auto dependencies = DefaultFileTransactionDependencies();
    const auto sync = dependencies.syncDirectory;
    dependencies.syncDirectory = [&](const fs::path &path, std::string &error) {
      synced.push_back(path); return sync(path, error);
    };
    auto assertBarrier = [&] {
      auto expected = paths.journalRoot;
      std::size_t position = 0;
      for (;;) {
        require(position < synced.size() && synced[position++] == expected,
                "journal hierarchy is synced through its previously existing ancestor before mutation");
        if (expected == fixture.temp.root) break;
        expected = expected.parent_path();
      }
    };
    dependencies.faultPoint = [&](std::string_view point) { if (point == "copying") assertBarrier(); };
    StorageFileTransaction transaction(paths, dependencies);
    if (recoverFirst) {
      require(!transaction.Recover().recoveryRequired, "fresh startup initializes durable journal hierarchy");
      assertBarrier();
    }
    require(transaction.Replace(fixture.candidate, fixture.encoded).outcome == FileOutcome::Converted,
            "first transaction succeeds after directory durability barrier");
  }
  Fixture fixture;
  auto paths = MakeStoragePlatformPaths(fixture.temp.root / "new/private", {fixture.temp.root / "library"});
  auto dependencies = DefaultFileTransactionDependencies();
  const auto sync = dependencies.syncDirectory;
  dependencies.syncDirectory = [&](const fs::path &path, std::string &error) {
    if (path == fixture.temp.root) { error = "injected hierarchy flush failure"; return false; }
    return sync(path, error);
  };
  const auto failed = StorageFileTransaction(paths, dependencies).Replace(fixture.candidate, fixture.encoded);
  require(failed.outcome == FileOutcome::Failed && fs::exists(fixture.candidate.source) &&
              !fs::exists(fixture.candidate.destination) && fs::is_empty(paths.journalRoot),
          "failed ancestor flush prevents journal decisions and source mutation");
  const auto recovery = StorageFileTransaction(paths, dependencies).Recover();
  require(recovery.recoveryRequired, "startup cannot admit work after a hierarchy flush failure");
  require(StorageFileTransaction(paths).Replace(fixture.candidate, fixture.encoded).outcome == FileOutcome::Converted,
          "retry flushes the originally captured ancestor even when failed creation left directories behind");
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
#ifdef _WIN32
std::wstring fileDacl(const fs::path &path) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  require(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
              nullptr, nullptr, nullptr, nullptr, &descriptor) == ERROR_SUCCESS, "read fixture DACL");
  LPWSTR text = nullptr;
  const auto converted = ConvertSecurityDescriptorToStringSecurityDescriptorW(
      descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr);
  LocalFree(descriptor);
  require(converted, "format fixture DACL");
  const std::wstring result(text);
  LocalFree(text);
  return result;
}
void restrictFileDacl(const fs::path &path) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"D:P(A;;FA;;;OW)(A;;FA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr), "create private fixture DACL");
  const auto applied = SetFileSecurityW(path.c_str(),
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
  LocalFree(descriptor);
  require(applied, "apply private fixture DACL");
}
#endif
void testReplacementPermissions() {
#ifdef _WIN32
  for (bool samePath : {false, true}) {
    Fixture f(samePath);
    restrictFileDacl(f.candidate.source);
    const auto expected = fileDacl(f.candidate.source);
    require(StorageFileTransaction(f.paths).Replace(f.candidate, f.encoded).outcome == FileOutcome::Converted,
            "Windows replacement succeeds with an explicit protected source DACL");
    require(fileDacl(f.candidate.destination) == expected,
            "Windows replacement preserves explicit DACL entries and inheritance protection");
    requireNoRecoveryFiles(f);
  }
#else
  for (bool samePath : {false, true}) {
    for (auto mode : {fs::perms::owner_read | fs::perms::owner_write,
                      fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read,
                      fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read}) {
      Fixture f(samePath);
      fs::permissions(f.candidate.source, mode);
      require(StorageFileTransaction(f.paths).Replace(f.candidate, f.encoded).outcome == FileOutcome::Converted,
              "replacement succeeds for private and shared source permissions");
      require((fs::status(f.candidate.destination).permissions() & fs::perms::all) == mode,
              "replacement preserves source access permissions");
      requireNoRecoveryFiles(f);
    }
  }
  Fixture failed;
  const auto sharedMode = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read;
  fs::permissions(failed.candidate.source, sharedMode);
  FileTransactionDependencies dependencies;
  dependencies.preservePermissions = [](const fs::path &, const FileIdentity &, const fs::path &,
                                        const FileIdentity &, std::string &error) {
    error = "injected permission failure"; return false;
  };
  const auto result = StorageFileTransaction(failed.paths, dependencies).Replace(failed.candidate, failed.encoded);
  require(result.outcome == FileOutcome::Failed && !fs::exists(failed.candidate.destination) &&
              read(failed.candidate.source) == std::string(1024, 's') &&
              (fs::status(failed.candidate.source).permissions() & fs::perms::all) == sharedMode,
          "permission preservation failure leaves the original media and mode intact");
  requireNoRecoveryFiles(failed);
#endif
}
void testPermissionCopyRejectsChangedIdentity() {
  Fixture f;
  std::string error;
  const auto installed = f.temp.root / "library/install.tmp";
  f.temp.write("library/install.tmp", "installation");
  const auto installedIdentity = *ReadFileIdentity(installed, error);
  auto sourceIdentity = f.candidate.identity;
  ++sourceIdentity.fileId;
  const auto preserve = DefaultFileTransactionDependencies().preservePermissions;
  require(!preserve(f.candidate.source, sourceIdentity, installed, installedIdentity, error),
          "permission copying rejects a substituted source identity");
  auto changedInstalledIdentity = installedIdentity;
  ++changedInstalledIdentity.fileId;
  require(!preserve(f.candidate.source, f.candidate.identity, installed, changedInstalledIdentity, error),
          "permission copying rejects a substituted installation identity");
  require(read(f.candidate.source) == std::string(1024, 's') && read(installed) == "installation",
          "identity rejection leaves both file contents unchanged");
}
void testOverlappingRegisteredRootDoesNotBlockRecovery() {
  Fixture f;
  // A broad registration is unrelated to the selected library and can contain
  // the app-private directory without authorizing optimization of that directory.
  f.paths.allowedRoots.push_back(f.temp.root);
  fs::create_directories(f.paths.temporaryRoot);
  require(!StorageFileTransaction(f.paths).Recover().recoveryRequired,
          "empty private temporary directory beneath a registration does not block startup");
  const auto job = f.paths.temporaryRoot / "12345678-1234-4234-8234-123456789abc";
  f.temp.write(fs::relative(job / "candidate.mp4", f.temp.root), "abandoned candidate");
  f.temp.write(fs::relative(f.paths.temporaryRoot / "unknown", f.temp.root), "retained");
  require(!StorageFileTransaction(f.paths).Recover().recoveryRequired && !fs::exists(job) &&
              read(f.paths.temporaryRoot / "unknown") == "retained" &&
              read(f.candidate.source) == std::string(1024, 's'),
          "overlapping registrations do not prevent owned cleanup or affect unrelated files");
}
void testTwoWorkerJournalsAfterRestart() {
  Fixture f;
  const auto job = f.paths.temporaryRoot / "12345678-1234-4234-8234-123456789abc";
  const auto unknown = f.paths.temporaryRoot / "12345678-1234-4234-8234-123456789abd";
  f.temp.write(fs::relative(unknown / "candidate-not-a-uuid.mp4", f.temp.root), "foreign");
  std::string error;
  for (const auto id : {"12345678-1234-4234-8234-123456789abe", "12345678-1234-4234-8234-123456789abf"}) {
    auto candidate = f.candidate;
    candidate.source = f.temp.root / "library" / id / "source.mpg";
    candidate.destination = candidate.source.parent_path() / "source.mp4";
    f.temp.write(fs::relative(candidate.source, f.temp.root), std::string(1024, 's'));
    candidate.identity = *ReadFileIdentity(candidate.source, error);
    auto encoded = f.encoded; encoded.outputPath = job / (std::string("candidate-") + id + ".mp4");
    f.temp.write(fs::relative(encoded.outputPath, f.temp.root), std::string(128, 'v'));
    FileTransactionDependencies dependencies;
    dependencies.faultPoint = [](std::string_view point) { if (point == "published") throw Crash{}; };
    try { StorageFileTransaction(f.paths, dependencies).Replace(candidate, encoded); }
    catch (const Crash &) {}
  }
  require(std::distance(fs::directory_iterator(f.paths.journalRoot), fs::directory_iterator{}) == 2,
      "two independent durable worker journals survive simulated termination");
  f.temp.write(fs::relative(f.paths.journalRoot / "corrupt.json", f.temp.root), "invalid");
  require(StorageFileTransaction(f.paths).Recover().recoveryRequired && fs::exists(job), "unresolved recovery preserves worker outputs");
  fs::remove(f.paths.journalRoot / "corrupt.json");
  auto report = StorageFileTransaction(f.paths).Recover();
  require(!report.recoveryRequired && !fs::exists(job) && fs::exists(unknown), "restart clears recognized worker outputs and preserves unknown names");
  for (const auto id : {"12345678-1234-4234-8234-123456789abe", "12345678-1234-4234-8234-123456789abf"})
    require(read(f.temp.root / "library" / id / "source.mpg") == std::string(1024, 's') &&
        !fs::exists(f.temp.root / "library" / id / "source.mp4"), "both original media restored");
}
void testAbandonedCandidatesAfterRestart() {
  constexpr auto job = "12345678-1234-4234-8234-123456789abc";
  for (const bool journal : {false, true}) {
    Fixture f;
    const auto candidate = f.paths.temporaryRoot / job /
        (journal ? "candidate.mp4" : "candidate-12345678-1234-4234-8234-123456789abd.MP4");
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
void testInstallationIdentityFailurePreservesOwnership() {
  for (bool foreign : {false, true}) {
    Fixture f;
    auto dependencies = DefaultFileTransactionDependencies();
    const auto identity = dependencies.identity;
    bool failed = false;
    fs::path installation;
    dependencies.identity = [&](const fs::path &path, std::string &error) -> std::optional<FileIdentity> {
      if (!failed && path.extension() == ".install") {
        failed = true; installation = path;
        if (foreign) {
          fs::rename(path, f.temp.root / "private/displaced.install");
          std::ofstream(path) << "foreign replacement";
        }
        error = "injected installation identity failure"; return {};
      }
      return identity(path, error);
    };
    const auto result = StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded);
    require(failed && result.outcome == FileOutcome::Failed && !result.reclaimedBytes &&
                read(f.candidate.source) == std::string(1024, 's') && !fs::exists(f.candidate.destination),
            "installation identity failure never retires the source");
    if (foreign) {
      require(result.reason.code == StorageReason::RecoveryRequired && read(installation) == "foreign replacement",
              "identity failure never unlinks a foreign replacement at the installation path");
    } else {
      require(result.reason.code != StorageReason::RecoveryRequired && !fs::exists(installation),
              "owned installation identity failure settles cleanup without leaking an unjournaled file");
      requireNoRecoveryFiles(f);
    }
  }
  for (bool foreign : {false, true}) {
    Fixture f;
    auto dependencies = DefaultFileTransactionDependencies();
    const auto create = dependencies.createExclusive;
    fs::path installation;
    dependencies.createExclusive = [&](const fs::path &path, std::string &error) {
      auto result = create(path, error);
      if (!result.ready) return result;
      installation = path;
      if (foreign) {
        fs::rename(path, f.temp.root / "private/displaced.install");
        std::ofstream(path) << "foreign replacement";
      }
      result.ready = false; error = "injected initial installation flush failure";
      return result;
    };
    const auto result = StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded);
    require(result.outcome == FileOutcome::Failed && read(f.candidate.source) == std::string(1024, 's') &&
                !fs::exists(f.candidate.destination), "failed creation flush never retires the source");
    if (foreign) {
      require(result.reason.code == StorageReason::RecoveryRequired && read(installation) == "foreign replacement" &&
                  !fs::is_empty(f.paths.journalRoot),
              "failed creation flush journals owned identity and preserves an unrelated replacement");
    } else {
      require(result.reason.code == StorageReason::IoError && !fs::exists(installation),
              "failed creation flush removes only its owned installation and reports ordinary failure");
      requireNoRecoveryFiles(f);
    }
  }
}
void testTargetRevalidationRetainsCleanupEvidence() {
  {
    Fixture f;
    f.candidate.destination = f.temp.root / "library/unsupported.ogg";
    auto dependencies = DefaultFileTransactionDependencies();
    const auto create = dependencies.createExclusive;
    bool created = false;
    dependencies.createExclusive = [&](const fs::path &path, std::string &error) {
      created = true; return create(path, error);
    };
    const auto result = StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded);
    require(result.reason.code == StorageReason::UnsafePath && !created,
        "invalid static targets are rejected before creating an installation");
    requireNoRecoveryFiles(f);
  }
  Fixture f;
  const auto parent = f.candidate.source.parent_path(), moved = f.temp.root / "moved-library";
  std::error_code linkError;
  fs::create_directory_symlink(parent, moved, linkError);
  if (linkError) return; // Symlinks may require additional privileges on Windows.
  fs::remove(moved);
  auto dependencies = DefaultFileTransactionDependencies();
  const auto create = dependencies.createExclusive;
  fs::path installation;
  dependencies.createExclusive = [&](const fs::path &path, std::string &error) {
    auto result = create(path, error);
    require(result.ready, "create installation before parent replacement");
    installation = path;
    fs::rename(parent, moved);
    fs::create_directory_symlink(moved, parent);
    return result;
  };
  const auto result = StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded);
  require(result.reason.code == StorageReason::RecoveryRequired && fs::exists(installation) &&
      !fs::is_empty(f.paths.journalRoot), "failed revalidation cleanup retains a recovery journal");
  require(StorageFileTransaction(f.paths).Recover().recoveryRequired,
      "recovery refuses a replaced parent until access is safe again");
  fs::remove(parent); fs::rename(moved, parent);
  const auto recovered = StorageFileTransaction(f.paths).Recover();
  require(!recovered.recoveryRequired && !fs::exists(installation) &&
      read(f.candidate.source) == std::string(1024, 's') && !fs::exists(f.candidate.destination),
      "restoring the parent lets startup clean the owned installation without changing source media");
  requireNoRecoveryFiles(f);
}
void testInitialJournalFailureReportsUnsettledStateOnly() {
  for (int failure = 0; failure < 4; ++failure) {
    Fixture f;
    auto dependencies = DefaultFileTransactionDependencies();
    const auto create = dependencies.createExclusive;
    const auto remove = dependencies.remove;
    const auto sync = dependencies.syncDirectory;
    fs::path installation, journal, blocker;
    dependencies.createExclusive = [&](const fs::path &path, std::string &error) {
      auto created = create(path, error);
      require(created.ready, "create installation before journal failure");
      installation = path;
      const auto id = path.stem().string().substr(std::string(".asobmashow-storage-").size());
      journal = f.paths.journalRoot / (id + ".json");
      blocker = failure == 3 ? journal : fs::path(journal.string() + ".tmp");
      fs::create_directory(blocker);
      std::ofstream(blocker / "foreign") << "retain";
      return created;
    };
    dependencies.remove = [&](const fs::path &path, std::string &error) {
      if (failure == 1 && path == installation) { error = "injected install removal failure"; return false; }
      return remove(path, error);
    };
    dependencies.syncDirectory = [&](const fs::path &path, std::string &error) {
      if (failure == 2 && path == f.candidate.source.parent_path()) {
        error = "injected install cleanup flush failure"; return false;
      }
      return sync(path, error);
    };
    const auto result = StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded);
    require(result.outcome == FileOutcome::Failed && read(f.candidate.source) == std::string(1024, 's') &&
                !fs::exists(f.candidate.destination) && read(blocker / "foreign") == "retain",
            "initial journal failure preserves the source and unrelated journal artifacts");
    require(result.reason.code == (failure == 0 ? StorageReason::IoError : StorageReason::RecoveryRequired),
            "unpublished journal failure releases maintenance only after installation cleanup is durable");
    require(fs::exists(installation) == (failure == 1), "initial journal failure attempts owned installation cleanup");
    if (failure == 0) {
      require(!fs::exists(journal) && !StorageFileTransaction(f.paths).Recover().recoveryRequired,
              "clean unpublished failure leaves no transaction for startup recovery");
    }
  }
}
void testLosslessReplacementWithoutSavings() {
  for (const auto bytes : {1024u, 1152u}) {
    for (const auto point : {"", "prepared", "published", "retired", "committed"}) {
      Fixture f;
      fs::rename(f.candidate.source, f.temp.root / "library/asset.wav");
      f.candidate.source = f.temp.root / "library/asset.wav";
      f.candidate.destination = f.temp.root / "library/asset.flac";
      f.candidate.operation = AssetOperation::LosslessFlac;
      f.candidate.kind = asobmshow::chart_assets::MediaKind::Audio;
      f.temp.write("private/encoded.mp4", std::string(bytes, 'f'));
      FileTransactionDependencies dependencies;
      dependencies.faultPoint = [point](std::string_view reached) { if (reached == point) throw Crash{}; };
      try {
        const auto result = StorageFileTransaction(f.paths, dependencies).Replace(f.candidate, f.encoded);
        require(std::string_view(point).empty(), "lossless crash boundary reached");
        require(result.outcome == FileOutcome::Converted && result.reclaimedBytes == 0 &&
                    result.addedBytes == bytes - 1024 && result.bytesAfter == bytes,
                "equal or larger lossless output replaces WAV without claiming savings");
      } catch (const Crash &) {}
      const auto recovered = StorageFileTransaction(f.paths).Recover();
      require(!recovered.recoveryRequired && !recovered.reclaimedBytes,
              "equal or larger lossless journal recovers without unsigned savings");
      require(recovered.addedBytes == (std::string_view(point) == "committed" ? bytes - 1024 : 0),
              "recovery reports growth only when finalizing a committed backup");
      const bool committed = std::string_view(point).empty() || std::string_view(point) == "committed";
      require(committed ? !fs::exists(f.candidate.source) && read(f.candidate.destination) == std::string(bytes, 'f')
                        : read(f.candidate.source) == std::string(1024, 's') && !fs::exists(f.candidate.destination),
              "recovery preserves only the durable lossless conversion decision");
      requireNoRecoveryFiles(f);
    }
  }
  for (const auto operation : {AssetOperation::LossyVorbis, AssetOperation::CompressH264}) {
    Fixture f;
    if (operation == AssetOperation::LossyVorbis) {
      fs::rename(f.candidate.source, f.temp.root / "library/asset.wav");
      f.candidate.source = f.temp.root / "library/asset.wav";
      f.candidate.destination = f.temp.root / "library/asset.ogg";
      f.candidate.operation = operation;
    }
    f.temp.write("private/encoded.mp4", std::string(1024, 'f'));
    const auto result = StorageFileTransaction(f.paths).Replace(f.candidate, f.encoded);
    require(result.reason.code == StorageReason::NotSmaller && fs::exists(f.candidate.source) &&
                !fs::exists(f.candidate.destination), "lossy replacements still require smaller output");
  }
}
void testExistingDestinationReuse() {
  for (const auto point : {"", "prepared", "retired-before-journal", "retired", "committed"}) {
    Fixture f;
    f.temp.write("library/asset.mp4", std::string(2048, 'd'));
    std::string error;
    f.candidate.existingDestinationIdentity = ReadFileIdentity(f.candidate.destination, error);
    f.candidate.existingDestinationHash = *HashStorageFile(f.candidate.destination, {}, error);
    FileTransactionDependencies dependencies;
    dependencies.faultPoint = [point](std::string_view reached) { if (reached == point) throw Crash{}; };
    try {
      const auto result = StorageFileTransaction(f.paths, dependencies).ReuseExisting(f.candidate);
      require(std::string_view(point).empty(), "reuse reaches its requested crash boundary");
      require(result.outcome == FileOutcome::Deleted && result.reclaimedBytes == 1024 && !result.addedBytes &&
                  result.filesystemChanges.size() == 1 && result.filesystemChanges.front().path == f.candidate.source &&
                  !result.filesystemChanges.front().after,
              "reusing a larger existing destination reclaims the source without counting retained bytes twice");
    } catch (const Crash &) {}
    const auto recovered = StorageFileTransaction(f.paths).Recover();
    const bool committed = std::string_view(point).empty() || std::string_view(point) == "committed";
    require(!recovered.recoveryRequired && fs::exists(f.candidate.source) != committed &&
                ReadFileIdentity(f.candidate.destination, error) == f.candidate.existingDestinationIdentity &&
                read(f.candidate.destination) == std::string(2048, 'd'),
            "reuse recovery restores only uncommitted sources and never replaces or removes the retained destination");
    require(recovered.reclaimedBytes == (std::string_view(point) == "committed" ? 1024 : 0),
            "committed reuse recovery counts the whole removed source once");
    requireNoRecoveryFiles(f);
  }
  for (const auto point : {"before", "prepared", "retired", "committed"}) {
    Fixture f;
    f.temp.write("library/asset.mp4", std::string(128, 'd'));
    std::string error;
    f.candidate.existingDestinationIdentity = ReadFileIdentity(f.candidate.destination, error);
    f.candidate.existingDestinationHash = *HashStorageFile(f.candidate.destination, {}, error);
    auto changeDestination = [&] { f.temp.write("library/asset.mp4", "foreign destination edit"); };
    if (std::string_view(point) == "before") changeDestination();
    FileTransactionDependencies dependencies;
    dependencies.faultPoint = [&](std::string_view reached) { if (reached == point) changeDestination(); };
    const auto result = StorageFileTransaction(f.paths, dependencies).ReuseExisting(f.candidate);
    require(result.reclaimedBytes == 0 && read(f.candidate.destination) == "foreign destination edit",
            "changed retained destination is never overwritten or used to claim reclaimed bytes");
    if (std::string_view(point) == "committed") {
      require(result.reason.code == StorageReason::RecoveryRequired &&
                  StorageFileTransaction(f.paths).Recover().recoveryRequired,
              "changed destination after commit preserves the recovery backup");
      bool originalRetained = false;
      for (const auto &entry : fs::directory_iterator(f.candidate.source.parent_path()))
        if (entry.path().extension() == ".backup") originalRetained |= read(entry.path()) == std::string(1024, 's');
      require(originalRetained, "committed reuse retains source bytes when its destination changes");
    } else require(result.outcome != FileOutcome::Deleted && read(f.candidate.source) == std::string(1024, 's'),
                   "destination changes before commit preserve or restore the original source");
  }
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
    unsigned beforeRecovery = 0, afterRecovery = 0;
    bool observedFolder = true;
    const auto recovered = StorageFileTransaction(f.paths, {}).Recover({}, {
      .before = [&](const fs::path &folder) { ++beforeRecovery; observedFolder &= folder == f.candidate.source.parent_path(); },
      .after = [&](const fs::path &folder, const auto &) { ++afterRecovery; observedFolder &= folder == f.candidate.source.parent_path(); },
    });
    require(!recovered.recoveryRequired && recovered.reclaimedBytes == 896,
            "later recovery accounts for successful cleanup");
    require(beforeRecovery == 1 && afterRecovery == 1 && observedFolder,
            "recovery reports the current folder and one completed operation without a counting scan");
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
    testAbandonedScanCleanup();
    testReservationIncludesPendingRecoveryRoots();
    testConcurrentCommitAndCancellation();
    testRecoveryAfterContainerRelocation();
    testJournalHierarchyIsDurableBeforeMutation();
    testTwoWorkerJournalsAfterRestart();
    testReplacementPermissions();
    testPermissionCopyRejectsChangedIdentity();
    testOverlappingRegisteredRootDoesNotBlockRecovery();
    testAbandonedCandidatesAfterRestart();
    testSuccessAndCancellation();
    testInstallationIdentityFailurePreservesOwnership();
    testTargetRevalidationRetainsCleanupEvidence();
    testInitialJournalFailureReportsUnsettledStateOnly();
    testLosslessReplacementWithoutSavings();
    testExistingDestinationReuse();
    testRestartAtEveryBoundary();
    testFailuresNeverClobberAndCleanupAccounting();
    testForeignIdentityAndCorruptJournalArePreserved();
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
  }
}
