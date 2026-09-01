#include "music_select/MusicSelectPreview.h"

#include <iostream>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

MusicSelectPreviewSelection song(std::string id, std::string folder,
                                 std::string preview) {
  return {.id = std::move(id),
          .folder = std::move(folder),
          .previewPath = std::move(preview)};
}

void testExactSelectionDelayAndFolderTransition() {
  MusicSelectPreviewController controller;
  const auto first = song("one", "/songs/a", "/songs/a/preview.ogg");
  expect(!controller.selectedBarMoved(first, 1'000).stopAudio,
         "first selection does not stop absent preview audio");
  expect(!controller.update(401'000, false),
         "preview does not start at the exact 400ms boundary");
  auto started = controller.update(401'001, false);
  expect(started && started->path == first.previewPath,
         "preview starts strictly after Beatoraja's 400ms delay");

  const auto sibling = song("two", "/songs/a", "/songs/a/two.ogg");
  expect(!controller.selectedBarMoved(sibling, 500'000).stopAudio,
         "moving within one song folder preserves the current preview");
  expect(!controller.update(1'000'000, true),
         "a pending play action suppresses preview replacement");
  auto replaced = controller.update(1'000'000, false);
  expect(replaced && replaced->path == sibling.previewPath,
         "the sibling preview replaces audio once play is no longer pending");

  const auto other = song("three", "/songs/b", "");
  expect(controller.selectedBarMoved(other, 1'100'000).stopAudio,
         "moving to another folder stops the active preview immediately");
  auto noPreview = controller.update(1'500'001, false);
  expect(noPreview && !noPreview->path,
         "a song without PREVIEW switches to the selector default audio");
  expect(controller.selectedBarMoved(std::nullopt, 2'000'000).stopAudio,
         "moving from a song to a non-song stops preview state");
}
} // namespace

int main() {
  testExactSelectionDelayAndFolderTransition();
  if (failures != 0) return 1;
  std::cout << "music-select preview tests passed\n";
  return 0;
}
