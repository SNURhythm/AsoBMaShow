#include "../src/storage/ChartResourceInventory.h"
#include "StorageTestSupport.h"

#include <iostream>
#include <set>

using namespace storage_management;
using storage_test::require;
using storage_test::TempDirectory;
namespace fs = std::filesystem;

namespace {
InventoryRequest request(const fs::path &root) {
  InventoryRequest result;
  result.groups.push_back({"group", root});
  result.policy.audio = AudioAction::LossyVorbis;
  result.policy.bga = BgaAction::CompressH264;
  return result;
}
std::set<fs::path> candidateNames(const StorageInventory &inventory) {
  std::set<fs::path> result;
  for (const auto &candidate : inventory.candidates) result.insert(candidate.source.filename());
  return result;
}
void testWholeFolderIncludesSabunsAndUnreferencedMedia() {
  TempDirectory temp;
  temp.write("group/leader.bms", "#WAV01 lead.wav\n#BMP01 lead.mpg\n");
  temp.write("group/sabun.pms", "#WAV01 nested/sabun.flac\n#BMP01 nested/sabun.avi\n");
  temp.write("group/broken.bme", "unparseable\xff\xfe");
  for (const auto &name : {"lead.wav", "lead.mpg", "nested/sabun.FLAC", "nested/sabun.AVI",
                           "nested/未使用.WAV", "nested/extra.m1v", "banner.png", "stage.bmp",
                           "preview.ogg", "readme.txt"}) temp.write(fs::path("group") / name);
  auto input = request(temp.root / "group");
  input.groups.push_back({"nested", temp.root / "group/nested"});
  input.groups.push_back({"duplicate", temp.root / "group"});
  const auto inventory = BuildInventory(input, {}, {});
  require(inventory.complete && !inventory.cancelled, "ordinary folders inventory completely");
  require(candidateNames(inventory) == std::set<fs::path>{
              "lead.wav", "lead.mpg", "sabun.FLAC", "sabun.AVI", "未使用.WAV", "extra.m1v"},
          "recursive compression includes sabun and unreferenced resources once");
  require(inventory.candidates.size() == 6, "overlapping groups never duplicate operations");
  require(inventory.totalBytes == inventory.groups.front().totalBytes,
          "aggregate size is a union, not sum of overlapping selected rows");
  auto trailing = request(temp.root / "group" / "");
  require(candidateNames(BuildInventory(trailing, {}, {})) == candidateNames(inventory),
          "trailing folder separators do not hide children");
  input.groups = {{"fallback", temp.root / "group/leader.bms", true, true}};
  require(candidateNames(BuildInventory(input, {}, {})) == candidateNames(inventory),
          "chart-path fallback selects the whole containing folder");

  input.policy.audio = AudioAction::Keep;
  input.policy.bga = BgaAction::Delete;
  const auto deletion = BuildInventory(input, {}, {});
  require(candidateNames(deletion) == std::set<fs::path>{"lead.mpg", "sabun.AVI", "extra.m1v"},
          "Delete BGA includes every video but preserves all images and audio");
  for (const auto &candidate : deletion.candidates)
    require(candidate.operation == AssetOperation::Delete, "deletion never schedules encoding");
}
void testAmbiguousStemsAndFreshness() {
  TempDirectory temp;
  for (const auto &name : {"sound.wav", "sound.flac", "movie.mpg", "movie.png", "plain.wav"})
    temp.write(name);
  const auto input = request(temp.root);
  auto inventory = BuildInventory(input, {}, {});
  require(candidateNames(inventory) == std::set<fs::path>{"plain.wav"},
          "ambiguous same-stem alternatives are excluded from conversion");
  std::string error;
  require(RevalidateInventory(inventory, {}, error), "unchanged inventory stays valid");
  temp.write("plain.wav", "external change");
  require(!RevalidateInventory(inventory, {}, error), "external edits invalidate inventory");
  inventory = BuildInventory(input, {}, {});
  temp.write("plain.ogg");
  require(!RevalidateInventory(inventory, {}, error), "new sibling collision invalidates inventory");
  temp.write("movie.mp4");
  auto deletion = input;
  deletion.policy.audio = AudioAction::Keep;
  deletion.policy.bga = BgaAction::Delete;
  require(candidateNames(BuildInventory(deletion, {}, {})) == std::set<fs::path>{"movie.mpg", "movie.mp4"},
          "delete all video alternatives while retaining the same-stem image");
}
void testUnsafeAndUnsupportedFiles() {
  TempDirectory temp;
  temp.write("outside.wav");
  temp.write("selected/safe.wav");
  fs::create_symlink(temp.root / "outside.wav", temp.root / "selected/link.wav");
  fs::create_directory_symlink(temp.root, temp.root / "selected/escape");
  fs::create_hard_link(temp.root / "outside.wav", temp.root / "selected/hard.wav");
  auto input = request(temp.root / "selected");
  const auto inventory = BuildInventory(input, {}, {});
  require(candidateNames(inventory) == std::set<fs::path>{"safe.wav"},
          "symlinks and hard links cannot escape or mutate other selections");
  require(inventory.complete && inventory.totalBytes ==
              fs::file_size(temp.root / "selected/safe.wav") + fs::file_size(temp.root / "selected/hard.wav") &&
              inventory.groups.front().audioBytes == inventory.totalBytes,
          "hard-linked media contributes logical bytes while remaining ineligible for mutation");
  auto overlapping = input;
  overlapping.groups.push_back({"duplicate", temp.root / "selected"});
  const auto unionInventory = BuildInventory(overlapping);
  require(unionInventory.totalBytes == inventory.totalBytes &&
              unionInventory.groups.back().totalBytes == inventory.totalBytes,
          "overlapping measurements count hard-linked paths once in the union");
  fs::create_symlink(temp.root / "absent", temp.root / "selected/safe.ogg");
  require(BuildInventory(input, {}, {}).candidates.empty(),
          "dangling destination symlink is still a collision");
  fs::remove(temp.root / "selected/safe.ogg");
  fs::create_symlink(temp.root / "outside.wav", temp.root / "selected/safe.mp3");
  require(BuildInventory(input, {}, {}).candidates.empty(),
          "unsafe same-stem alternatives still block ambiguous audio conversion");
  fs::remove(temp.root / "selected/safe.mp3");
  input.groups.front().path = temp.root / "selected/escape";
  require(BuildInventory(input, {}, {}).candidates.empty(), "symlink root is rejected");
  input.groups.front().path = temp.root / "missing";
  auto missing = BuildInventory(input, {}, {});
  require(!missing.complete && !missing.groups.front().totalBytes.has_value(),
          "missing roots report unavailable size, not zero");
  input.groups.front().path = temp.root / "selected";
  input.groups.front().writableLocal = false;
  require(BuildInventory(input, {}, {}).candidates.empty(), "archive/SAF groups never mutate");
  input.groups.front().writableLocal = true;
  const auto cancelled = BuildInventory(input, [] { return false; }, {});
  require(cancelled.cancelled && cancelled.candidates.empty(), "cancel stops traversal promptly");
  fs::permissions(temp.root / "selected", fs::perms::none);
  const auto unreadable = BuildInventory(input, {}, {});
  fs::permissions(temp.root / "selected", fs::perms::owner_all);
  require(!unreadable.complete && unreadable.candidates.empty(), "unreadable folder is unavailable");
}
}

int main() {
  try {
    testWholeFolderIncludesSabunsAndUnreferencedMedia();
    testAmbiguousStemsAndFreshness();
    testUnsafeAndUnsupportedFiles();
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
