#pragma once

#include <cstdint>

namespace rendering {

inline constexpr uint32_t kBgfxTransientVertexBufferBytes =
    16U * 1024U * 1024U;
inline constexpr uint32_t kBgfxTransientIndexBufferBytes =
    4U * 1024U * 1024U;

template <typename Limits>
inline void applyBgfxTransientBufferLimits(Limits &limits) {
  limits.maxTransientVbSize = kBgfxTransientVertexBufferBytes;
  limits.maxTransientIbSize = kBgfxTransientIndexBufferBytes;
}

} // namespace rendering
