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
    std::shared_ptr<std::atomic_bool> cancellation;
  };

  explicit RecordingPreviewPort(
      MusicSelectPreviewAudioService::PlayRequest play = {})
      : play_(std::move(play)) {}

  [[nodiscard]] MusicSelectPreviewAudioService::AudioPort port() {
    return {
        [this](const std::filesystem::path &path, bool loop,
               const std::shared_ptr<std::atomic_bool> &cancellation,
               std::stop_token stop) {
          {
            std::lock_guard lock(mutex_);
            calls_.push_back({path, loop, cancellation});
            condition_.notify_all();
          }
          return play_ ? play_(path, loop, cancellation, stop) : true;
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
  MusicSelectPreviewAudioService::PlayRequest play_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<PlayCall> calls_;
  int stopped_ = 0;
};

class PreviewLoadGate {
public:
  void wait() {
    std::unique_lock lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [&] { return released_; });
  }

  bool waitUntilEntered() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(2),
                               [&] { return entered_; });
  }

  void release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
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

void testSilenceStopsWithoutStartingDefaultBgm() {
  const std::filesystem::path defaultBgm = "/assets/select.wav";
  const std::filesystem::path preview = "/songs/a/preview.ogg";
  RecordingPreviewPort port;
  MusicSelectPreviewAudioService service(port.port(), defaultBgm);
  expect(port.waitForPlayCount(1),
         "the default select BGM starts with the select screen");
  service.switchTo(preview);
  expect(port.waitForPlayCount(2),
         "selecting a chart with a preview starts the preview");
  service.silence();
  expect(port.waitForStopCount(1),
         "silence asks the boundary to stop playback");
  const auto calls = port.takeCalls();
  expect(calls.size() == 2 && calls[0].path == defaultBgm,
         "silence leaves the default select BGM unplayed");
  expect(calls.size() == 2 && calls[1].path == preview,
         "silence leaves the preview stopped and does not route to the default");
}

