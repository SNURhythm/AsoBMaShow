#pragma once
#include "../audio/AudioWrapper.h"
#include <array>
#include <limits>

namespace kombo {
enum class Sound { Progress, Complete, Freeze, RoundAdvance, GoalAchieved };

// Short bundled cues use the existing system bus (and therefore master volume).
// Scene ownership releases handles safely; rendering never triggers playback.
class Sounds {
public:
  Sounds() = default;
  Sounds(const Sounds &) = delete;
  Sounds &operator=(const Sounds &) = delete;
  ~Sounds() {
    if (audio_) for (auto handle : handles_) if (handle) {
      audio_->stopSkinSound(*handle);
      audio_->disposeSkinSound(*handle);
    }
  }
  void play(AudioWrapper &audio, Sound cue) {
    audio_ = &audio;
    const auto index = static_cast<std::size_t>(cue);
    if (!handles_[index]) {
      constexpr std::array names{"progress.wav", "complete.wav", "freeze.wav", "round-advance.wav", "goal-achieved.wav"};
      std::atomic_bool cancelled{false};
      const auto loaded = audio.loadSkinSound(
          fspath_to_path_t(std::filesystem::path("assets/audio/kombo") / names[index]),
          // This limit covers the whole shared skin-sound cache. Our five
          // trusted bundled files have a separate small encoded-file bound.
          cancelled, 256 * 1024, std::numeric_limits<std::size_t>::max());
      if (!loaded.handle) return;
      handles_[index] = loaded.handle;
    }
    audio.playSkinSound(*handles_[index], 1.0F, false);
  }
private:
  AudioWrapper *audio_ = nullptr;
  std::array<std::optional<audio::SkinSoundHandle>, 5> handles_{};
};
} // namespace kombo
