#pragma once

#include <array>
#include <string_view>

namespace asobmshow::chart_assets {

inline constexpr std::array<std::string_view, 4> kAudioExtensions = {
    "flac", "wav", "ogg", "mp3"};

} // namespace asobmshow::chart_assets
