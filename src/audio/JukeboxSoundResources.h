#pragma once

#include "AudioMix.h"
#include "../path.h"

#include <unordered_map>
#include <utility>
#include <vector>

namespace jukebox_sound_resources {

using SoundMap = std::unordered_map<int, path_t>;

template <typename SoundStore>
[[nodiscard]] audio::playback::BackendOperationResult
PruneAndCommitSoundMap(SoundStore &soundStore, SoundMap &currentMap,
                       SoundMap nextMap,
                       const std::vector<path_t> &obsoletePaths) {
  auto result = soundStore.pruneSounds(obsoletePaths);
  if (result.success) {
    currentMap = std::move(nextMap);
  }
  return result;
}

} // namespace jukebox_sound_resources
