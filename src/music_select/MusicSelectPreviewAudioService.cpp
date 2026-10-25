#include "MusicSelectPreview.h"
#include "../path.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

class MusicSelectPreviewAudioService::Impl {
public:
  explicit Impl(MusicSelectPreviewAudioService::AudioPort port,
                std::filesystem::path defaultPath)
      : port_(std::move(port)), defaultPath_(std::move(defaultPath)) {
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
    // Beatoraja's PreviewThread starts the looping SELECT BGM the moment the
    // worker starts (PreviewMusicProcessor.java:79-81); queue that initial
    // switch so the switchTo(nullopt) dedupe cannot swallow the default.
    {
      std::lock_guard lock(mutex_);
      ++requestSerial_;
    }
    condition_.notify_one();
  }

  ~Impl() {
    worker_.request_stop();
    {
      std::lock_guard lock(mutex_);
      if (loadCancellation_) {
        loadCancellation_->store(true, std::memory_order_release);
      }
    }
    condition_.notify_all();
  }

  void switchTo(std::optional<std::filesystem::path> path,
                std::filesystem::path chartPath, bool highlight) {
    {
      std::lock_guard lock(mutex_);
      // While the scene is silenced (paused, launching, or in error) no
      // navigation/preview request may re-cue the default BGM. The scene lifts
      // suppression explicitly on resume.
      if (suppressed_) return;
      if (requestedPath_ == path && requestedChartPath_ == chartPath &&
          requestedHighlight_ == highlight) return;
      if (loadCancellation_) {
        loadCancellation_->store(true, std::memory_order_release);
      }
      requestedPath_ = std::move(path);
      requestedChartPath_ = std::move(chartPath);
      requestedHighlight_ = highlight;
      ++requestSerial_;
    }
    condition_.notify_one();
  }

void silence() {
    std::lock_guard lock(mutex_);
    // Silence is sticky: it suppresses every later play request (including a
    // racing switchTo(nullopt)/resumeDefaultBgm) until the scene resumes, so
    // the select BGM can never be re-cued after a launch/pause begins.
    suppressed_ = true;
    // An empty path is distinct from the nullopt "play default" state, so the
    // worker maps an empty target to a stop with no subsequent default playback.
    const std::filesystem::path emptyPath;
    if (requestedPath_ == emptyPath) return;
    if (loadCancellation_) {
      loadCancellation_->store(true, std::memory_order_release);
    }
    requestedPath_ = emptyPath;
    requestedChartPath_.clear();
    ++requestSerial_;
    condition_.notify_one();
  }

  void silenceAndWait() {
    silence();
    std::unique_lock lock(mutex_);
    const auto serial = requestSerial_;
    condition_.wait(lock, [&] { return processedSerial_ >= serial; });
  }

void resumeDefaultBgm() {
    std::lock_guard lock(mutex_);
    // Returning to the nullopt state asks the worker to play the looping
    // default select BGM again.
    suppressed_ = false;
    if (!requestedPath_.has_value() && requestedChartPath_.empty()) return;
    if (loadCancellation_) {
      loadCancellation_->store(true, std::memory_order_release);
    }
    requestedPath_.reset();
    requestedChartPath_.clear();
    ++requestSerial_;
    condition_.notify_one();
  }

private:
  void acknowledge(std::uint64_t serial) {
    std::lock_guard lock(mutex_);
    processedSerial_ = serial;
    condition_.notify_all();
  }

  void run(std::stop_token stop) {
    std::uint64_t observedSerial = 0;
    std::optional<std::filesystem::path> playingPath;
    while (!stop.stop_requested()) {
      std::optional<std::filesystem::path> requested;
      std::uint64_t serial = 0;
      bool suppressed = false;
      std::filesystem::path chartPath;
      bool highlight = true;
      std::shared_ptr<std::atomic_bool> cancellation;
      {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] {
          return stop.stop_requested() || requestSerial_ != observedSerial;
        });
        if (stop.stop_requested()) break;
        serial = requestSerial_;
        requested = requestedPath_;
        chartPath = requestedChartPath_;
        highlight = requestedHighlight_;
        suppressed = suppressed_;
        cancellation = std::make_shared<std::atomic_bool>(false);
        loadCancellation_ = cancellation;
      }

