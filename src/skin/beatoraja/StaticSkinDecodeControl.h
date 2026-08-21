#pragma once

#include <cstddef>
#include <cstdint>

namespace skin {

enum class StaticSkinDecodePhase : std::uint8_t {
  JsonStructure,
  JsonModel,
  Lr2Model,
};

struct StaticSkinDecodeCheckpoint {
  using Notify = void (*)(StaticSkinDecodePhase, std::size_t, void *) noexcept;

  Notify notify = nullptr;
  void *context = nullptr;
};

} // namespace skin
