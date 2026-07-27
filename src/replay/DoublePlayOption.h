#pragma once

#include <cstdint>

namespace replay {

enum class DoublePlayOption : std::uint8_t {
  Normal = 0,
  Flip = 1,
};

} // namespace replay
