#pragma once

#include "../package/SkinPathPolicy.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace skin {

// LR2 paths are game-root relative, even when a download retains its
// LR2files/Theme tree inside a differently named package. Resolve against
// the selected entry's layout, without searching unrelated packages.
inline std::string lr2GenericPath(const std::filesystem::path &path) {
  const auto utf8 = path.generic_u8string();
  return {utf8.begin(), utf8.end()};
}

inline std::optional<std::string> lr2PackagePath(
    const SkinEntryId &entry, std::string_view authored) {
  std::string path(authored);
  std::ranges::replace(path, '\\', '/');
  while (path.starts_with("./")) path.erase(0, 2);
  constexpr std::string_view prefix = "LR2files/Theme/";
  if (!path.starts_with(prefix)) return std::nullopt;
  const auto suffix = pathFromUtf8(path.substr(prefix.size()));
  if (suffix.empty() || suffix.is_absolute()) return std::nullopt;
  for (const auto &component : suffix) {
    if (component == "..") return std::nullopt;
  }

  const std::filesystem::path parent =
      pathFromUtf8(entry.packageRelativePath).parent_path();
  std::filesystem::path ancestor;
  for (const auto &component : parent) {
    ancestor /= component;
    if (component == "Theme" && ancestor.parent_path().filename() == "LR2files") {
      return lr2GenericPath(ancestor / suffix);
    }
  }

  // Also accept a theme extracted on its own, including under a wrapper.
  const auto theme = *suffix.begin();
  if (entry.package.directoryName == lr2GenericPath(theme)) {
    return lr2GenericPath(suffix.lexically_relative(theme));
  }
  ancestor.clear();
  for (const auto &component : parent) {
    ancestor /= component;
    if (component == theme) {
      return lr2GenericPath(ancestor.parent_path() / suffix);
    }
  }
  return std::nullopt;
}

inline std::string lr2EntryRelativePath(const SkinEntryId &entry,
                                       std::string_view authored) {
  const auto path = lr2PackagePath(entry, authored);
  if (!path) return std::string(authored);
  return lr2GenericPath(pathFromUtf8(*path).lexically_relative(
      pathFromUtf8(entry.packageRelativePath).parent_path()));
}

} // namespace skin
