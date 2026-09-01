#include "music_select/MusicSelectReplaySlots.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void touch(const std::filesystem::path &path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path) << "replay";
}

void testUsesExactSourceChartPathsAndFilesExistsSemantics() {
  const auto root = std::filesystem::temp_directory_path() /
                    "asobmashow-music-select-replay-slots";
  std::filesystem::remove_all(root);

  ChartMetaRecord undefined;
  undefined.meta.SHA256 = std::string(64, 'a');
  undefined.meta.LnMode = 0;
  undefined.meta.TotalLongNotes = 1;
  const auto undefinedPaths = musicSelectChartReplaySlotPaths(undefined, 2);
  require(undefinedPaths.has_value() &&
              (*undefinedPaths)[0].relativePath ==
                  std::filesystem::path("replay") /
                      ("C" + std::string(64, 'a') + ".brd") &&
              (*undefinedPaths)[2].relativePath ==
                  std::filesystem::path("replay") /
                      ("C" + std::string(64, 'a') + "_2.brd"),
          "undefined LN charts use the selected source LN prefix and slot suffix");
  touch(root / (*undefinedPaths)[0].relativePath);
  touch(root / (*undefinedPaths)[2].relativePath);
  require(musicSelectExistingChartReplaySlots(undefined, 2, root) ==
              std::array<bool, 4>{true, false, true, false},
          "slot availability is exactly filesystem existence for slots zero through three");

  ChartMetaRecord authored = undefined;
  authored.meta.LnMode = 2;
  const auto authoredPaths = musicSelectChartReplaySlotPaths(authored, 2);
  require(authoredPaths.has_value() &&
              (*authoredPaths)[0].relativePath ==
                  std::filesystem::path("replay") /
                      (std::string(64, 'a') + ".brd"),
          "authored LN charts do not use a selected-mode prefix");

  std::filesystem::remove_all(root);
}

void testFindsOnlyTheExactModernChartReference() {
  ChartMetaRecord record;
  record.meta.SHA256 = std::string(64, 'b');
  const auto paths = musicSelectChartReplaySlotPaths(record, 0);
  require(paths.has_value(), "canonical chart paths are available");
  if (!paths) return;

  ModernReplayFileInventoryEntry course{
      .owner = ModernReplayOwnerKind::CourseResult,
      .reference = {.resultId = 11, .identity = (*paths)[1]}};
  ModernReplayFileInventoryEntry chart{
      .owner = ModernReplayOwnerKind::ChartResult,
      .reference = {.resultId = 22, .identity = (*paths)[1]}};
  const std::array entries{course, chart};
  require(musicSelectChartReplayResultId(entries, (*paths)[1]) == 22,
          "slot lookup matches owner kind and the complete canonical path identity");
  require(!musicSelectChartReplayResultId(entries, (*paths)[3]).has_value(),
          "slot lookup never substitutes another replay history entry");
}

} // namespace

int main() {
  testUsesExactSourceChartPathsAndFilesExistsSemantics();
  testFindsOnlyTheExactModernChartReference();
  if (failures != 0) return 1;
  std::cout << "music-select replay slot tests passed\n";
  return 0;
}