void testResumeAfterSilenceRestartsDefaultBgm() {
  const std::filesystem::path defaultBgm = "/assets/select.wav";
  RecordingPreviewPort port;
  MusicSelectPreviewAudioService service(port.port(), defaultBgm);
  expect(port.waitForPlayCount(1),
         "the default select BGM starts with the select screen");
  service.silence();
  expect(port.waitForStopCount(1),
         "silence stops the default BGM");
  const auto afterSilence = port.takeCalls();
  expect(afterSilence.size() == 1 && afterSilence[0].path == defaultBgm,
         "silence does not start the default BGM");

  service.resumeDefaultBgm();
  expect(port.waitForPlayCount(2),
         "resuming after silence replays the default select BGM");
  const auto calls = port.takeCalls();
  expect(calls.size() == 2 && calls[0].path == defaultBgm &&
             calls[1].path == defaultBgm && calls[1].loop,
         "resumeDefaultBgm routes back to the looping default select BGM");
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

void testSilenceSuppressesReCueFromLaterDefaultSwitch() {
  const std::filesystem::path defaultBgm = "/assets/select.wav";
  RecordingPreviewPort port;
  MusicSelectPreviewAudioService service(port.port(), defaultBgm);
  expect(port.waitForPlayCount(1),
         "the default select BGM starts with the select screen");
  service.silence();
  expect(port.waitForStopCount(1),
         "silence stops the default BGM");
  service.switchTo(std::nullopt);
  // A racing switchTo(nullopt) after a launch/pause began must not re-cue the
  // default BGM (it previously did, bleeding the select BGM into gameplay).
  service.resumeDefaultBgm();
  expect(port.waitForPlayCount(2),
         "resumeDefaultBgm after silence replays the select BGM");
  service.silence();
  expect(port.waitForStopCount(2),
         "re-silencing stops the resumed BGM");
  const auto calls = port.takeCalls();
  expect(calls.size() == 2 && calls[0].path == defaultBgm &&
             calls[1].path == defaultBgm && calls[1].loop,
         "only the initial default and the explicit resume play; the racing "
         "switchTo stayed suppressed");
}

void testFailedPreviewRestoresDefaultBgm() {
  const std::filesystem::path defaultBgm = "/assets/select.wav";
  const std::filesystem::path preview = "/songs/a/missing.ogg";
  RecordingPreviewPort port(
      [&](const auto &path, bool, const auto &, std::stop_token) {
        return path != preview;
      });
  MusicSelectPreviewAudioService service(port.port(), defaultBgm);
  expect(port.waitForPlayCount(1), "failure fixture starts the default BGM");
  service.switchTo(preview);
  expect(port.waitForPlayCount(3),
         "a failed preview explicitly restores the stopped default BGM");
  const auto calls = port.takeCalls();
  expect(calls.size() == 3 && calls[1].path == preview &&
             calls[2].path == defaultBgm && calls[2].loop,
         "a genuine preview failure falls back once to the looping default");
  expect(calls.size() == 3 &&
             calls[1].cancellation == calls[2].cancellation,
         "fallback retains the failed preview's cancellation token");
  service.switchTo(preview);
  expect(!port.waitForPlayCount(4, 50),
         "the same failed selection does not repeatedly retry its preview");
  service.switchTo(std::nullopt);
  expect(!port.waitForPlayCount(4, 50),
         "a successful fallback is tracked as playing and is not restarted");
}

void testFailedPreviewDoesNotRetryFailedDefault() {
  const std::filesystem::path defaultBgm = "/assets/select.wav";
  RecordingPreviewPort port(
      [](const auto &, bool, const auto &, std::stop_token) { return false; });
  MusicSelectPreviewAudioService service(port.port(), defaultBgm);
  expect(port.waitForPlayCount(1), "the missing default is attempted initially");
  expect(!port.waitForPlayCount(2, 50), "a failed default does not retry itself");
  service.switchTo("/songs/a/corrupt.ogg");
  expect(port.waitForPlayCount(3),
         "a failed preview attempts its default fallback even after initial failure");
  expect(!port.waitForPlayCount(4, 50),
         "a failed fallback does not start a default retry loop");
  service.switchTo(std::nullopt);
  expect(port.waitForPlayCount(4),
         "an explicit default request retries because failed fallback is not playing");
  expect(!port.waitForPlayCount(5, 50),
         "the explicit failed default is attempted only once");
}

void testFailedPreviewWithoutDefaultStaysSilent() {
  RecordingPreviewPort port(
      [](const auto &, bool, const auto &, std::stop_token) { return false; });
  MusicSelectPreviewAudioService service(port.port());
  expect(port.waitForStopCount(1), "no-default fixture starts silent");
  service.switchTo("/songs/a/unsupported.audio");
  expect(port.waitForPlayCount(1), "the unsupported preview is attempted");
  expect(!port.waitForPlayCount(2, 50),
         "preview failure never attempts an empty fallback path");
}

void testPreviewMatchingFailedDefaultIsNotRetried() {
  const std::filesystem::path defaultBgm = "/assets/select.wav";
  RecordingPreviewPort port(
      [](const auto &, bool, const auto &, std::stop_token) { return false; });
  MusicSelectPreviewAudioService service(port.port(), defaultBgm);
  expect(port.waitForPlayCount(1), "same-path fixture tries the default BGM");
  service.switchTo(defaultBgm);
  expect(port.waitForPlayCount(2), "the explicit same-path preview is attempted");
  expect(!port.waitForPlayCount(3, 50),
         "a preview equal to the failed default does not retry the same file");
}

void testCancelledPreviewDoesNotRestoreDefault() {
  const std::filesystem::path defaultBgm = "/assets/select.wav";
  const std::filesystem::path preview = "/songs/a/preview.ogg";
  const std::filesystem::path latest = "/songs/c/preview.ogg";
  PreviewLoadGate gate;
  RecordingPreviewPort port(
      [&](const auto &path, bool, const auto &, std::stop_token) {
        if (path == preview) {
          gate.wait();
          return false;
        }
        return true;
      });
  MusicSelectPreviewAudioService service(port.port(), defaultBgm);
  expect(port.waitForPlayCount(1), "navigation fixture starts the default BGM");
  service.switchTo(preview);
  expect(gate.waitUntilEntered(), "preview load blocks before cancellation");
  service.switchTo("/songs/b/preview.ogg");
  service.switchTo(latest);
  gate.release();
  expect(port.waitForPlayCount(3), "rapid navigation plays the latest preview");
  const auto calls = port.takeCalls();
  expect(calls.size() == 3 && calls[2].path == latest &&
             calls[1].cancellation->load(),
         "cancelled preview skips default fallback and intermediate selections");
}

void testSilencedFailedPreviewDoesNotRestoreDefault() {
  const std::filesystem::path defaultBgm = "/assets/select.wav";
  const std::filesystem::path preview = "/songs/a/preview.ogg";
  PreviewLoadGate gate;
  RecordingPreviewPort port(
      [&](const auto &path, bool, const auto &, std::stop_token) {
        if (path == preview) {
          gate.wait();
          return false;
        }
        return true;
      });
  MusicSelectPreviewAudioService service(port.port(), defaultBgm);
  expect(port.waitForPlayCount(1), "silence fixture starts the default BGM");
  service.switchTo(preview);
  expect(gate.waitUntilEntered(), "preview load blocks before backgrounding");
  service.silence();
  service.switchTo(std::nullopt);
  gate.release();
  expect(port.waitForStopCount(1), "backgrounded request stops audio");
  expect(port.takeCalls().size() == 2,
         "a failed load during silence never restores the default BGM");
}

void testSilenceCancelsInFlightFallback() {
  const std::filesystem::path defaultBgm = "/assets/select.wav";
  PreviewLoadGate gate;
  int defaults = 0;
  RecordingPreviewPort port(
      [&](const auto &path, bool, const auto &, std::stop_token) {
        if (path != defaultBgm) return false;
        if (++defaults == 2) gate.wait();
        return true;
      });
  MusicSelectPreviewAudioService service(port.port(), defaultBgm);
  expect(port.waitForPlayCount(1), "fallback cancellation starts the default");
  service.switchTo("/songs/a/missing.ogg");
  expect(gate.waitUntilEntered(), "failed preview enters its default fallback");
  service.silence();
  const auto calls = port.takeCalls();
  expect(calls.size() == 3 && calls[2].cancellation->load(),
         "silence cancels the in-flight fallback through its retained token");
  gate.release();
  expect(port.waitForStopCount(1), "cancelled fallback playback is stopped");
  service.resumeDefaultBgm();
  expect(port.waitForPlayCount(4),
         "explicit resume replays default instead of deduplicating stale fallback");
}

void testNavigationCleansUpStaleFallback() {
  const std::filesystem::path defaultBgm = "/assets/select.wav";
  PreviewLoadGate gate;
  int defaults = 0;
  RecordingPreviewPort port(
      [&](const auto &path, bool, const auto &, std::stop_token) {
        if (path != defaultBgm) return false;
        if (++defaults == 2) gate.wait();
        return true;
      });
  MusicSelectPreviewAudioService service(port.port(), defaultBgm);
  expect(port.waitForPlayCount(1), "stale fallback fixture starts the default");
  service.switchTo("/songs/a/missing.ogg");
  expect(gate.waitUntilEntered(), "navigation waits for the fallback load");
  const int stopsBeforeNavigation = port.stopped();
  service.switchTo(std::nullopt);
  gate.release();
  expect(port.waitForPlayCount(4),
         "new default request is not deduplicated against a cancelled fallback");
  const auto calls = port.takeCalls();
  expect(calls.size() == 4 && calls[2].cancellation->load() &&
             calls[2].cancellation != calls[3].cancellation,
         "navigation cancels fallback and gives the new default its own token");
  expect(port.stopped() > stopsBeforeNavigation,
         "a successful but stale fallback is stopped before processing navigation");
}

void testShutdownCancelsPreviewAndFallback() {
  const std::filesystem::path defaultBgm = "/assets/select.wav";
  for (const bool blockFallback : {false, true}) {
    PreviewLoadGate gate;
    int defaults = 0;
    RecordingPreviewPort port(
        [&](const auto &path, bool, const auto &, std::stop_token stop) {
          if (path == defaultBgm && ++defaults == 1) return true;
          if ((path == defaultBgm) == blockFallback) {
            std::stop_callback releaseOnStop(stop, [&] { gate.release(); });
            gate.wait();
          }
          return false;
        });
    auto service = std::make_unique<MusicSelectPreviewAudioService>(
        port.port(), defaultBgm);
    expect(port.waitForPlayCount(1), "shutdown fixture starts the default BGM");
    service->switchTo("/songs/a/missing.ogg");
    expect(gate.waitUntilEntered(), "shutdown interrupts an in-flight load");
    service.reset();
    const auto calls = port.takeCalls();
    expect(calls.size() == (blockFallback ? 3 : 2) &&
               calls.back().cancellation->load(),
           "shutdown cancels the active request without further fallback attempts");
    expect(port.stopped() >= 1, "shutdown stops preview worker audio");
  }
}
} // namespace

