#include "skin/beatoraja/SkinCoverNormalization.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
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

SkinSpriteFrames sprite() {
  return {.resource = 41,
          .frames = {{.x = 3,
                      .y = 5,
                      .w = 7,
                      .h = 11,
                      .gridColumn = 0,
                      .gridRow = 0,
                      .gridColumns = 1,
                      .gridRows = 1},
                     {.x = 13,
                      .y = 17,
                      .w = 19,
                      .h = 23,
                      .gridColumn = 0,
                      .gridRow = 0,
                      .gridColumns = 1,
                      .gridRows = 1}},
          .cycleMillis = 240,
          .timer = SkinTimerPropertyId{29}};
}

SkinCoverNormalizationInput input(SkinCoverKind kind) {
  return {.kind = kind,
          .sprite = sprite(),
          .authoredDisappearLine = std::nullopt,
          .authoredDisappearLineLinksLift = std::nullopt,
          .lineScale = 1.0,
          .authoredDestinationOffsetIds = {44, 44, -9}};
}

void testPinnedDefaultsAppendOffsetsWithoutChangingSpriteAnimation() {
  auto hiddenInput = input(SkinCoverKind::Hidden);
  hiddenInput.lineScale = 1.25;
  const auto hidden = normalizeSkinCover(hiddenInput);
  const auto lift = normalizeSkinCover(input(SkinCoverKind::Lift));

  expect(hidden.cover.has_value() && lift.cover.has_value(),
         "valid hidden and lift covers normalize");
  expect(hidden.error == SkinCoverNormalizationError::None &&
             lift.error == SkinCoverNormalizationError::None,
         "valid cover normalization reports no error");
  if (!hidden.cover || !lift.cover) {
    return;
  }

  expect(hidden.cover->kind == SkinCoverKind::Hidden &&
             hidden.cover->disappearLine == -1.25 &&
             hidden.cover->disappearLineLinksLift,
         "Hidden scales its pinned default line and remains lift-linked");
  expect(lift.cover->kind == SkinCoverKind::Lift &&
             lift.cover->disappearLine == -1.0 &&
             !lift.cover->disappearLineLinksLift,
         "Lift defaults to unclipped and not lift-linked");
  expect(hidden.cover->sprite.resource == 41 &&
             hidden.cover->sprite.frames.size() == 2 &&
             hidden.cover->sprite.frames[1].h == 23 &&
             hidden.cover->sprite.cycleMillis == 240 &&
             hidden.cover->sprite.timer == SkinTimerPropertyId{29},
         "cover retains expanded sprite frames, cycle, and timer");
  expect(hidden.destinationOffsetIds ==
             std::vector<int>({44, 44, -9, kSkinCoverLiftOffsetId,
                               kSkinCoverHiddenOffsetId}),
         "Hidden preserves authored offset duplicates and order before pinned offsets");
  expect(lift.destinationOffsetIds ==
             std::vector<int>({44, 44, -9, kSkinCoverLiftOffsetId}),
         "Lift preserves authored offset duplicates and order before pinned offset");
}

void testAuthoredLineScaleAndLinkOverrideRemainRuntimeVisible() {
  auto hiddenInput = input(SkinCoverKind::Hidden);
  hiddenInput.authoredDisappearLine = 120.0;
  hiddenInput.authoredDisappearLineLinksLift = false;
  hiddenInput.lineScale = 1.25;
  const auto hidden = normalizeSkinCover(hiddenInput);

  auto liftInput = input(SkinCoverKind::Lift);
  liftInput.authoredDisappearLine = 120.0;
  liftInput.authoredDisappearLineLinksLift = true;
  liftInput.lineScale = 1.25;
  const auto lift = normalizeSkinCover(liftInput);

  expect(hidden.cover && lift.cover, "authored cover overrides normalize");
  if (!hidden.cover || !lift.cover) {
    return;
  }
  expect(hidden.cover->disappearLine == 150.0 &&
             lift.cover->disappearLine == 150.0,
         "authored disappear line retains the loader's Y scale");
  expect(!hidden.cover->disappearLineLinksLift &&
             lift.cover->disappearLineLinksLift,
         "authored link override remains visible to later clipping");
  expect(hidden.cover->disappearLine >= 0.0 &&
             !hidden.cover->disappearLineLinksLift &&
             lift.cover->disappearLine >= 0.0 &&
             lift.cover->disappearLineLinksLift,
         "later runtime clipping can shift only the explicitly linked cover by lift");
}

void testUnsafeInputFailsClosedWithStructuredErrors() {
  auto invalidSprite = input(SkinCoverKind::Hidden);
  invalidSprite.sprite.frames.clear();
  const auto spriteResult = normalizeSkinCover(invalidSprite);
  expect(!spriteResult.cover &&
             spriteResult.error == SkinCoverNormalizationError::InvalidSprite,
         "empty expanded sprite fails closed");

  auto nonfiniteLine = input(SkinCoverKind::Hidden);
  nonfiniteLine.authoredDisappearLine = std::numeric_limits<double>::quiet_NaN();
  const auto lineResult = normalizeSkinCover(nonfiniteLine);
  expect(!lineResult.cover &&
             lineResult.error == SkinCoverNormalizationError::InvalidDisappearLine,
         "non-finite disappear line fails closed");

  auto unsafeScale = input(SkinCoverKind::Lift);
  unsafeScale.lineScale = std::numeric_limits<double>::infinity();
  const auto scaleResult = normalizeSkinCover(unsafeScale);
  expect(!scaleResult.cover &&
             scaleResult.error == SkinCoverNormalizationError::InvalidLineScale,
         "non-finite line scale fails closed even for the default line");

  auto excessiveOffsets = input(SkinCoverKind::Hidden);
  excessiveOffsets.authoredDestinationOffsetIds.assign(
      SkinCoverNormalizationPolicy::maxAuthoredDestinationOffsetIds + 1, 7);
  const auto offsetsResult = normalizeSkinCover(excessiveOffsets);
  expect(!offsetsResult.cover &&
             offsetsResult.error ==
                 SkinCoverNormalizationError::DestinationOffsetLimitExceeded,
         "unbounded authored destination offsets fail closed");
}

} // namespace

int main() {
  testPinnedDefaultsAppendOffsetsWithoutChangingSpriteAnimation();
  testAuthoredLineScaleAndLinkOverrideRemainRuntimeVisible();
  testUnsafeInputFailsClosedWithStructuredErrors();
  return failures == 0 ? 0 : 1;
}
