#pragma once

#include <array>
#include <string_view>

namespace asobmshow::chart_assets {

inline constexpr std::array<std::string_view, 4> kAudioExtensions = {
    "flac", "wav", "ogg", "mp3"};

// Visual playback tries these extensions in priority order, even when the
// authored extension names an existing file. Keep audio's separate semantics.
inline constexpr std::array<std::string_view, 9> kVideoExtensions = {
    "mp4", "wmv", "m4v", "webm", "mpg", "mpeg", "m1v", "m2v", "avi"};
inline constexpr std::array<std::string_view, 6> kImageExtensions = {
    "jpg", "jpeg", "gif", "bmp", "png", "tga"};

} // namespace asobmshow::chart_assets