      std::filesystem::path target = requested.value_or(defaultPath_);
      const bool canPlayChart = !chartPath.empty() && bool(port_.playChart);
      if (suppressed || (target.empty() && !canPlayChart)) {
        port_.stop();
        playingPath.reset();
        observedSerial = serial;
        acknowledge(serial);
        continue;
      }
      if (playingPath && *playingPath == target && (requested || !canPlayChart)) {
        observedSerial = serial;
        acknowledge(serial);
        continue;
      }

      SDL_Log("[select-audio] worker play target=%s",
              fspath_to_utf8(target).c_str());
      bool playedChart = !requested && canPlayChart;
      if (playedChart) port_.stop();
      bool ok = playedChart ? port_.playChart(chartPath, highlight, cancellation, stop)
                            : port_.play(target, true, cancellation, stop);
      SDL_Log("[select-audio] worker play result=%d", ok ? 1 : 0);
      bool stale = false;
      {
        std::lock_guard lock(mutex_);
        stale = cancellation->load(std::memory_order_acquire) ||
                requestSerial_ != serial || suppressed_ || stop.stop_requested();
      }
      if (!ok && !stale && canPlayChart && !playedChart) {
        port_.stop();
        playedChart = true;
        ok = port_.playChart(chartPath, highlight, cancellation, stop);
        std::lock_guard lock(mutex_);
        stale = cancellation->load(std::memory_order_acquire) ||
                requestSerial_ != serial || suppressed_ || stop.stop_requested();
      }
      if (!ok && !stale && (requested.has_value() || canPlayChart) &&
          !defaultPath_.empty() && (target != defaultPath_ || playedChart)) {
        playedChart = false;
        target = defaultPath_;
        ok = port_.play(target, true, cancellation, stop);
      }
      {
        std::lock_guard lock(mutex_);
        stale = cancellation->load(std::memory_order_acquire) ||
                requestSerial_ != serial || suppressed_ || stop.stop_requested();
        if (loadCancellation_ == cancellation) loadCancellation_.reset();
        if (ok && !stale && !playedChart) {
          playingPath = target;
        } else {
          playingPath.reset();
        }
      }
      if (ok && stale) port_.stop();
      observedSerial = serial;
      acknowledge(serial);
    }

    port_.stop();
  }

  AudioPort port_;
  std::filesystem::path defaultPath_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::optional<std::filesystem::path> requestedPath_;
  std::filesystem::path requestedChartPath_;
  bool requestedHighlight_ = true;
  std::uint64_t requestSerial_ = 0;
  std::uint64_t processedSerial_ = 0;
  bool suppressed_ = false;
  std::shared_ptr<std::atomic_bool> loadCancellation_;
  std::jthread worker_;
};

MusicSelectPreviewAudioService::MusicSelectPreviewAudioService(
    AudioPort port, std::filesystem::path defaultPath)
    : impl_(std::make_unique<Impl>(std::move(port), std::move(defaultPath))) {}

MusicSelectPreviewAudioService::~MusicSelectPreviewAudioService() = default;

void MusicSelectPreviewAudioService::switchTo(
    std::optional<std::filesystem::path> path, std::filesystem::path chartPath,
    bool highlight) {
  impl_->switchTo(std::move(path), std::move(chartPath), highlight);
}

void MusicSelectPreviewAudioService::silence() { impl_->silence(); }

void MusicSelectPreviewAudioService::silenceAndWait() { impl_->silenceAndWait(); }

void MusicSelectPreviewAudioService::resumeDefaultBgm() {
  impl_->resumeDefaultBgm();
}
