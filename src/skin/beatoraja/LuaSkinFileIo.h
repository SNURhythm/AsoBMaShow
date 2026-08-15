#pragma once

#include <cstdint>
#include <filesystem>
#include <ios>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace skin::lua_file_io {

inline std::filesystem::path physicalPathFromUtf8(std::string_view path) {
  std::u8string utf8;
  utf8.reserve(path.size());
  for (const unsigned char byte : path) {
    utf8.push_back(static_cast<char8_t>(byte));
  }
  return std::filesystem::path(utf8);
}

inline std::optional<std::streamoff>
checkedSeekPosition(std::streamoff base, std::int64_t offset) noexcept {
  static_assert(std::numeric_limits<std::streamoff>::is_signed);
  if (base < 0) {
    return std::nullopt;
  }

  const std::uintmax_t unsignedBase = static_cast<std::uintmax_t>(base);
  if (offset >= 0) {
    const std::uintmax_t unsignedOffset = static_cast<std::uintmax_t>(offset);
    const std::uintmax_t maximum =
        static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max());
    if (unsignedOffset > maximum - unsignedBase) {
      return std::nullopt;
    }
    return base + static_cast<std::streamoff>(unsignedOffset);
  }

  const std::uintmax_t magnitude =
      static_cast<std::uintmax_t>(-(offset + 1)) + 1U;
  if (magnitude >= unsignedBase) {
    return std::streamoff{0};
  }
  return base - static_cast<std::streamoff>(magnitude);
}

} // namespace skin::lua_file_io
