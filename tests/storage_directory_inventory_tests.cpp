#include "../src/storage/StoragePreflight.h"
#include "StorageTestSupport.h"
#include <algorithm>
#include <iostream>

using namespace storage_management;
using storage_test::require;
using storage_test::TempDirectory;
namespace fs = std::filesystem;
namespace {
InventoryRequest request(const fs::path &path) {
  return {.groups = {{"root", path}}, .policy = {.audio = AudioAction::LossyVorbis}};
}
void summariesDoNotRetainAllAssets() {
  TempDirectory temp;
  temp.write("charts/a.wav", std::string(10, 'a'));
  temp.write("charts/b.wav", std::string(20, 'b'));
  temp.write("charts/sub/c.wav", std::string(30, 'c'));
  temp.write("charts/sub/readme.txt", "12345");
  temp.write("charts/chart.bms", "1234");
  temp.write("outside.wav", "1234567");
  fs::create_hard_link(temp.root / "outside.wav", temp.root / "charts/hard.wav");
  auto input = request(temp.root / "charts");
  input.groups.push_back({"sub", temp.root / "charts/sub"});
  input.groups.push_back({"duplicate", temp.root / "charts"});
  const auto preflight = BuildStoragePreflight(input);
  require(preflight.complete && preflight.directories.size() == 2, "preflight retains one summary per directory");
  require(preflight.eligibleCount == 3 && preflight.eligibleBytes == 60 && preflight.totalBytes == 76,
          "preflight sums eligible assets once while measuring ineligible hard links");
  require(preflight.groups.at(0).totalBytes == 76 && preflight.groups.at(1).totalBytes == 35 &&
              preflight.groups.at(2).totalBytes == 76, "recursive and duplicate groups retain their sizes");
  require(preflight.skippedCount == 1 && preflight.skips.size() == 1, "skip counts survive compact summaries");
  for (const auto &[path, summary] : preflight.directories)
    require(summary.samples.empty() && summary.fingerprint.size() == 64, "directory summaries contain hashes, not asset samples");
}
void fingerprintsRejectChangedEntries() {
  TempDirectory temp;
  temp.write("charts/a.wav", "12345");
  auto input = request(temp.root / "charts");
  auto preflight = BuildStoragePreflight(input);
  std::string error;
  auto loaded = LoadStorageDirectory(preflight.directories.at(temp.root / "charts"), input, {}, error);
  require(loaded && loaded->Size() == 1, "unchanged directory loads its bounded working set");
  fs::last_write_time(temp.root / "charts/a.wav", fs::last_write_time(temp.root / "charts/a.wav") + std::chrono::seconds(1));
  require(!LoadStorageDirectory(preflight.directories.at(temp.root / "charts"), input, {}, error),
          "changed source metadata invalidates directory fingerprint");
  preflight = BuildStoragePreflight(input);
  temp.write("charts/a.ogg", "collision");
  require(!LoadStorageDirectory(preflight.directories.at(temp.root / "charts"), input, {}, error),
          "new destination never becomes part of confirmed evidence");
  preflight = BuildStoragePreflight(input);
  const auto oldTime = fs::last_write_time(temp.root / "charts");
  fs::rename(temp.root / "charts", temp.root / "retired");
  temp.write("charts/a.wav", "12345"); temp.write("charts/a.ogg", "collision");
  fs::last_write_time(temp.root / "charts", oldTime);
  require(!LoadStorageDirectory(preflight.directories.at(temp.root / "charts"), input, {}, error),
          "directory replacement is rejected even with restored timestamps");
}
StorageDirectoryEntry audio(std::string name) {
  return {.name = std::move(name), .kind = StorageEntryKind::Regular, .bytes = 10,
          .permissions = fs::perms::owner_read | fs::perms::owner_write,
          .identity = FileIdentity{.bytes = 10, .modified = fs::file_time_type{}, .device = 1, .fileId = 2}};
}
void largeDirectorySpillRetainsCollisionRules() {
  TempDirectory temp;
  temp.write("keep", "unrelated");
  const auto scratch = temp.root / u8"임시-音楽-🎵";
  fs::create_directory(scratch);
  {
    StorageDirectoryInventory memory(temp.root);
    StorageDirectoryInventory disk(temp.root, {.records = 2, .bytes = 4096, .scratchRoot = scratch});
    std::vector<StorageDirectoryEntry> entries{audio("sound.WAV"), audio("SOUND.flac"), audio("plain.wav"),
        {.name = "plain.mp3", .kind = StorageEntryKind::Directory},
        audio("link.wav"), {.name = "link.ogg", .kind = StorageEntryKind::Symlink}};
    std::string error;
    for (const auto &entry : entries) require(memory.Add(entry, error), "add in-memory entry");
    std::reverse(entries.begin(), entries.end());
    for (const auto &entry : entries) require(disk.Add(entry, error), "add spilled entry");
    require(!memory.Spilled() && disk.Spilled() && disk.Size() == 6, "large directory spills without retaining all entries");
    require(memory.Fingerprint({}, error) == disk.Fingerprint({}, error), "ordered fingerprint is independent of enumeration and spill");
    StorageDiagnostic reason;
    require(!StorageCandidateForEntry(audio("sound.WAV"), disk, {.audio = AudioAction::LossyVorbis}, {}, reason) &&
                reason.code == StorageReason::Collision, "case-varied same-stem alternatives stay ambiguous after spill");
    require(StorageCandidateForEntry(audio("plain.wav"), disk, {.audio = AudioAction::LossyVorbis}, {}, reason).has_value(),
            "an unrelated directory extension does not block safe media");
    require(!StorageCandidateForEntry(audio("link.wav"), disk, {.audio = AudioAction::LossyVorbis}, {}, reason),
            "unsafe destination remains a collision");
    require(disk.Remove("sound.WAV", error), "advance removes retired entry");
    auto changed = audio("sound.ogg");
    require(disk.Set(changed, error) && disk.Find("sound.ogg", error).has_value(), "advance retains installed entry");
    require(!disk.Find("sound.WAV", error), "retired entry stays absent");
    require(!disk.Visit([] { return false; }, [](const auto &) { return true; }, error), "spill iteration observes cancellation");
  }
  require(fs::is_empty(scratch) && fs::exists(temp.root / "keep"),
          "Unicode spill cleanup preserves unrelated private files");
}
void skipDetailsAndSpillErrorsStayBounded() {
  TempDirectory temp;
  StorageScanDependencies deps;
  deps.limits.scratchRoot = temp.root;
  deps.enumerate = [](const auto &, const auto &, auto, auto check, auto visit, auto &, auto *, auto &) {
    for (int i = 0; i < 5000; ++i) {
      if (!check() || !visit(StorageDirectoryEntry{.name = std::to_string(i) + ".wav",
          .kind = StorageEntryKind::Symlink})) return false;
    }
    return true;
  };
  const auto preflight = BuildStoragePreflight(request(temp.root / "synthetic"), {}, nullptr, deps);
  require(preflight.complete && preflight.skippedCount == 5000 && preflight.skips.size() == 100 &&
      preflight.directories.begin()->second.reasons.at(StorageReason::UnsafePath) == 5000,
      "large directory keeps exact reason counts and only bounded detail samples");
  require(fs::is_empty(temp.root), "completed preflight releases its private spill");
  std::string error;
  StorageDirectoryInventory directory(temp.root, {.records = 0, .scratchRoot = temp.root});
  require(directory.Add(audio("first.wav"), error), "force first entry to disk");
  require(!directory.Add(audio("first.wav"), error) && directory.Size() == 1,
      "duplicate insert cannot alter inventory cardinality");
  require(!directory.Add(audio("../escape.wav"), error), "reject nonlocal names");
  auto next = directory.Next({}, error);
  require(next && next->name == "first.wav" && !directory.Next(next->name, error), "ordered seek ends at final entry");
}
void failedSpillAndRestartNeverAuthorizeStaleMetadata() {
  TempDirectory temp;
  temp.write("not-directory", "keep");
  std::string error;
  StorageDirectoryInventory blocked(temp.root, {.records = 1, .scratchRoot = temp.root / "not-directory"});
  require(blocked.Add(audio("one.wav"), error), "first record fits memory");
  require(!blocked.Add(audio("two.wav"), error) && !error.empty(), "spill creation failure is explicit");
  temp.write("storage-directory-old/directory.sqlite", "unknown old scan");
  StorageDirectoryInventory restarted(temp.root, {.records = 1, .scratchRoot = temp.root});
  require(restarted.Add(audio("live.wav"), error) && restarted.Size() == 1 && !restarted.Find("one.wav", error),
          "restart starts from live enumeration rather than prior spill state");
  require(fs::exists(temp.root / "storage-directory-old/directory.sqlite"), "unknown stale private state remains untouched");
  const auto cancelled = BuildStoragePreflight(request(temp.root), [] { return false; });
  require(cancelled.cancelled && !cancelled.complete && cancelled.eligibleCount == 0, "cancelled preflight authorizes no candidates");
}
}
int main() {
  try {
    summariesDoNotRetainAllAssets();
    fingerprintsRejectChangedEntries();
    largeDirectorySpillRetainsCollisionRules();
    failedSpillAndRestartNeverAuthorizeStaleMetadata();
    skipDetailsAndSpillErrorsStayBounded();
  } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
