#include "skin/beatoraja/SkinDestinationEvaluator.h"

#include <array>
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

bool near(double actual, double expected, double epsilon = 1e-6) {
  return std::abs(actual - expected) <= epsilon;
}

SkinDestinationBody animated(int acceleration = 0) {
  SkinDestinationBody body;
  body.timer = SkinTimerPropertyId{1};
  body.loop = -1;
  body.center = 2;
  body.blend = SkinBlendMode::Additive;
  body.filter = SkinFilterMode::Linear;
  body.frames = {
      {.timeMillis = 100,
       .x = 10.0,
       .y = 20.0,
       .width = 80.0,
       .height = 40.0,
       .angleDegrees = 10.0,
       .rgba = {0, 64, 32, 128},
       .acceleration = acceleration,
       .clip = SkinSourceRect{.x = 5, .y = 10, .w = 60, .h = 20}},
      {.timeMillis = 1100,
       .x = 30.0,
       .y = 60.0,
       .width = 160.0,
       .height = 80.0,
       .angleDegrees = 91.0,
       .rgba = {255, 64, 32, 255},
       .acceleration = acceleration,
       .clip = SkinSourceRect{.x = 13, .y = 14, .w = 140, .h = 60}},
  };
  return body;
}

SkinDestinationEvaluationInputs
inputs(std::int64_t nowMicros, std::int64_t startMicros,
       std::span<const bool> conditions = {},
       std::span<const SkinRuntimeOffset> offsets = {},
       std::optional<bool> timerOff = std::nullopt) {
  return {.nowMicros = nowMicros,
          .timerStartMicros = startMicros,
          .timerOff = timerOff.value_or(startMicros == INT64_MIN),
          .optionConditions = conditions,
          .orderedOffsets = offsets};
}

