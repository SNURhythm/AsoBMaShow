#include "skin/beatoraja/SkinNoteLaneGeometryNormalization.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace {

using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

SkinAuthoredRect rect(double marker) {
  return {.x = marker,
          .y = marker + 10.0,
          .width = marker + 20.0,
          .height = marker + 30.0};
}

SkinNoteLaneGeometryNormalizationInput completeInput() {
  return {.normalFirstFrameHeights = {8.0, 16.0, 24.0},
          .laneDestinations = {rect(1.0), rect(2.0), rect(3.0)}};
}

void testMixedSizePrefixUsesNormalFirstFrameFallbackInLaneOrder() {
  auto input = completeInput();
  input.authoredNoteHeights = {101.5, 202.5};

  const auto result = normalizeSkinNoteLaneGeometry(input);
  expect(result.geometry.has_value(), "mixed size prefix normalizes");
  if (!result.geometry) {
    return;
  }
  const auto &lanes = result.geometry->lanes;
  expect(lanes.size() == 3, "normal metadata establishes three output lanes");
  expect(lanes[0].authoredLane == 0 && lanes[1].authoredLane == 1 &&
             lanes[2].authoredLane == 2,
         "output preserves the authored normal-lane order");
  expect(lanes[0].laneDestination.x == 1.0 &&
             lanes[1].laneDestination.x == 2.0 &&
             lanes[2].laneDestination.x == 3.0,
         "each lane retains its indexed destination rectangle");
  expect(lanes[0].authoredNoteHeight == 101.5 &&
             lanes[1].authoredNoteHeight == 202.5 &&
             lanes[2].authoredNoteHeight == 24.0,
         "authored size prefix overrides only matching lanes and later lanes "
         "use normal first-frame height");
}

void testMissingNormalMetadataPreservesDeferredHeight() {
  auto input = completeInput();
  input.normalFirstFrameHeights[1].reset();
  input.authoredNoteHeights = {50.0};

  const auto result = normalizeSkinNoteLaneGeometry(input);
  expect(result.geometry.has_value(),
         "missing resource metadata does not erase geometry");
  if (!result.geometry) {
    return;
  }
  expect(result.geometry->lanes[0].authoredNoteHeight == 50.0 &&
             !result.geometry->lanes[1].authoredNoteHeight.has_value() &&
             result.geometry->lanes[2].authoredNoteHeight == 24.0,
         "missing normal first-frame metadata remains explicitly deferred");
}

void testDst2AbsentAndSentinelRemainUnsetWhilePresentValueAppliesToEveryLane() {
  auto input = completeInput();
  const auto absent = normalizeSkinNoteLaneGeometry(input);
  expect(absent.geometry.has_value() &&
             !absent.geometry->lanes[0].secondaryDestinationY.has_value(),
         "absent dst2 leaves the secondary destination unset");

  input.secondaryDestinationY = kSkinNoteLaneGeometryDst2Sentinel;
  const auto sentinel = normalizeSkinNoteLaneGeometry(input);
  expect(
      sentinel.geometry.has_value() &&
          !sentinel.geometry->lanes[2].secondaryDestinationY.has_value(),
      "Integer.MIN_VALUE dst2 sentinel leaves the secondary destination unset");

  input.secondaryDestinationY = 73;
  const auto present = normalizeSkinNoteLaneGeometry(input);
  expect(present.geometry.has_value(), "present dst2 normalizes");
  if (!present.geometry) {
    return;
  }
  expect(present.geometry->lanes[0].secondaryDestinationY == 73 &&
             present.geometry->lanes[1].secondaryDestinationY == 73 &&
             present.geometry->lanes[2].secondaryDestinationY == 73,
         "present dst2 applies the same exact integer Y to every lane");
}

void testExpansionRateDefaultsAndOverridesRemainExactIntegers() {
  auto input = completeInput();
  const auto defaulted = normalizeSkinNoteLaneGeometry(input);
  expect(defaulted.geometry.has_value() &&
             defaulted.geometry->expansionRatePercent[0] == 100 &&
             defaulted.geometry->expansionRatePercent[1] == 100,
         "omitted expansion rate retains the pinned integer defaults");

  input.expansionRatePercent = {115, -12};
  const auto overridden = normalizeSkinNoteLaneGeometry(input);
  expect(overridden.geometry.has_value() &&
             overridden.geometry->expansionRatePercent[0] == 115 &&
             overridden.geometry->expansionRatePercent[1] == -12,
         "provided expansion rate remains unchanged as two exact integers");
}

