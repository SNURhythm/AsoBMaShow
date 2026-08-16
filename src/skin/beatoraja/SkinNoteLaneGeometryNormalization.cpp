#include "SkinNoteLaneGeometryNormalization.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <cmath>
#include <utility>

namespace skin {
namespace {

SkinNoteLaneGeometryNormalizationResult
failure(SkinNoteLaneGeometryNormalizationError error) {
  return {.geometry = std::nullopt, .error = error};
}

bool finiteRect(const SkinAuthoredRect &rect) noexcept {
  const auto bounded = [](double value) {
    return std::isfinite(value) &&
           std::abs(value) <=
               SkinNoteLaneGeometryNormalizationPolicy::maxGeometryMagnitude;
  };
  return bounded(rect.x) && bounded(rect.y) && bounded(rect.width) &&
         bounded(rect.height);
}

bool finiteAndBounded(double value) noexcept {
  return std::isfinite(value) &&
         std::abs(value) <=
             SkinNoteLaneGeometryNormalizationPolicy::maxGeometryMagnitude;
}

bool boundedInteger(int value) noexcept {
  return std::abs(static_cast<std::int64_t>(value)) <=
         static_cast<std::int64_t>(
             SkinNoteLaneGeometryNormalizationPolicy::maxGeometryMagnitude);
}

} // namespace

SkinNoteLaneGeometryNormalizationResult normalizeSkinNoteLaneGeometry(
    const SkinNoteLaneGeometryNormalizationInput &input) {
  const std::size_t laneCount = input.normalFirstFrameHeights.size();
  if (laneCount > SkinNoteLaneGeometryNormalizationPolicy::maxLanes) {
    return failure(SkinNoteLaneGeometryNormalizationError::LaneLimitExceeded);
  }
  if (input.laneDestinations.size() >
          SkinNoteLaneGeometryNormalizationPolicy::maxLaneDestinations ||
      input.authoredNoteHeights.size() >
          SkinNoteLaneGeometryNormalizationPolicy::maxAuthoredNoteHeights) {
    return failure(SkinNoteLaneGeometryNormalizationError::UnsafeCardinality);
  }
  if (input.laneDestinations.size() < laneCount) {
    return failure(
        SkinNoteLaneGeometryNormalizationError::MissingLaneDestination);
  }
  for (const auto &destination : input.laneDestinations) {
    if (!finiteRect(destination)) {
      return failure(SkinNoteLaneGeometryNormalizationError::NonFiniteGeometry);
    }
  }
  for (const auto &height : input.normalFirstFrameHeights) {
    if (height && !finiteAndBounded(*height)) {
      return failure(SkinNoteLaneGeometryNormalizationError::NonFiniteGeometry);
    }
  }
  for (const double height : input.authoredNoteHeights) {
    if (!finiteAndBounded(height)) {
      return failure(SkinNoteLaneGeometryNormalizationError::NonFiniteGeometry);
    }
  }
  if ((input.secondaryDestinationY &&
       *input.secondaryDestinationY != kSkinNoteLaneGeometryDst2Sentinel &&
       !boundedInteger(*input.secondaryDestinationY)) ||
      !boundedInteger(input.expansionRatePercent[0]) ||
      !boundedInteger(input.expansionRatePercent[1])) {
    return failure(SkinNoteLaneGeometryNormalizationError::NonFiniteGeometry);
  }

  const std::optional<int> secondaryDestinationY =
      input.secondaryDestinationY &&
              *input.secondaryDestinationY != kSkinNoteLaneGeometryDst2Sentinel
          ? input.secondaryDestinationY
          : std::nullopt;

  SkinNormalizedNoteLaneGeometrySet normalized;
  normalized.expansionRatePercent = input.expansionRatePercent;
  normalized.lanes.reserve(laneCount);
  for (std::size_t lane = 0; lane < laneCount; ++lane) {
    const std::optional<double> noteHeight =
        lane < input.authoredNoteHeights.size()
            ? std::optional<double>{input.authoredNoteHeights[lane]}
            : input.normalFirstFrameHeights[lane];
    normalized.lanes.push_back(
        {.authoredLane = lane,
         .laneDestination = input.laneDestinations[lane],
         .authoredNoteHeight = noteHeight,
         .secondaryDestinationY = secondaryDestinationY});
  }

  return {.geometry = std::move(normalized),
          .error = SkinNoteLaneGeometryNormalizationError::None};
}

} // namespace skin

#endif