void testTimerConditionAndFrameSelection() {
  auto body = animated();
  auto off = evaluateSkinDestinationAuthored(
      body, inputs(600'000, std::numeric_limits<std::int64_t>::min()));
  expect(!off.geometry, "INT64_MIN timer is the exact timer-off sentinel");

  auto pre = evaluateSkinDestinationAuthored(body, inputs(99'000, 0));
  expect(!pre.geometry, "before the first destination frame is suppressed");

  auto half = evaluateSkinDestinationAuthored(body, inputs(600'000, 0));
  expect(half.geometry.has_value(), "timer-relative middle frame evaluates");
  if (half.geometry) {
    expect(near(half.geometry->rect.x, 20.0) &&
               near(half.geometry->rect.y, 40.0),
           "linear geometry interpolation follows destination milliseconds");
    expect(near(half.geometry->angleDegrees, 50.0),
           "angle interpolation truncates before offsets");
    expect(near(half.geometry->rgba[0], 0.5) &&
               near(half.geometry->rgba[3], 191.5 / 255.0),
           "interpolated RGBA remains normalized and fractional");
    expect(half.geometry->clip && near(half.geometry->clip->x, 9.0) &&
               near(half.geometry->clip->width, 100.0),
           "clips interpolate with the frame interval");
    expect(half.geometry->centerX == 0.5 && half.geometry->centerY == 0.0,
           "destination center id maps to the pinned pivot");
    expect(half.geometry->blend == SkinBlendMode::Additive &&
               half.geometry->filter == SkinFilterMode::Linear,
           "presentation blend and filter carry through evaluation");
  }

  auto stopped = evaluateSkinDestinationAuthored(body, inputs(1'101'000, 0));
  expect(!stopped.geometry, "loop -1 suppresses strictly after the end frame");

  body.loop = 100;
  const auto sentinelRead = evaluateSkinDestinationAuthored(
      body, inputs(0, INT64_MIN, {}, {}, false));
  expect(sentinelRead.geometry.has_value(),
         "an ON timer keeps an independent INT64_MIN value read active");
}

void testLoopRateAndIndependentMicrosecondTruncation() {
  auto body = animated();
  body.loop = 400;
  auto looped = evaluateSkinDestinationAuthored(body, inputs(1'250'000, 0));
  expect(looped.geometry && near(looped.geometry->rect.x, 19.0),
         "loop point modulo is applied before frame selection");

  auto truncation = evaluateSkinDestinationAuthored(body, inputs(600'100, 900));
  expect(truncation.geometry && near(truncation.geometry->rect.x, 20.0),
         "now and timer micros are independently truncated toward zero before "
         "subtracting");

  for (const auto [acceleration, expected] :
       std::array<std::pair<int, double>, 4>{
           {{0, 15.0}, {1, 11.25}, {2, 18.75}, {3, 10.0}}}) {
    auto rateBody = animated(acceleration);
    const auto result =
        evaluateSkinDestinationAuthored(rateBody, inputs(350'000, 0));
    expect(result.geometry && near(result.geometry->rect.x, expected),
           "pinned acceleration transforms the raw interval rate");
  }
}

void testOmittedLoopUsesPinnedZeroDefaultAndWrapsExactEnd() {
  SkinDestinationBody body;
  body.frames = {
      {.timeMillis = 0, .x = 10.0, .y = 0.0, .width = 1.0, .height = 1.0},
      {.timeMillis = 100, .x = 110.0, .y = 0.0, .width = 1.0, .height = 1.0},
  };
  const auto beforeEnd =
      evaluateSkinDestinationAuthored(body, inputs(99'000, 0));
  const auto exactEnd =
      evaluateSkinDestinationAuthored(body, inputs(100'000, 0));
  expect(body.loop == 0 && beforeEnd.geometry && exactEnd.geometry &&
             near(beforeEnd.geometry->rect.x, 109.0) &&
             near(exactEnd.geometry->rect.x, 10.0),
         "omitted Java Destination loop defaults to zero and exact end wraps "
         "to the first frame");
}

void testConditionsAndOrderedOffsets() {
  auto body = animated();
  body.conditions = {1, SkinBooleanPropertyId{2}};
  body.drawCondition = SkinBooleanPropertyId{3};
  const std::array<bool, 3> enabled = {true, true, true};
  const std::array<bool, 3> hidden = {true, false, true};
  auto visible =
      evaluateSkinDestinationAuthored(body, inputs(600'000, 0, enabled));
  expect(visible.geometry.has_value(),
         "conditions map in authored order plus draw condition");
  auto suppressed =
      evaluateSkinDestinationAuthored(body, inputs(600'000, 0, hidden));
  expect(!suppressed.geometry, "a false resolved condition suppresses drawing");
  const std::array<bool, 2> mismatch = {true, true};
  auto malformed =
      evaluateSkinDestinationAuthored(body, inputs(600'000, 0, mismatch));
  expect(!malformed.geometry && !malformed.diagnostics.empty(),
         "condition length mismatch diagnoses and suppresses");

  body.conditions.clear();
  body.drawCondition.reset();
  const std::array<SkinRuntimeOffset, 2> offsets = {
      {{.x = 14, .y = 7, .w = 20, .h = 10, .r = 10, .a = 0},
       {.x = 0, .y = 0, .w = 0, .h = 0, .r = 5, .a = 100}}};
  auto adjusted =
      evaluateSkinDestinationAuthored(body, inputs(600'000, 0, {}, offsets));
  expect(adjusted.geometry && near(adjusted.geometry->rect.x, 24.0) &&
             near(adjusted.geometry->rect.y, 42.0) &&
             near(adjusted.geometry->rect.width, 140.0) &&
             near(adjusted.geometry->angleDegrees, 65.0),
         "ordered non-relative offsets adjust position size and angle before "
         "projection");
  expect(adjusted.geometry && near(adjusted.geometry->rgba[3], 191.5 / 255.0),
         "interpolated color preserves the reference alpha-offset early-return "
         "quirk");
}

void testEmptyDestinationIsSilentlyDroppedBeforeConditions() {
  SkinDestinationBody body;
  body.conditions = {1, SkinBooleanPropertyId{2}};
  body.drawCondition = SkinBooleanPropertyId{3};
  const std::array<bool, 1> intentionallyIncomplete = {false};
  const auto result = evaluateSkinDestinationAuthored(
      body, inputs(600'000, 0, intentionallyIncomplete));
  expect(!result.geometry && result.diagnostics.empty(),
         "an empty destination is silently dropped before condition "
         "validation, matching Beatoraja Skin.prepare");
}

void testObjectAccelerationAndPinnedOffsetAlphaBranches() {
  auto latched = animated();
  latched.frames[0].acceleration = 0;
  latched.frames[1].acceleration = 1;
  const auto laterAcceleration =
      evaluateSkinDestinationAuthored(latched, inputs(350'000, 0));
  expect(laterAcceleration.geometry &&
             near(laterAcceleration.geometry->rect.x, 11.25),
         "the first nonzero authored acceleration latches for the object");

  auto fixedColor = animated();
  fixedColor.frames[0].rgba = {1, 2, 3, 100};
  fixedColor.frames[1].rgba = {1, 2, 3, 100};
  const std::array<SkinRuntimeOffset, 1> raiseAlpha = {{{.a = 200}}};
  const auto fixedMidpoint = evaluateSkinDestinationAuthored(
      fixedColor, inputs(600'000, 0, {}, raiseAlpha));
  expect(fixedMidpoint.geometry && near(fixedMidpoint.geometry->rgba[3], 1.0),
         "fixed color applies and clamps alpha offsets even mid-interval");

  auto stepped = animated(3);
  const std::array<SkinRuntimeOffset, 1> lowerAlpha = {{{.a = -200}}};
  const auto steppedMidpoint = evaluateSkinDestinationAuthored(
      stepped, inputs(600'000, 0, {}, lowerAlpha));
  expect(steppedMidpoint.geometry &&
             near(steppedMidpoint.geometry->rgba[3], 128.0 / 255.0),
         "changing step color skips alpha offsets between keyframes");
  for (const auto [time, alpha] :
       std::array<std::pair<std::int64_t, double>, 2>{{{100'000, 0.0},
                                                     {1'100'000, 55.0 / 255.0}}}) {
    const auto endpoint = evaluateSkinDestinationAuthored(
        stepped, inputs(time, 0, {}, lowerAlpha));
    expect(endpoint.geometry && near(endpoint.geometry->rgba[3], alpha),
           "changing step color applies alpha offsets at exact keyframes");
  }
  stepped.frames[1].rgba = stepped.frames[0].rgba;
  const auto fixedStep = evaluateSkinDestinationAuthored(
      stepped, inputs(600'000, 0, {}, lowerAlpha));
  expect(fixedStep.geometry && near(fixedStep.geometry->rgba[3], 0.0),
         "fixed step color still applies and clamps mid-interval alpha offsets");
}

void testFractionalOffsetAndClipSuppression() {
  auto body = animated();
  const std::array<SkinRuntimeOffset, 1> odd = {
      {{.x = 3, .y = -5, .w = 5, .h = -3}}};
  const auto halfShift =
      evaluateSkinDestinationAuthored(body, inputs(100'000, 0, {}, odd));
  expect(halfShift.geometry && near(halfShift.geometry->rect.x, 10.5) &&
             near(halfShift.geometry->rect.y, 16.5) &&
             near(halfShift.geometry->rect.width, 85.0) &&
             near(halfShift.geometry->rect.height, 37.0),
         "odd positive and negative offsets preserve Java floating half-unit "
         "shifts");

  const std::array<SkinRuntimeOffset, 1> zeroWidth = {{{.w = -60}}};
  const auto noWidthClip =
      evaluateSkinDestinationAuthored(body, inputs(100'000, 0, {}, zeroWidth));
  expect(noWidthClip.geometry && !noWidthClip.geometry->clip,
         "zero-width offset-adjusted clips are disabled");
  const std::array<SkinRuntimeOffset, 1> negativeHeight = {{{.h = -21}}};
  const auto noHeightClip = evaluateSkinDestinationAuthored(
      body, inputs(100'000, 0, {}, negativeHeight));
  expect(noHeightClip.geometry && !noHeightClip.geometry->clip,
         "negative-height offset-adjusted clips are disabled");
}

void testSourceRegionStretchAndProjection() {
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 200.0, .height = 100.0},
      {.x = 0.0, .y = 0.0, .width = 200.0, .height = 100.0}, {});
  AuthoredDestinationGeometry geometry;
  geometry.rect = {.x = 10.0, .y = 20.0, .width = 90.0, .height = 30.0};
  SkinSourceRegionGeometry source{
      .textureWidth = 400,
      .textureHeight = 300,
      .region = {.x = 101, .y = 51, .w = 61, .h = 41}};
  struct Expected {
    double x;
    double y;
    double width;
    double height;
    int regionX;
    int regionY;
    int regionWidth;
    int regionHeight;
  };
  const std::array<Expected, 11> expected = {
      {{10.0, 20.0, 90.0, 30.0, 101, 51, 61, 41},
       {32.6829268293, 20.0, 44.6341463415, 30.0, 101, 51, 61, 41},
       {10.0, 4.7540983607, 90.0, 60.4918032787, 101, 51, 61, 41},
       {10.0, 20.0, 90.0, 30.0, 101, 61, 61, 20},
       {10.0, 4.7540983607, 90.0, 60.4918032787, 101, 51, 61, 41},
       {10.0, 20.0, 90.0, 30.0, 101, 61, 61, 20},
       {32.6829268293, 20.0, 44.6341463415, 30.0, 101, 51, 61, 41},
       {32.6829268293, 20.0, 44.6341463415, 30.0, 101, 51, 61, 41},
       {32.6829268293, 20.0, 44.6341463415, 30.0, 101, 51, 61, 41},
       {24.5, 14.5, 61.0, 41.0, 101, 51, 61, 41},
       {24.5, 20.0, 61.0, 30.0, 101, 56, 61, 30}}};
  for (int id = 0; id <= 10; ++id) {
    geometry.stretch = static_cast<SkinStretchMode>(id);
    const auto projected =
        projectSkinDestinationToUi(geometry, source, viewport);
    const auto &want = expected[static_cast<std::size_t>(id)];
    expect(near(projected.vertices[0][0], want.x) &&
               near(projected.vertices[0][1], 100.0 - want.y) &&
               near(projected.vertices[2][0], want.x + want.width) &&
               near(projected.vertices[2][1], 100.0 - want.y - want.height),
           "every stretch id has exact adjusted destination vertices");
    expect(
        near(projected.normalizedUvs[0][0],
             static_cast<double>(want.regionX) / 400.0) &&
            near(projected.normalizedUvs[0][1],
                 static_cast<double>(want.regionY + want.regionHeight) /
                     300.0) &&
            near(projected.normalizedUvs[2][0],
                 static_cast<double>(want.regionX + want.regionWidth) /
                     400.0) &&
            near(projected.normalizedUvs[2][1],
                 static_cast<double>(want.regionY) / 300.0),
        "every stretch id binds the top-origin source rows to the matching "
        "screen rows");
  }

  geometry.stretch = SkinStretchMode::KeepAspectRatioFitWidthTrimmed;
  const auto trimmed = projectSkinDestinationToUi(geometry, source, viewport);
  expect(near(trimmed.normalizedUvs[0][1], 81.0 / 300.0) &&
             near(trimmed.normalizedUvs[2][1], 61.0 / 300.0),
         "trimmed cropping uses centered Java truncation inside a non-origin "
         "source region");
  geometry.stretch = SkinStretchMode::NoResize;
  const auto noResize = projectSkinDestinationToUi(geometry, source, viewport);
  expect(near(noResize.vertices[0][0], 24.5) &&
             near(noResize.vertices[2][0], 85.5),
         "no-resize centers the source-sized destination without expansion");

  geometry.rect = {.x = 10.0, .y = 20.0, .width = 50.0, .height = 30.0};
  geometry.stretch = SkinStretchMode::NoResizeTrimmed;
  const auto bothTrimmed =
      projectSkinDestinationToUi(geometry, source, viewport);
  expect(near(bothTrimmed.normalizedUvs[0][0], 106.0 / 400.0) &&
             near(bothTrimmed.normalizedUvs[0][1], 86.0 / 300.0) &&
             near(bothTrimmed.normalizedUvs[2][0], 156.0 / 400.0) &&
             near(bothTrimmed.normalizedUvs[2][1], 56.0 / 300.0),
         "no-resize-trimmed crops both source axes in copied-region order");

  geometry.rect = {.x = 10.0, .y = 20.0, .width = 90.0, .height = 30.0};
  geometry.stretch = SkinStretchMode::NoResize;
  geometry.centerX = 0.5;
  geometry.centerY = 0.5;
  geometry.angleDegrees = 90.0;
  const auto rotated = projectSkinDestinationToUi(geometry, source, viewport);
  expect(near(rotated.vertices[0][0], 75.5) &&
             near(rotated.vertices[0][1], 95.5) &&
             near(rotated.vertices[2][0], 34.5) &&
             near(rotated.vertices[2][1], 34.5),
         "rotation occurs after stretch adjustment");

  const auto invalidTexture = projectSkinDestinationToUi(
      geometry,
      {.textureWidth = 0, .textureHeight = 300, .region = source.region},
      viewport);
  const auto invalidRegion =
      projectSkinDestinationToUi(geometry,
                                 {.textureWidth = 400,
                                  .textureHeight = 300,
                                  .region = {.x = 1, .y = 1, .w = 0, .h = 1}},
                                 viewport);
  expect(
      invalidTexture.vertices == UiDestinationGeometry{}.vertices &&
          invalidRegion.normalizedUvs == UiDestinationGeometry{}.normalizedUvs,
      "zero texture or source dimensions do not produce projectable geometry");

  geometry.angleDegrees = 0.0;
  geometry.clip =
      SkinAuthoredRect{.x = 10.0, .y = 20.0, .width = -1.0, .height = 4.0};
  const auto negativeClip =
      projectSkinDestinationToUi(geometry, source, viewport);
  expect(
      !negativeClip.clip,
      "projection never resurrects a negative clip with absolute dimensions");
}

void testBilinearFilterUsesStretchedAuthoredSize() {
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 100, .height = 100}, {.x = 0, .y = 0, .width = 200, .height = 200}, {});
  AuthoredDestinationGeometry geometry{
      .rect = {.x = 0, .y = 0, .width = 4, .height = 2},
      .angleDegrees = 45, .filter = SkinFilterMode::BeatorajaBilinear};
  const SkinSourceRegionGeometry source{
      .textureWidth = 8, .textureHeight = 8, .region = {.x = 0, .y = 0, .w = 4, .h = 2}};
  expect(projectSkinDestinationToUi(geometry, source, viewport).filter == SkinFilterMode::Nearest,
         "Standard projection retains its authored-size filtering rule");
  geometry.rect.width = 8;
  expect(projectSkinDestinationToUi(geometry, source, viewport).filter == SkinFilterMode::BeatorajaBilinear,
         "resized image retains Beatoraja bilinear sampling");
  geometry.stretch = SkinStretchMode::NoResize;
  expect(projectSkinDestinationToUi(geometry, source, viewport).filter == SkinFilterMode::Nearest,
         "no-resize stretch bypasses bilinear after restoring source dimensions");
  for (const auto region : {SkinSourceRect{.x = 4, .y = 0, .w = -4, .h = 2},
                            SkinSourceRect{.x = 0, .y = 2, .w = 4, .h = -2}}) {
    auto flipped = source;
    flipped.region = region;
    expect(projectSkinDestinationToUi(geometry, flipped, viewport).filter == SkinFilterMode::Nearest,
           "flipped no-resize images compare intrinsic positive dimensions for sampling");
  }
}

void testCompatibilityUsesDestinationResolutionBeforeStretchAndRotation() {
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 100, .height = 100},
      {.x = 10, .y = 20, .width = 300, .height = 200},
      {.mode = ViewportMode::Stretch});
  AuthoredDestinationGeometry geometry{
      .rect = {.x = 10, .y = 20, .width = 4, .height = 2},
      .centerX = 0.5, .centerY = 0.5,
      .filter = SkinFilterMode::BeatorajaBilinear,
      .stretch = SkinStretchMode::NoResize,
      .useDestinationResolution = true};
  const SkinSourceRegionGeometry source{
      .textureWidth = 64, .textureHeight = 64,
      .region = {.x = 10, .y = 20, .w = 8, .h = 6}};
  const auto checkCorners = [](const UiDestinationGeometry &projected,
                               double x0, double y0, double x2, double y2,
                               std::string_view message) {
    expect(near(projected.vertices[0][0], x0) &&
               near(projected.vertices[0][1], y0) &&
               near(projected.vertices[2][0], x2) &&
               near(projected.vertices[2][1], y2), message);
  };
  const auto noResize = projectSkinDestinationToUi(geometry, source, viewport);
  checkCorners(noResize, 42, 181, 50, 175,
               "compatibility no-resize preserves source dimensions in the logical destination canvas");
  expect(noResize.filter == SkinFilterMode::Nearest,
         "no-resize suppresses bilinear after destination-resolution stretch");
  geometry.useDestinationResolution = false;
  checkCorners(projectSkinDestinationToUi(geometry, source, viewport),
               34, 184, 58, 172,
               "Standard no-resize continues to stretch in authored units");
  geometry.useDestinationResolution = true;
  geometry.stretch = SkinStretchMode::Stretch;
  geometry.angleDegrees = 90;
  geometry.filter = SkinFilterMode::Nearest;
  checkCorners(projectSkinDestinationToUi(geometry, source, viewport),
               48, 184, 44, 172,
               "unfiltered compatibility rotation follows nonuniform destination scaling");

  geometry.angleDegrees = 0;
  geometry.filter = SkinFilterMode::BeatorajaBilinear;
  geometry.stretch = SkinStretchMode::NoResizeTrimmed;
  geometry.rect.width = 2;
  geometry.rect.height = 1;
  const auto trimmed = projectSkinDestinationToUi(geometry, source, viewport);
  checkCorners(trimmed, 40, 180, 46, 178,
               "trimmed compatibility dimensions use the scaled destination on both axes");
  expect(near(trimmed.normalizedUvs[0][0], 11.0 / 64) &&
             near(trimmed.normalizedUvs[0][1], 24.0 / 64) &&
             near(trimmed.normalizedUvs[2][0], 17.0 / 64) &&
             near(trimmed.normalizedUvs[2][1], 22.0 / 64) &&
             trimmed.filter == SkinFilterMode::Nearest,
         "two-axis source trimming and sampling use destination-resolution dimensions");

  geometry.rect.width = 4;
  geometry.rect.height = 2;
  geometry.stretch = SkinStretchMode::NoResize;
  auto flipped = source;
  flipped.region = {.x = 18, .y = 26, .w = -8, .h = -6};
  const auto flippedResult = projectSkinDestinationToUi(geometry, flipped, viewport);
  checkCorners(flippedResult, 42, 181, 50, 175,
               "flipped compatibility sources keep positive intrinsic no-resize dimensions");
  expect(flippedResult.filter == SkinFilterMode::Nearest &&
             near(flippedResult.normalizedUvs[0][0], 18.0 / 64) &&
             near(flippedResult.normalizedUvs[0][1], 20.0 / 64) &&
             near(flippedResult.normalizedUvs[2][0], 10.0 / 64) &&
             near(flippedResult.normalizedUvs[2][1], 26.0 / 64),
         "flips retain their UV direction when unity filtering is suppressed");

  geometry.clip = AuthoredRect{.x = 1, .y = 2, .width = 3, .height = 4};
  const auto custom = evaluatePlaySkinViewport(
      {.width = 100, .height = 100},
      {.x = 10, .y = 20, .width = 300, .height = 200},
      {.mode = ViewportMode::Custom, .customBase = CustomViewportBase::Stretch,
       .scaleX = 2, .scaleY = 0.5, .translateX = 7, .translateY = -9});
  const auto customized = projectSkinDestinationToUi(geometry, source, custom);
  checkCorners(customized, -69, 141.5, -53, 138.5,
               "custom scaling remains after logical destination no-resize stretch");
  expect(customized.filter == SkinFilterMode::Nearest && customized.clip &&
             near(customized.clip->x, -127) && near(customized.clip->y, 155) &&
             near(customized.clip->width, 18) && near(customized.clip->height, 4),
         "custom scale does not select bilinear and clips retain the complete authored transform");
  const auto fit = evaluatePlaySkinViewport(
      {.width = 100, .height = 100},
      {.x = 10, .y = 20, .width = 300, .height = 200}, {});
  checkCorners(projectSkinDestinationToUi(geometry, source, fit),
               80, 181, 88, 175,
               "Fit selects a centered logical destination canvas before no-resize stretch");
}

void testCompatibilityFilterComparesLogicalDestinationSize() {
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 100, .height = 100},
      {.x = 0, .y = 0, .width = 200, .height = 200}, {});
  AuthoredDestinationGeometry geometry{
      .rect = {.x = 0, .y = 0, .width = 4, .height = 2},
      .filter = SkinFilterMode::BeatorajaBilinear,
      .useDestinationResolution = true};
  const SkinSourceRegionGeometry source{
      .textureWidth = 8, .textureHeight = 8,
      .region = {.x = 0, .y = 0, .w = 4, .h = 2}};
  expect(projectSkinDestinationToUi(geometry, source, viewport).filter ==
             SkinFilterMode::BeatorajaBilinear,
         "destination-resolution enlargement selects bilinear even at authored unity size");
  geometry.rect.width = 2;
  geometry.rect.height = 1;
  expect(projectSkinDestinationToUi(geometry, source, viewport).filter ==
             SkinFilterMode::Nearest,
         "destination-resolution scaling to exact source size suppresses bilinear");
}

} // namespace

int main() {
  testBilinearFilterUsesStretchedAuthoredSize();
  testCompatibilityUsesDestinationResolutionBeforeStretchAndRotation();
  testCompatibilityFilterComparesLogicalDestinationSize();
  testTimerConditionAndFrameSelection();
  testLoopRateAndIndependentMicrosecondTruncation();
  testOmittedLoopUsesPinnedZeroDefaultAndWrapsExactEnd();
  testConditionsAndOrderedOffsets();
  testEmptyDestinationIsSilentlyDroppedBeforeConditions();
  testObjectAccelerationAndPinnedOffsetAlphaBranches();
  testFractionalOffsetAndClipSuppression();
  testSourceRegionStretchAndProjection();
  return failures == 0 ? 0 : 1;
}
