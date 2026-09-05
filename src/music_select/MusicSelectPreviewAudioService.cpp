#include "MusicSelectPreview.h"

#include "../audio/SelectAudioDiagnostics.h"
#include <SDL2/SDL.h>

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

  void switchTo(std::optional<std::filesystem::path> path) {
    {
      std::lock_guard lock(mutex_);
      if (requestedPath_ == path) return;
      if (loadCancellation_) {
        loadCancellation_->store(true, std::memory_order_release);
      }
      requestedPath_ = std::move(path);
      ++requestSerial_;
    }
    condition_.notify_one();
  }

void silence() {
    std::lock_guard lock(mutex_);
    // An empty path is distinct from the nullopt "play default" state, so the
    // worker maps an empty target to a stop with no subsequent default playback.
    const std::filesystem::path emptyPath;
    if (requestedPath_ == emptyPath) return;
    if (loadCancellation_) {
      loadCancellation_->store(true, std::memory_order_release);
    }
    requestedPath_ = emptyPath;
    ++requestSerial_;
    condition_.notify_one();
  }

void resumeDefaultBgm() {
    std::lock_guard lock(mutex_);
    // Returning to the nullopt state asks the worker to play the looping
    // default select BGM again.
    if (!requestedPath_.has_value()) return;
    if (loadCancellation_) {
      loadCancellation_->store(true, std::memory_order_release);
    }
    requestedPath_.reset();
    ++requestSerial_;
    condition_.notify_one();
  }

private:
  void run(std::stop_token stop) {
    audio::diag::SelectAudioLog("[bgm] worker started");
    std::uint64_t observedSerial = 0;
    std::optional<std::filesystem::path> playingPath;
    while (!stop.stop_requested()) {
      std::optional<std::filesystem::path> requested;
      std::uint64_t serial = 0;
      std::shared_ptr<std::atomic_bool> cancellation;
      {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] {
          return stop.stop_requested() || requestSerial_ != observedSerial;
        });
        if (stop.stop_requested()) break;
        serial = requestSerial_;
        requested = requestedPath_;
        cancellation = std::make_shared<std::atomic_bool>(false);
        loadCancellation_ = cancellation;
      }
      audio::diag::SelectAudioLog("[bgm] worker woke serial=" +
                                  std::to_string(serial));

      const std::filesystem::path target = requested.value_or(defaultPath_);
      if (target.empty()) {
        port_.stop();
        playingPath.reset();
        observedSerial = serial;
        continue;
      }
      if (playingPath && *playingPath == target) {
        observedSerial = serial;
        continue;
      }

      audio::diag::SelectAudioLog("[bgm] worker target=" + target.string());
      SDL_Log("[select-audio] worker play target=%s",
              target.string().c_str());
      const bool ok = port_.play(target, true, cancellation, stop);
      SDL_Log("[select-audio] worker play result=%d", ok ? 1 : 0);
      audio::diag::SelectAudioLog(std::string("[bgm] worker play result=") +
                                  (ok ? "ok" : "FAILED"));
      bool stale = cancellation->load(std::memory_order_acquire);
      {
        std::lock_guard lock(mutex_);
        stale = stale || requestSerial_ != serial;
        if (loadCancellation_ == cancellation) loadCancellation_.reset();
      }
      if (ok && !stale && !stop.stop_requested()) {
        playingPath = target;
      } else {
        playingPath.reset();
      }
      observedSerial = serial;
    }

    port_.stop();
  }

  AudioPort port_;
  std::filesystem::path defaultPath_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::optional<std::filesystem::path> requestedPath_;
  std::uint64_t requestSerial_ = 0;
  std::shared_ptr<std::atomic_bool> loadCancellation_;
  std::jthread worker_;
};

MusicSelectPreviewAudioService::MusicSelectPreviewAudioService(
    AudioPort port, std::filesystem::path defaultPath)
    : impl_(std::make_unique<Impl>(std::move(port), std::move(defaultPath))) {}

MusicSelectPreviewAudioService::~MusicSelectPreviewAudioService() = default;

void MusicSelectPreviewAudioService::switchTo(
    std::optional<std::filesystem::path> path) {
  impl_->switchTo(std::move(path));
}

void MusicSelectPreviewAudioService::silence() { impl_->silence(); }

void MusicSelectPreviewAudioService::resumeDefaultBgm() {
  impl_->resumeDefaultBgm();
}