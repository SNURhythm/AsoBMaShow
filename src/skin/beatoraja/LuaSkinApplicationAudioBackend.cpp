#include "LuaSkinApplicationAudioBackend.h"

#include "../../audio/AudioWrapper.h"
#include "../../path.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <stop_token>
#include <utility>

namespace skin {
namespace {

class ApplicationLuaSkinAudioBackend final : public LuaSkinAudioBackend {
public:
  ApplicationLuaSkinAudioBackend(AudioWrapper &audio,
                                 std::function<float()> systemVolume,
                                 LuaSkinApplicationAudioLimits limits,
                                 std::shared_ptr<SkinLiveResourceCounters> counters)
      : audio_(&audio), systemVolume_(std::move(systemVolume)),
        limits_(limits), liveCounters_(std::move(counters)) {}

  ~ApplicationLuaSkinAudioBackend() override {
    retryPendingControls();
    for (auto found = sounds_.begin(); found != sounds_.end();) {
      if (audio_->retireSkinSoundForTeardown(found->second.handle)) {
        if (liveCounters_) {
          liveCounters_->audioDestroyed(found->second.decodedBytes);
        }
        found = sounds_.erase(found);
      } else {
        ++found;
      }
    }
  }

  float systemVolume() const noexcept override {
    if (!systemVolume_) {
      return 1.0F;
    }
    try {
      return systemVolume_();
    } catch (...) {
      return 0.0F;
    }
  }

  std::optional<LuaSkinAudioIdentity>
  load(const std::filesystem::path &path,
       std::stop_token stop) noexcept override {
    ++loadAttempts_;
    try {
      retryPendingControls();
      if (stop.stop_requested() || sounds_.size() >= limits_.maximumIdentities) {
        return std::nullopt;
      }
      const path_t backendPath = fspath_to_path_t(path);
      std::error_code sizeError;
      const std::uintmax_t measuredSize =
          std::filesystem::file_size(path, sizeError);
      const std::size_t encodedBytes =
          sizeError
              ? 0
              : static_cast<std::size_t>(std::min<std::uintmax_t>(
                    measuredSize, std::numeric_limits<std::size_t>::max()));
      if (encodedBytes > limits_.maximumEncodedBytes ||
          encodedBytesTotal_ > limits_.maximumEncodedBytes - encodedBytes) {
        return std::nullopt;
      }
      std::atomic_bool cancelled = stop.stop_requested();
      std::stop_callback cancellation(stop,
                                      [&cancelled] { cancelled = true; });
      const auto loaded = audio_->loadSkinSound(
          backendPath, cancelled,
          limits_.maximumEncodedBytes - encodedBytesTotal_,
          limits_.maximumDecodedBytes, stop);
      if (!loaded.handle || cancelled) {
        if (loaded.handle) {
          (void)audio_->disposeSkinSound(*loaded.handle);
        }
        return std::nullopt;
      }
      LuaSkinAudioIdentity identity{.value = ++nextIdentity_};
      if (!identity) {
        identity.value = ++nextIdentity_;
      }
      auto [inserted, unique] = sounds_.emplace(
          identity, SoundRecord{.handle = *loaded.handle,
                                .encodedBytes = encodedBytes,
                                .decodedBytes = loaded.decodedBytes});
      if (!unique) {
        (void)audio_->disposeSkinSound(*loaded.handle);
        return std::nullopt;
      }
      encodedBytesTotal_ += inserted->second.encodedBytes;
      decodedBytesTotal_ += inserted->second.decodedBytes;
      if (liveCounters_) {
        liveCounters_->audioCreated(inserted->second.decodedBytes);
      }
      ++loadsSucceeded_;
      return identity;
    } catch (...) {
      return std::nullopt;
    }
  }

  LuaSkinAudioActivityCounters activityCounters() const noexcept override {
    return {.loadAttempts = loadAttempts_,
            .loadsSucceeded = loadsSucceeded_,
            .liveIdentities = sounds_.size()};
  }

  void play(LuaSkinAudioIdentity identity, float volume,
            bool loop) noexcept override {
    retryPendingControls();
    if (const auto found = sounds_.find(identity); found != sounds_.end()) {
      if (found->second.pendingControl != PendingControl::None) {
        return;
      }
      (void)audio_->playSkinSound(found->second.handle, volume, loop);
    }
  }

  void stop(LuaSkinAudioIdentity identity) noexcept override {
    retryPendingControls();
    if (const auto found = sounds_.find(identity); found != sounds_.end()) {
      if (found->second.pendingControl == PendingControl::Dispose) {
        return;
      }
      if (audio_->stopSkinSound(found->second.handle)) {
        found->second.pendingControl = PendingControl::None;
      } else {
        found->second.pendingControl = PendingControl::Stop;
      }
    }
  }

