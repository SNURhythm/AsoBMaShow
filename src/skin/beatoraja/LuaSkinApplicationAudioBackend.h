#pragma once

#include "LuaSkinAudioHost.h"

#include <functional>
#include <memory>

class AudioWrapper;

namespace skin {

[[nodiscard]] std::shared_ptr<LuaSkinAudioBackend>
createLuaSkinApplicationAudioBackend(AudioWrapper &,
                                     std::function<float()> systemVolume);

} // namespace skin
