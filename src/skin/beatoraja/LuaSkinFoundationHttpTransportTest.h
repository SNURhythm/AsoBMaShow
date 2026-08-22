#pragma once

#include "LuaSkinHttpClient.h"

#include <cstddef>
#include <span>

namespace skin {

struct LuaSkinFoundationAppendProbe {
  std::size_t storedBytes = 0;
  bool continued = false;
  bool tooLarge = false;
};

[[nodiscard]] LuaSkinFoundationAppendProbe
probeLuaSkinFoundationAppend(LuaSkinHttpLimits limits,
                             std::span<const std::byte> chunk);

} // namespace skin
