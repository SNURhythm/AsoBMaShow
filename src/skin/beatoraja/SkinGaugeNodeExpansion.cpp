#include "SkinGaugeNodeExpansion.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <array>
#include <utility>

namespace skin {
namespace {

using RoleMap = std::array<std::uint8_t, SkinGaugeNodeExpansionPolicy::roleCount>;

constexpr RoleMap kFourNodeRoles{
    0, 1, 2, 3, 0, 1, 0, 1, 2, 3, 0, 1,
    0, 1, 2, 3, 0, 1, 0, 1, 2, 3, 0, 1,
    0, 1, 2, 3, 0, 1, 0, 1, 2, 3, 0, 1,
};

constexpr RoleMap kEightNodeRoles{
    4, 5, 6, 7, 4, 5, 4, 5, 6, 7, 4, 5,
    0, 1, 2, 3, 0, 1, 0, 1, 2, 3, 0, 1,
    4, 5, 6, 7, 4, 5, 4, 5, 6, 7, 4, 5,
};

constexpr RoleMap kTwelveNodeRoles{
    4,  5,  6,  7,  10, 11, 4,  5,  6,  7,  10, 11,
    0,  1,  2,  3,  8,  9,  0,  1,  2,  3,  8,  9,
    4,  5,  6,  7,  10, 11, 4,  5,  6,  7,  10, 11,
};

constexpr RoleMap kThirtySixNodeRoles{
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
    12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,
};

const RoleMap *roleMapFor(std::size_t nodeCount) noexcept {
  switch (nodeCount) {
  case 4:
    return &kFourNodeRoles;
  case 8:
    return &kEightNodeRoles;
  case 12:
    return &kTwelveNodeRoles;
  case 36:
    return &kThirtySixNodeRoles;
  default:
    return nullptr;
  }
}

SkinGaugeNodeExpansionResult failure(SkinGaugeNodeExpansionError error) {
  return {.gauge = std::nullopt, .error = error};
}

} // namespace

SkinGaugeNodeExpansionResult
expandSkinGaugeNodes(const SkinGaugeNodeExpansionInput &input) {
  const RoleMap *const roleMap = roleMapFor(input.nodes.size());
  if (roleMap == nullptr) {
    return failure(SkinGaugeNodeExpansionError::UnsupportedNodeCount);
  }
  if (input.images.size() > SkinGaugeNodeExpansionPolicy::maxSourceImages) {
    return failure(SkinGaugeNodeExpansionError::SourceImageLimitExceeded);
  }
  if (input.animationType < static_cast<int>(SkinGaugeAnimationType::Random) ||
      input.animationType > static_cast<int>(SkinGaugeAnimationType::Flicker)) {
    return failure(SkinGaugeNodeExpansionError::InvalidAnimationType);
  }
  if (input.parts < 1 ||
      input.parts > static_cast<int>(SkinGaugeNodeExpansionPolicy::maxParts) ||
      input.animationRange < 0 ||
      input.animationRange >
          static_cast<int>(SkinGaugeNodeExpansionPolicy::maxAnimationRange) ||
      input.animationCycleMillis < 0 ||
      input.animationCycleMillis >
          SkinGaugeNodeExpansionPolicy::maxAnimationCycleMillis ||
      (input.animationType == static_cast<int>(SkinGaugeAnimationType::Flicker) &&
       input.animationCycleMillis <
           SkinGaugeNodeExpansionPolicy::minFlickerCycleMillis) ||
      input.resultStartMillis <
          SkinGaugeNodeExpansionPolicy::minResultTimeMillis ||
      input.resultStartMillis >
          SkinGaugeNodeExpansionPolicy::maxResultTimeMillis ||
      input.resultEndMillis <
          SkinGaugeNodeExpansionPolicy::minResultTimeMillis ||
      input.resultEndMillis >
          SkinGaugeNodeExpansionPolicy::maxResultTimeMillis ||
      input.resultStartMillis >= input.resultEndMillis) {
    return failure(SkinGaugeNodeExpansionError::InvalidAnimationParameters);
  }

  // JsonSkinObjectLoader walks the image list and would use its first match.
  // A source-neutral model must instead fail closed rather than preserve an
  // order-dependent ambiguity.
  for (std::size_t image = 0; image < input.images.size(); ++image) {
    for (std::size_t prior = 0; prior < image; ++prior) {
      if (input.images[prior].id == input.images[image].id) {
        return failure(SkinGaugeNodeExpansionError::AmbiguousNodeImage);
      }
    }
  }

  std::array<const SkinSpriteFrames *, SkinGaugeNodeExpansionPolicy::roleCount>
      resolved{};
  std::size_t animationFrameCount = 0;
  for (std::size_t node = 0; node < input.nodes.size(); ++node) {
    const SkinSpriteFrames *sprite = nullptr;
    for (const auto &image : input.images) {
      if (image.id == input.nodes[node]) {
        sprite = &image.sprite;
        break;
      }
    }
    if (sprite == nullptr) {
      return failure(SkinGaugeNodeExpansionError::MissingNodeImage);
    }
    if (sprite->frames.empty()) {
      return failure(SkinGaugeNodeExpansionError::EmptyAnimationFrames);
    }
    if (node == 0) {
      animationFrameCount = sprite->frames.size();
      if (animationFrameCount >
          SkinGaugeNodeExpansionPolicy::maxExpandedSpriteFrames /
              SkinGaugeNodeExpansionPolicy::roleCount) {
        return failure(SkinGaugeNodeExpansionError::FrameLimitExceeded);
      }
    } else if (sprite->frames.size() != animationFrameCount) {
      return failure(SkinGaugeNodeExpansionError::UnequalAnimationFrameCount);
    }
    resolved[node] = sprite;
  }

  SkinGaugeObject gauge;
  gauge.orderedNodes.reserve(SkinGaugeNodeExpansionPolicy::roleCount);
  for (const std::uint8_t node : *roleMap) {
    const SkinSpriteFrames &source = *resolved[node];
    gauge.orderedNodes.push_back(
        {.resource = source.resource,
         .frames = source.frames,
         // Pinned JsonSkinObjectLoader creates SkinGauge(..., 0, 0, ...).
         .cycleMillis = 0,
         .timer = std::nullopt});
  }
  gauge.parts = input.parts;
  gauge.animation = static_cast<SkinGaugeAnimationType>(input.animationType);
  gauge.animationRange = input.animationRange;
  gauge.animationCycleMillis = input.animationCycleMillis;
  gauge.resultStartMillis = input.resultStartMillis;
  gauge.resultEndMillis = input.resultEndMillis;
  return {.gauge = std::move(gauge),
          .error = SkinGaugeNodeExpansionError::None};
}

} // namespace skin

#endif
