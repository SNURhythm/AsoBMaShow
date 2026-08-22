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
  // Monotonic per-key CPU Pixmap generation. The upload cache must not infer
  // dirtiness from shared pointer identity because session Pixmaps are
  // retained and redrawn in-place on their source-defined cadence.
  std::uint64_t contentRevision = 0;
};

} // namespace skin
