#pragma once

#include "BeatorajaSkinModel.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <vector>

namespace skin {

enum class SkinGeneratedTextureLayer : std::uint8_t {
  Primary,
  Background,
  Shape,
  Cursor,
  Line,
};

struct SkinGeneratedTextureKey {
  SkinObjectId sourceObject = 0;
  std::uint32_t authoredOrdinal = 0;
  SkinGeneratedTextureLayer layer = SkinGeneratedTextureLayer::Primary;

  auto operator<=>(const SkinGeneratedTextureKey &) const = default;
};

struct SkinGeneratedTextureData {
  int width = 0;
  int height = 0;
  std::shared_ptr<const std::vector<std::uint8_t>> rgba;
};

} // namespace skin