  void dispose(LuaSkinAudioIdentity identity) noexcept override {
    retryPendingControls();
    if (const auto found = sounds_.find(identity); found != sounds_.end()) {
      if (audio_->disposeSkinSound(found->second.handle)) {
        if (liveCounters_) {
          liveCounters_->audioDestroyed(found->second.decodedBytes);
        }
        encodedBytesTotal_ -= found->second.encodedBytes;
        decodedBytesTotal_ -= found->second.decodedBytes;
        sounds_.erase(found);
      } else {
        found->second.pendingControl = PendingControl::Dispose;
      }
    }
  }

private:
  enum class PendingControl : std::uint8_t { None, Stop, Dispose };

  struct SoundRecord {
    audio::SkinSoundHandle handle;
    std::size_t encodedBytes = 0;
    std::size_t decodedBytes = 0;
    PendingControl pendingControl = PendingControl::None;
  };

  void retryPendingControls() noexcept {
    for (auto found = sounds_.begin(); found != sounds_.end();) {
      if (found->second.pendingControl == PendingControl::None) {
        ++found;
        continue;
      }
      if (found->second.pendingControl == PendingControl::Stop) {
        if (audio_->stopSkinSound(found->second.handle)) {
          found->second.pendingControl = PendingControl::None;
        }
        ++found;
        continue;
      }
      if (!audio_->disposeSkinSound(found->second.handle)) {
        ++found;
        continue;
      }
      encodedBytesTotal_ -= found->second.encodedBytes;
      decodedBytesTotal_ -= found->second.decodedBytes;
      if (liveCounters_) {
        liveCounters_->audioDestroyed(found->second.decodedBytes);
      }
      found = sounds_.erase(found);
    }
  }

  AudioWrapper *audio_ = nullptr;
  std::function<float()> systemVolume_;
  LuaSkinApplicationAudioLimits limits_;
  std::map<LuaSkinAudioIdentity, SoundRecord> sounds_;
  std::size_t encodedBytesTotal_ = 0;
  std::size_t decodedBytesTotal_ = 0;
  std::uint64_t nextIdentity_ = 0;
  std::uint64_t loadAttempts_ = 0;
  std::uint64_t loadsSucceeded_ = 0;
  std::shared_ptr<SkinLiveResourceCounters> liveCounters_;
};

class NoOutputLuaSkinAudioBackend final : public LuaSkinAudioBackend {
public:
  explicit NoOutputLuaSkinAudioBackend(
      std::shared_ptr<SkinLiveResourceCounters> counters)
      : liveCounters_(std::move(counters)) {}
  ~NoOutputLuaSkinAudioBackend() override {
    if (liveCounters_) {
      for (std::size_t index = 0; index < identities_.size(); ++index) {
        liveCounters_->audioDestroyed(0);
      }
    }
  }
  float systemVolume() const noexcept override { return 0.0F; }

  std::optional<LuaSkinAudioIdentity>
  load(const std::filesystem::path &, std::stop_token stop) noexcept override {
    ++loadAttempts_;
    if (stop.stop_requested() || identities_.size() >= 256) {
      return std::nullopt;
    }
    LuaSkinAudioIdentity identity{.value = ++nextIdentity_};
    if (!identity) {
      identity.value = ++nextIdentity_;
    }
    identities_.emplace(identity, true);
    if (liveCounters_) liveCounters_->audioCreated(0);
    ++loadsSucceeded_;
    return identity;
  }

  void play(LuaSkinAudioIdentity, float, bool) noexcept override {}
  void stop(LuaSkinAudioIdentity) noexcept override {}
  void dispose(LuaSkinAudioIdentity identity) noexcept override {
    if (identities_.erase(identity) != 0 && liveCounters_) {
      liveCounters_->audioDestroyed(0);
    }
  }

  LuaSkinAudioActivityCounters activityCounters() const noexcept override {
    return {.loadAttempts = loadAttempts_,
            .loadsSucceeded = loadsSucceeded_,
            .liveIdentities = identities_.size()};
  }

private:
  std::map<LuaSkinAudioIdentity, bool> identities_;
  std::uint64_t nextIdentity_ = 0;
  std::uint64_t loadAttempts_ = 0;
  std::uint64_t loadsSucceeded_ = 0;
  std::shared_ptr<SkinLiveResourceCounters> liveCounters_;
};

} // namespace

std::shared_ptr<LuaSkinAudioBackend>
createLuaSkinApplicationAudioBackend(
    AudioWrapper &audio, std::function<float()> systemVolume,
    LuaSkinApplicationAudioLimits limits,
    std::shared_ptr<SkinLiveResourceCounters> counters) {
  return std::make_shared<ApplicationLuaSkinAudioBackend>(
      audio, std::move(systemVolume), limits, std::move(counters));
}

std::shared_ptr<LuaSkinAudioBackend> createLuaSkinNoOutputAudioBackend(
    std::shared_ptr<SkinLiveResourceCounters> counters) {
  return std::make_shared<NoOutputLuaSkinAudioBackend>(std::move(counters));
}

} // namespace skin
