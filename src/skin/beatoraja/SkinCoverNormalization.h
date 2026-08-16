#pragma once

#include "BeatorajaSkinModel.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace skin {

// These IDs are pinned Beatoraja c2ed5db1 SkinProperty constants:
// OFFSET_LIFT=3 and OFFSET_HIDDEN_COVER=5. They are not header-configured
// offsets in the source-neutral AsoBMaShow model, so this helper owns their
// explicit compatibility names.
inline constexpr int kSkinCoverLiftOffsetId = 3;
inline constexpr int kSkinCoverHiddenOffsetId = 5;

// Source-neutral input after a loader has resolved source images into frames.
// The line is optional so an omitted source field can preserve the pinned
// cover default (-1) without relying on a format-specific default value.
struct SkinCoverNormalizationInput {
  SkinCoverKind kind = SkinCoverKind::Hidden;
  SkinSpriteFrames sprite;
  std::optional<double> authoredDisappearLine;
  std::optional<bool> authoredDisappearLineLinksLift;
  double lineScale = 1.0;
  std::vector<int> authoredDestinationOffsetIds;
};

struct SkinCoverNormalizationPolicy {
  static constexpr std::size_t maxAuthoredDestinationOffsetIds = 256;
  static constexpr double maxScaledDisappearLine = 8'192.0;
};

enum class SkinCoverNormalizationError : std::uint8_t {
  None,
  InvalidSprite,
  InvalidLineScale,
  InvalidDisappearLine,
  DestinationOffsetLimitExceeded,
};

struct SkinCoverNormalizationResult {
  std::optional<SkinCoverObject> cover;
  std::vector<int> destinationOffsetIds;
  SkinCoverNormalizationError error = SkinCoverNormalizationError::None;
};

// Mirrors JsonPlaySkinObjectLoader at Beatoraja c2ed5db1: authored destination
// offsets remain in order (including duplicates), then Hidden adds Lift and
// Hidden Cover offsets while Lift adds only Lift. The resulting link flag is
// retained for the renderer's later clipping/lift calculation.
[[nodiscard]] SkinCoverNormalizationResult
normalizeSkinCover(const SkinCoverNormalizationInput &input);

} // namespace skin
