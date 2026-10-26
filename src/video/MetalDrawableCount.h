#pragma once

#include <cstdint>

namespace video {
// App-owned extension to our pinned bgfx Metal renderer. Carry the preference
// with each reset so resize, export restoration and display rollback agree.
// The generated-source patch rejects collisions with upstream reset flags.
inline constexpr std::uint32_t kMetalTripleBufferingResetFlag = 0x40000000;
constexpr std::uint32_t metalDrawableCount(std::uint32_t resetFlags) {
  return (resetFlags & kMetalTripleBufferingResetFlag) != 0 ? 3 : 2;
}
} // namespace video
