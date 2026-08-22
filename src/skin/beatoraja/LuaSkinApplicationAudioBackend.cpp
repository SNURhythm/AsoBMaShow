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
                                 LuaSkinApplicationAudioLimits limits)
      : audio_(&audio), systemVolume_(std::move(systemVolume)),
        limits_(limits) {}

  ~ApplicationLuaSkinAudioBackend() override {
    for (const auto &[identity, record] : sounds_) {
      (void)identity;
      (void)audio_->disposeSkinSound(record.handle);
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
    try {
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
      return identity;
    } catch (...) {
      return std::nullopt;
    }
  }

  void play(LuaSkinAudioIdentity identity, float volume,
            bool loop) noexcept override {
    if (const auto found = sounds_.find(identity); found != sounds_.end()) {
      (void)audio_->playSkinSound(found->second.handle, volume, loop);
    }
  }

  void stop(LuaSkinAudioIdentity identity) noexcept override {
    if (const auto found = sounds_.find(identity); found != sounds_.end()) {
      (void)audio_->stopSkinSound(found->second.handle);
    }
  }

  void dispose(LuaSkinAudioIdentity identity) noexcept override {
    if (const auto found = sounds_.find(identity); found != sounds_.end()) {
      (void)audio_->disposeSkinSound(found->second.handle);
      encodedBytesTotal_ -= found->second.encodedBytes;
      decodedBytesTotal_ -= found->second.decodedBytes;
      sounds_.erase(found);
    }
  }

private:
  struct SoundRecord {
    audio::SkinSoundHandle handle;
    std::size_t encodedBytes = 0;
    std::size_t decodedBytes = 0;
  };

  AudioWrapper *audio_ = nullptr;
  std::function<float()> systemVolume_;
  LuaSkinApplicationAudioLimits limits_;
  std::map<LuaSkinAudioIdentity, SoundRecord> sounds_;
  std::size_t encodedBytesTotal_ = 0;
  std::size_t decodedBytesTotal_ = 0;
  std::uint64_t nextIdentity_ = 0;
};

class NoOutputLuaSkinAudioBackend final : public LuaSkinAudioBackend {
public:
  float systemVolume() const noexcept override { return 0.0F; }

  std::optional<LuaSkinAudioIdentity>
  load(const std::filesystem::path &, std::stop_token stop) noexcept override {
    if (stop.stop_requested() || identities_.size() >= 256) {
      return std::nullopt;
    }
    LuaSkinAudioIdentity identity{.value = ++nextIdentity_};
    if (!identity) {
      identity.value = ++nextIdentity_;
    }
    identities_.emplace(identity, true);
    return identity;
  }

  void play(LuaSkinAudioIdentity, float, bool) noexcept override {}
  void stop(LuaSkinAudioIdentity) noexcept override {}
  void dispose(LuaSkinAudioIdentity identity) noexcept override {
    identities_.erase(identity);
  }

private:
  std::map<LuaSkinAudioIdentity, bool> identities_;
  std::uint64_t nextIdentity_ = 0;
};

} // namespace

std::shared_ptr<LuaSkinAudioBackend>
createLuaSkinApplicationAudioBackend(
    AudioWrapper &audio, std::function<float()> systemVolume,
    LuaSkinApplicationAudioLimits limits) {
  return std::make_shared<ApplicationLuaSkinAudioBackend>(
      audio, std::move(systemVolume), limits);
}

std::shared_ptr<LuaSkinAudioBackend> createLuaSkinNoOutputAudioBackend() {
  return std::make_shared<NoOutputLuaSkinAudioBackend>();
}

} // namespace skin
