#include "GameplaySkinSourceFormat.h"

#include <cstddef>

namespace skin {
namespace {

[[nodiscard]] bool asciiCaseInsensitiveSuffix(std::string_view path,
                                               std::string_view suffix) noexcept {
  if (path.size() < suffix.size()) {
    return false;
  }

  const auto start = path.size() - suffix.size();
  for (std::size_t index = 0; index < suffix.size(); ++index) {
    char actual = path[start + index];
    const char expected = suffix[index];
    if (actual >= 'A' && actual <= 'Z') {
      actual = static_cast<char>(actual - 'A' + 'a');
    }
    if (actual != expected) {
      return false;
    }
  }
  return true;
}

} // namespace

std::optional<GameplaySkinSourceFormat>
gameplaySkinSourceFormatForPath(std::string_view packageRelativePath) noexcept {
  if (asciiCaseInsensitiveSuffix(packageRelativePath, ".luaskin")) {
    return GameplaySkinSourceFormat::Lua;
  }
  if (asciiCaseInsensitiveSuffix(packageRelativePath, ".json")) {
    return GameplaySkinSourceFormat::Json;
  }
  if (asciiCaseInsensitiveSuffix(packageRelativePath, ".lr2skin")) {
    return GameplaySkinSourceFormat::Lr2;
  }
  return std::nullopt;
}

} // namespace skin