int main(int argc, char **argv) {
  testExactSelectionDelayAndFolderTransition();
  testFloatWriterObservesThePostWriteSelection();
  testEmptyPreviewWithinFolderRoutesBackToDefaultAudio();
  testArchiveGatedSelectionDefersToDefaultAudio();
  testNoPreviewFallsBackToDefaultSelectBgm();
  testEmptySelectionSwitchRoutesBackToDefault();
  testSilenceStopsWithoutStartingDefaultBgm();
  testResumeAfterSilenceRestartsDefaultBgm();
  testIdleWithoutDefaultStaysSilent();
  testSilenceSuppressesReCueFromLaterDefaultSwitch();
  testFailedPreviewRestoresDefaultBgm();
  testFailedPreviewDoesNotRetryFailedDefault();
  testFailedPreviewWithoutDefaultStaysSilent();
  testPreviewMatchingFailedDefaultIsNotRetried();
  testCancelledPreviewDoesNotRestoreDefault();
  testSilencedFailedPreviewDoesNotRestoreDefault();
  testSilenceCancelsInFlightFallback();
  testNavigationCleansUpStaleFallback();
  testShutdownCancelsPreviewAndFallback();
  return music_select_runtime_ledger_assertions::finish(
      argc, argv, "music_select_preview_tests", failures,
      "music-select preview assertion(s) failed",
      "music-select preview tests passed");
}
