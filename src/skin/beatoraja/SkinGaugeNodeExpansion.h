#pragma once

#include "BeatorajaSkinModel.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skin {

// Source-neutral representation of an authored Gauge node reference and the
// Images available to resolve it. The sprite is deliberately retained here so
// this expansion neither reads files nor depends on a Lua runtime.
struct SkinGaugeNodeImage {
  std::string id;
  SkinSpriteFrames sprite;
};

struct SkinGaugeNodeExpansionInput {
  std::vector<std::string> nodes;
  std::vector<SkinGaugeNodeImage> images;
  int parts = 50;
  int animationType = 0;
  int animationRange = 3;
  int animationCycleMillis = 33;
  int resultStartMillis = 0;
  int resultEndMillis = 500;
};

struct SkinGaugeNodeExpansionPolicy {
  static constexpr std::size_t roleCount = 36;
  static constexpr std::size_t maxSourceImages = 2'048;
  static constexpr std::size_t maxParts = 512;
  static constexpr std::size_t maxAnimationRange = 1'024;
  static constexpr int maxAnimationCycleMillis = 60'000;
  static constexpr int minFlickerCycleMillis = 4;
  static constexpr int minResultTimeMillis = -600'000;
  static constexpr int maxResultTimeMillis = 600'000;
  // The limit counts every copied frame after the fixed 36-role expansion.
  static constexpr std::size_t maxExpandedSpriteFrames = 200'000;
};

enum class SkinGaugeNodeExpansionError : std::uint8_t {
  None,
  UnsupportedNodeCount,
  SourceImageLimitExceeded,
  AmbiguousNodeImage,
  MissingNodeImage,
  EmptyAnimationFrames,
  UnequalAnimationFrameCount,
  FrameLimitExceeded,
  InvalidAnimationType,
  InvalidAnimationParameters,
};

struct SkinGaugeNodeExpansionResult {
  std::optional<SkinGaugeObject> gauge;
  SkinGaugeNodeExpansionError error = SkinGaugeNodeExpansionError::None;
};

// Mirrors JsonSkinObjectLoader's 4/8/12/36 node layouts at Beatoraja
// c2ed5db1. The copied role sprites intentionally have no timer/cycle: the
// pinned constructor passes 0, 0 to SkinGauge's SkinSourceImageSet.
[[nodiscard]] SkinGaugeNodeExpansionResult
expandSkinGaugeNodes(const SkinGaugeNodeExpansionInput &input);

} // namespace skin
