#include "ChartVisualAssetResolver.h"

#include <algorithm>

namespace asobmshow::chart_assets {

std::string LowerMediaExtension(const std::filesystem::path &path) {
  const auto encoded = path.extension().u8string();
  std::string result;
  for (const auto ch : encoded) {
    if (ch == u8'.' && result.empty()) continue;
    if (ch > 127) return {};
    result.push_back(ch >= u8'A' && ch <= u8'Z' ? static_cast<char>(ch + 32) : static_cast<char>(ch));
  }
  return result;
}

MediaKind ClassifyMediaPath(const std::filesystem::path &path) {
  const auto extension = LowerMediaExtension(path);
  if (std::ranges::find(kAudioExtensions, extension) != kAudioExtensions.end()) return MediaKind::Audio;
  if (std::ranges::find(kVideoExtensions, extension) != kVideoExtensions.end()) return MediaKind::Video;
  if (std::ranges::find(kImageExtensions, extension) != kImageExtensions.end()) return MediaKind::Image;
  return MediaKind::Other;
}

} // namespace asobmshow::chart_assets
