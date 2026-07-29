#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace canonical_digest {

[[nodiscard]] inline bool
isCanonicalLowerHex(std::string_view value, std::size_t size) noexcept {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

} // namespace canonical_digest