void testMissingLaneDestinationAndUnsafeCardinalityFailClosed() {
  auto missingDestination = completeInput();
  missingDestination.laneDestinations.pop_back();
  const auto missingResult = normalizeSkinNoteLaneGeometry(missingDestination);
  expect(!missingResult.geometry.has_value() &&
             missingResult.error ==
                 SkinNoteLaneGeometryNormalizationError::MissingLaneDestination,
         "each normal lane requires an indexed destination instead of indexing "
         "past dst");

  auto tooManyLanes = completeInput();
  tooManyLanes.normalFirstFrameHeights.resize(
      SkinNoteLaneGeometryNormalizationPolicy::maxLanes + 1, 8.0);
  const auto laneLimitResult = normalizeSkinNoteLaneGeometry(tooManyLanes);
  expect(!laneLimitResult.geometry.has_value() &&
             laneLimitResult.error ==
                 SkinNoteLaneGeometryNormalizationError::LaneLimitExceeded,
         "over-bound normal lane counts fail before allocating output");

  auto tooManySizes = completeInput();
  tooManySizes.authoredNoteHeights.resize(
      SkinNoteLaneGeometryNormalizationPolicy::maxAuthoredNoteHeights + 1, 8.0);
  const auto sizeLimitResult = normalizeSkinNoteLaneGeometry(tooManySizes);
  expect(!sizeLimitResult.geometry.has_value() &&
             sizeLimitResult.error ==
                 SkinNoteLaneGeometryNormalizationError::UnsafeCardinality,
         "over-bound authored size arrays fail closed");
}

void testNonFiniteDestinationAndHeightFailClosed() {
  auto nonfiniteDestination = completeInput();
  nonfiniteDestination.laneDestinations[1].width =
      std::numeric_limits<double>::infinity();
  const auto destinationResult =
      normalizeSkinNoteLaneGeometry(nonfiniteDestination);
  expect(!destinationResult.geometry.has_value() &&
             destinationResult.error ==
                 SkinNoteLaneGeometryNormalizationError::NonFiniteGeometry,
         "non-finite destination geometry fails closed");

  auto nonfiniteHeight = completeInput();
  nonfiniteHeight.authoredNoteHeights = {
      std::numeric_limits<double>::quiet_NaN()};
  const auto heightResult = normalizeSkinNoteLaneGeometry(nonfiniteHeight);
  expect(!heightResult.geometry.has_value() &&
             heightResult.error ==
                 SkinNoteLaneGeometryNormalizationError::NonFiniteGeometry,
         "non-finite authored note height fails closed");
}

void testFiniteOutOfRangeGeometryAndExactIntegersFailClosed() {
  constexpr double oneOver = 8'193.0;

  auto outOfRangeDestination = completeInput();
  outOfRangeDestination.laneDestinations[0].x = oneOver;
  const auto destinationResult =
      normalizeSkinNoteLaneGeometry(outOfRangeDestination);
  expect(!destinationResult.geometry.has_value() &&
             destinationResult.error ==
                 SkinNoteLaneGeometryNormalizationError::NonFiniteGeometry,
         "finite destination components over the authored bound fail closed");

  auto outOfRangeFallbackHeight = completeInput();
  outOfRangeFallbackHeight.normalFirstFrameHeights[1] = oneOver;
  const auto fallbackHeightResult =
      normalizeSkinNoteLaneGeometry(outOfRangeFallbackHeight);
  expect(!fallbackHeightResult.geometry.has_value() &&
             fallbackHeightResult.error ==
                 SkinNoteLaneGeometryNormalizationError::NonFiniteGeometry,
         "finite normal fallback heights over the authored bound fail closed");

  auto outOfRangeAuthoredHeight = completeInput();
  outOfRangeAuthoredHeight.authoredNoteHeights = {oneOver};
  const auto authoredHeightResult =
      normalizeSkinNoteLaneGeometry(outOfRangeAuthoredHeight);
  expect(!authoredHeightResult.geometry.has_value() &&
             authoredHeightResult.error ==
                 SkinNoteLaneGeometryNormalizationError::NonFiniteGeometry,
         "finite authored heights over the authored bound fail closed");

  auto outOfRangeDst2 = completeInput();
  outOfRangeDst2.secondaryDestinationY = 8'193;
  const auto dst2Result = normalizeSkinNoteLaneGeometry(outOfRangeDst2);
  expect(!dst2Result.geometry.has_value() &&
             dst2Result.error ==
                 SkinNoteLaneGeometryNormalizationError::NonFiniteGeometry,
         "finite dst2 values over the authored bound fail closed");

  auto outOfRangeExpansion = completeInput();
  outOfRangeExpansion.expansionRatePercent = {8'193, 100};
  const auto expansionResult =
      normalizeSkinNoteLaneGeometry(outOfRangeExpansion);
  expect(!expansionResult.geometry.has_value() &&
             expansionResult.error ==
                 SkinNoteLaneGeometryNormalizationError::NonFiniteGeometry,
         "finite expansion percentages over the authored bound fail closed");
}

} // namespace

int main() {
  testMixedSizePrefixUsesNormalFirstFrameFallbackInLaneOrder();
  testMissingNormalMetadataPreservesDeferredHeight();
  testDst2AbsentAndSentinelRemainUnsetWhilePresentValueAppliesToEveryLane();
  testExpansionRateDefaultsAndOverridesRemainExactIntegers();
  testMissingLaneDestinationAndUnsafeCardinalityFailClosed();
  testNonFiniteDestinationAndHeightFailClosed();
  testFiniteOutOfRangeGeometryAndExactIntegersFailClosed();
  return failures == 0 ? 0 : 1;
}
