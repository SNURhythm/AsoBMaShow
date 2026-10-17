#pragma once

#include "ChartAssetExtensions.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace asobmshow::chart_assets {

enum class MediaKind { Other, Audio, Video, Image };
MediaKind ClassifyMediaPath(const std::filesystem::path &path);
std::string LowerMediaExtension(const std::filesystem::path &path);

template <typename Lookup>
std::optional<std::filesystem::path> FindWithReplacedExtensions(
    const std::filesystem::path &basePath,
    std::span<const std::string_view> extensions, const Lookup &lookup) {
  for (const auto extension : extensions) {
    auto candidate = basePath;
    candidate.replace_extension(std::string(extension));
    if (auto found = lookup(candidate)) return found;
  }
  return std::nullopt;
}

struct ResolvedVisualAsset {
  std::filesystem::path path;
  bool video = false;
};
template <typename Lookup>
std::optional<ResolvedVisualAsset> ResolveVisualAsset(
    const std::filesystem::path &path, const Lookup &lookup) {
  if (auto found = FindWithReplacedExtensions(path, kVideoExtensions, lookup))
    return ResolvedVisualAsset{*found, true};
  if (auto found = FindWithReplacedExtensions(path, kImageExtensions, lookup))
    return ResolvedVisualAsset{*found, false};
  return std::nullopt;
}

} // namespace asobmshow::chart_assets
