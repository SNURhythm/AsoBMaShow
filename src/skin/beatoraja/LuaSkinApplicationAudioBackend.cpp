#include "LuaSkinApplicationAudioBackend.h"

#include "../../audio/AudioWrapper.h"
#include "../../path.h"

#include <atomic>
#include <map>
#include <utility>

namespace skin {
namespace {

class ApplicationLuaSkinAudioBackend final : public LuaSkinAudioBackend {
public:
  ApplicationLuaSkinAudioBackend(AudioWrapper &audio,
                                 std::function<float()> systemVolume)
      : audio_(&audio), systemVolume_(std::move(systemVolume)) {}

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
  load(const std::filesystem::path &path) noexcept override {
    try {
      const path_t backendPath = fspath_to_path_t(path);
      LuaSkinAudioIdentity identity{.value = ++nextIdentity_};
      if (!identity) {
        identity.value = ++nextIdentity_;
      }
      auto [inserted, unique] = paths_.emplace(identity, backendPath);
      if (!unique) {
        return std::nullopt;
      }
      std::atomic_bool cancelled = false;
      if (!audio_->loadSound(backendPath, cancelled)) {
        paths_.erase(inserted);
        return std::nullopt;
      }
      return identity;
    } catch (...) {
      return std::nullopt;
    }
  }

  void play(LuaSkinAudioIdentity identity, float volume,
            bool loop) noexcept override {
    if (const auto found = paths_.find(identity); found != paths_.end()) {
      (void)audio_->playSkinSound(found->second, volume, loop);
    }
  }

  void stop(LuaSkinAudioIdentity identity) noexcept override {
    if (const auto found = paths_.find(identity); found != paths_.end()) {
      (void)audio_->stopSkinSound(found->second);
    }
  }

  void dispose(LuaSkinAudioIdentity identity) noexcept override {
    if (const auto found = paths_.find(identity); found != paths_.end()) {
      (void)audio_->disposeSkinSound(found->second);
      paths_.erase(found);
    }
  }

private:
  AudioWrapper *audio_ = nullptr;
  std::function<float()> systemVolume_;
  std::map<LuaSkinAudioIdentity, path_t> paths_;
  std::uint64_t nextIdentity_ = 0;
};

} // namespace

std::shared_ptr<LuaSkinAudioBackend>
createLuaSkinApplicationAudioBackend(
    AudioWrapper &audio, std::function<float()> systemVolume) {
  return std::make_shared<ApplicationLuaSkinAudioBackend>(
      audio, std::move(systemVolume));
}

} // namespace skin
