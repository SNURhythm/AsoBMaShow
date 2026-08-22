#pragma once

#include "LuaSkinAudioHost.h"
#include "SkinLiveResourceCounters.h"

#include <cstddef>
#include <functional>
#include <memory>

class AudioWrapper;

namespace skin {

struct LuaSkinApplicationAudioLimits {
  std::size_t maximumIdentities = 256;
  std::size_t maximumEncodedBytes = 16U * 1024U * 1024U;
  std::size_t maximumDecodedBytes = 64U * 1024U * 1024U;
};

[[nodiscard]] std::shared_ptr<LuaSkinAudioBackend>
createLuaSkinApplicationAudioBackend(AudioWrapper &,
                                     std::function<float()> systemVolume,
                                     LuaSkinApplicationAudioLimits = {},
                                     std::shared_ptr<SkinLiveResourceCounters> = {});

// Replay export mixes chart audio through its export-owned renderer. Skin
// audio is therefore source-faithfully silent and must not touch the live
// application mixer.
[[nodiscard]] std::shared_ptr<LuaSkinAudioBackend>
createLuaSkinNoOutputAudioBackend(
    std::shared_ptr<SkinLiveResourceCounters> = {});

} // namespace skin
