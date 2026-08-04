#pragma once

#include "BeatorajaSkinModel.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace skin {

inline constexpr int kSkinNoteLaneGeometryDst2Sentinel =
    std::numeric_limits<int>::min();

// Source-neutral lane metadata after the normal note visual has been resolved
// enough to expose its first frame's height. A missing value deliberately
// preserves the model's resource-preparation deferral for a missing `size`.
struct SkinNoteLaneGeometryNormalizationInput {
  std::vector<std::optional<double>> normalFirstFrameHeights;
  std::vector<SkinAuthoredRect> laneDestinations;
  std::vector<double> authoredNoteHeights;
  std::optional<int> secondaryDestinationY;
  // The Lua decoder must reject fractional expansionrate fields before this
  // helper is called; this boundary accepts only exact integer percentages.
  std::array<int, 2> expansionRatePercent{100, 100};
};

// This helper owns its pending presentation until decoder/model integration.
// Its fields intentionally correspond to SkinLaneNotePresentation without
// requiring any model-header change in this incremental task.
struct SkinNormalizedNoteLaneGeometry {
  std::size_t authoredLane = 0;
  SkinAuthoredRect laneDestination;
  std::optional<double> authoredNoteHeight;
  std::optional<int> secondaryDestinationY;
};

struct SkinNormalizedNoteLaneGeometrySet {
  std::vector<SkinNormalizedNoteLaneGeometry> lanes;
  std::array<int, 2> expansionRatePercent{100, 100};
};

struct SkinNoteLaneGeometryNormalizationPolicy {
  static constexpr std::size_t maxLanes = 256;
  static constexpr std::size_t maxLaneDestinations = 256;
  static constexpr std::size_t maxAuthoredNoteHeights = 256;
  // Matches LuaSkinTableDecoderPolicy::maxAuthoredDimension without coupling
  // this source-neutral helper to the Lua decoder implementation.
  static constexpr double maxGeometryMagnitude = 8'192.0;
};

enum class SkinNoteLaneGeometryNormalizationError : std::uint8_t {
  None,
  LaneLimitExceeded,
  UnsafeCardinality,
  MissingLaneDestination,
  // Includes finite geometry that exceeds maxGeometryMagnitude.
  NonFiniteGeometry,
};

struct SkinNoteLaneGeometryNormalizationResult {
  std::optional<SkinNormalizedNoteLaneGeometrySet> geometry;
  SkinNoteLaneGeometryNormalizationError error =
      SkinNoteLaneGeometryNormalizationError::None;
};

// Mirrors JsonPlaySkinObjectLoader's Note dst/size/dst2/expansionrate setup
// at Beatoraja c2ed5db1. Normal visual lanes establish the indexed lane
// space. Every such lane needs dst[i], while a short size prefix falls back to
// the normal first-frame height when available; otherwise the height remains
// explicitly deferred for resource preparation.
[[nodiscard]] SkinNoteLaneGeometryNormalizationResult
normalizeSkinNoteLaneGeometry(
    const SkinNoteLaneGeometryNormalizationInput &input);

} // namespace skin
