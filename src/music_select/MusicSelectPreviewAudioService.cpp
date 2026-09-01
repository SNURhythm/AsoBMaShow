#include "MusicSelectPreview.h"

#include "../audio/AudioWrapper.h"
#include "../path.h"

#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

class MusicSelectPreviewAudioService::Impl {
public:
  explicit Impl(AudioWrapper &audio) : audio_(&audio) {
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
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

private:
  void run(std::stop_token stop) {
    std::uint64_t observedSerial = 0;
    std::optional<audio::SkinSoundHandle> playing;
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

      if (requested == playingPath) {
        observedSerial = serial;
        continue;
      }
      if (playing) {
        (void)audio_->stopSkinSound(*playing);
        (void)audio_->disposeSkinSound(*playing);
        playing.reset();
        playingPath.reset();
      }
      if (!requested) {
        observedSerial = serial;
        continue;
      }

      const auto loaded = audio_->loadSkinSound(
          fspath_to_path_t(*requested), *cancellation,
          std::numeric_limits<std::size_t>::max(),
          std::numeric_limits<std::size_t>::max(), stop);
      bool stale = cancellation->load(std::memory_order_acquire);
      {
        std::lock_guard lock(mutex_);
        stale = stale || requestSerial_ != serial;
        if (loadCancellation_ == cancellation) loadCancellation_.reset();
      }
      if (!loaded.handle) {
        observedSerial = serial;
        continue;
      }
      if (stale || stop.stop_requested() ||
          !audio_->playSkinSound(*loaded.handle, 1.0F, true)) {
        (void)audio_->disposeSkinSound(*loaded.handle);
        observedSerial = serial;
        continue;
      }
      playing = loaded.handle;
      playingPath = std::move(requested);
      observedSerial = serial;
    }

    if (playing) {
      (void)audio_->stopSkinSound(*playing);
      (void)audio_->disposeSkinSound(*playing);
    }
  }

  AudioWrapper *audio_ = nullptr;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::optional<std::filesystem::path> requestedPath_;
  std::uint64_t requestSerial_ = 0;
  std::shared_ptr<std::atomic_bool> loadCancellation_;
  std::jthread worker_;
};

MusicSelectPreviewAudioService::MusicSelectPreviewAudioService(
    AudioWrapper &audio)
    : impl_(std::make_unique<Impl>(audio)) {}

MusicSelectPreviewAudioService::~MusicSelectPreviewAudioService() = default;

void MusicSelectPreviewAudioService::switchTo(
    std::optional<std::filesystem::path> path) {
  impl_->switchTo(std::move(path));
}
