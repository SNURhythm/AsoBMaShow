#include "music_select/MusicSelectPreview.h"

#include "music_select_runtime_ledger_assertions.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class RecordingPreviewPort {
public:
  struct PlayCall {
    std::filesystem::path path;
    bool loop = false;
  };

  [[nodiscard]] MusicSelectPreviewAudioService::AudioPort port() {
    return {
        [this](const std::filesystem::path &path, bool loop,
               const std::shared_ptr<std::atomic_bool> &, std::stop_token) {
          std::lock_guard lock(mutex_);
          calls_.push_back({path, loop});
          condition_.notify_all();
          return true;
        },
        [this]() {
          std::lock_guard lock(mutex_);
          ++stopped_;
          condition_.notify_all();
        }};
  }

  bool waitForPlayCount(std::size_t count,
                        std::int64_t timeoutMillis = 2000) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::milliseconds(timeoutMillis),
                               [&] { return calls_.size() >= count; });
  }

  bool waitForStopCount(int count, std::int64_t timeoutMillis = 2000) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::milliseconds(timeoutMillis),
                               [&] { return stopped_ >= count; });
  }

  [[nodiscard]] std::vector<PlayCall> takeCalls() {
    std::lock_guard lock(mutex_);
    return calls_;
  }

  [[nodiscard]] int stopped() {
    std::lock_guard lock(mutex_);
    return stopped_;
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<PlayCall> calls_;
  int stopped_ = 0;
};

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

void testFloatWriterObservesThePostWriteSelection() {
  MusicSelectPreviewController controller;
  const auto first = song("one", "/songs/a", "/songs/a/preview.ogg");
  (void)controller.selectedBarMoved(first, 1'000);
  expect(controller.update(401'001, false).has_value(),
         "writer fixture starts its first preview");

  (void)controller.selectedBarMoved(first, 500'000);
  const auto other = song("two", "/songs/b", "/songs/b/preview.ogg");
  controller.observeSelection(other, 500'000);
  expect(!controller.update(900'000, false),
         "post-write selection waits at the exact 400ms boundary");
  const auto replaced = controller.update(900'001, false);
  expect(replaced && replaced->path == other.previewPath,
         "the frame loop starts the post-write song after the old selection "
         "reset the timer");

  (void)controller.selectedBarMoved(other, 1'000'000);
  controller.observeSelection(std::nullopt, 1'000'000);
  expect(!controller.update(2'000'000, false),
         "a writer landing on a non-SongBar leaves preview audio running");
}

void testEmptyPreviewWithinFolderRoutesBackToDefaultAudio() {
  MusicSelectPreviewController controller;
  const auto first = song("one", "/songs/a", "/songs/a/preview.ogg");
  (void)controller.selectedBarMoved(first, 1'000);
  const auto started = controller.update(401'001, false);
  expect(started && started->path == first.previewPath,
         "the sibling preview starts before the empty-preview move");

  const auto blank = song("two", "/songs/a", "");
  const auto move = controller.selectedBarMoved(blank, 500'000);
  expect(!move.stopAudio,
         "moving to an empty-preview sibling keeps the folder's audio state");
  const auto switched = controller.update(900'001, false);
  expect(switched && !switched->path,
         "an empty preview selection routes back to the default select BGM");
}

void testArchiveGatedSelectionDefersToDefaultAudio() {
  MusicSelectPreviewController controller;
  const auto gated = song("archive", "/archives/x", "");
  (void)controller.selectedBarMoved(gated, 0);
  const auto switched = controller.update(401'001, false);
  expect(switched && !switched->path,
         "an archive-gated chart (archiveChartPreviewEnabled off) previews "
         "the default select BGM only");
}

void testNoPreviewFallsBackToDefaultSelectBgm() {
  const std::filesystem::path defaultBgm = "/assets/select.wav";
  RecordingPreviewPort port;
  MusicSelectPreviewAudioService service(port.port(), defaultBgm);
  service.switchTo(std::nullopt);
  expect(port.waitForPlayCount(1),
         "the preview worker starts the default select BGM when idle");
  const auto calls = port.takeCalls();
  expect(calls.size() == 1 && calls[0].path == defaultBgm && calls[0].loop,
         "a chart with no preview plays the default select BGM looped");
}

void testEmptySelectionSwitchRoutesBackToDefault() {
  const std::filesystem::path defaultBgm = "/assets/select.wav";
  const std::filesystem::path preview = "/songs/a/preview.ogg";
  RecordingPreviewPort port;
  MusicSelectPreviewAudioService service(port.port(), defaultBgm);
  expect(port.waitForPlayCount(1),
         "the default select BGM starts with the select screen");
  service.switchTo(preview);
  expect(port.waitForPlayCount(2),
         "selecting a chart with a preview starts the preview");
  service.switchTo(std::nullopt);
  expect(port.waitForPlayCount(3),
         "moving to an empty preview returns audio to the default");
  const auto calls = port.takeCalls();
  expect(calls.size() == 3 && calls[0].path == defaultBgm,
         "the worker begins on the default select BGM");
  expect(calls.size() == 3 && calls[1].path == preview && calls[1].loop,
         "the chart preview plays looped");
  expect(calls.size() == 3 && calls[2].path == defaultBgm,
         "the empty-preview switch reroutes to the default select BGM");
}

void testIdleWithoutDefaultStaysSilent() {
  RecordingPreviewPort port;
  MusicSelectPreviewAudioService service(port.port());
  service.switchTo(std::nullopt);
  expect(port.waitForStopCount(1),
         "an idle switch with no default asks the boundary to stop");
  expect(port.takeCalls().empty(),
         "no default BGM path and no preview request plays nothing");
}
} // namespace

int main(int argc, char **argv) {
  testExactSelectionDelayAndFolderTransition();
  testFloatWriterObservesThePostWriteSelection();
  testEmptyPreviewWithinFolderRoutesBackToDefaultAudio();
  testArchiveGatedSelectionDefersToDefaultAudio();
  testNoPreviewFallsBackToDefaultSelectBgm();
  testEmptySelectionSwitchRoutesBackToDefault();
  testIdleWithoutDefaultStaysSilent();
  return music_select_runtime_ledger_assertions::finish(
      argc, argv, "music_select_preview_tests", failures,
      "music-select preview assertion(s) failed",
      "music-select preview tests passed");
}
