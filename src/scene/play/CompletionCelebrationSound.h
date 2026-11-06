#pragma once

#include "CompletionCelebration.h"

#include <array>
#include <optional>
#include <string_view>

namespace completion_celebration {
inline constexpr std::array<std::string_view, 3> kSoundPaths{
    "assets/audio/completion/full-combo.wav",
    "assets/audio/completion/perfect.wav",
    "assets/audio/completion/max.wav"};

class SoundTrigger {
public:
  void reset() noexcept { consumed_ = false; }

  std::optional<std::size_t> poll(State state, long long visualMicros,
                                 bool builtInPresented) noexcept {
    if (consumed_ || state.achievement == Achievement::None ||
        visualMicros < state.startMicros) return std::nullopt;
    consumed_ = true;
    // Do not start an old cue after returning from the background or seeking.
    if (!builtInPresented ||
        static_cast<double>(visualMicros) - state.startMicros > 250'000)
      return std::nullopt;
    switch (state.achievement) {
    case Achievement::FullCombo: return 0;
    case Achievement::Perfect: return 1;
    case Achievement::Max: return 2;
    default: return std::nullopt;
    }
  }

private:
  bool consumed_ = false;
};
} // namespace completion_celebration
