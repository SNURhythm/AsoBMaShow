#pragma once

#include "../FileChecksum.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace replay {

[[nodiscard]] inline bool isCanonicalLegacyDigest(
    std::string_view value, std::size_t size) noexcept {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

// Schema v11 replay filenames require SHA-256. Schema v10 accepted MD5-only
// chart identities, so preserve those rows under a stable, namespaced fallback
// rather than discarding otherwise valid replay input.
[[nodiscard]] inline std::optional<std::string>
legacyReplaySha256ForMd5(std::string_view md5) {
  if (!isCanonicalLegacyDigest(md5, 32)) {
    return std::nullopt;
  }
  return file_checksum::sha256("asobmashow:legacy-md5:v1:" +
                               std::string(md5));
}

[[nodiscard]] inline bool storedChartIdentityMatches(
    std::string_view storedSha256, std::string_view storedMd5,
    std::string_view actualSha256, std::string_view actualMd5) {
  if (!isCanonicalLegacyDigest(storedSha256, 64) ||
      (!storedMd5.empty() && !isCanonicalLegacyDigest(storedMd5, 32))) {
    return false;
  }
  if (!storedMd5.empty()) {
    const auto fallback = legacyReplaySha256ForMd5(storedMd5);
    if (fallback.has_value() && storedSha256 == *fallback) {
      return actualMd5 == storedMd5;
    }
  }
  return actualSha256 == storedSha256 &&
         (storedMd5.empty() || actualMd5 == storedMd5);
}

} // namespace replay
