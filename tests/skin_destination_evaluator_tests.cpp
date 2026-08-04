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
      {.timeMillis = 100, .x = 10.0, .y = 20.0, .width = 80.0, .height = 40.0,
       .angleDegrees = 10.0, .rgba = {0, 64, 32, 128}, .acceleration = acceleration,
       .clip = SkinSourceRect{.x = 5, .y = 10, .w = 60, .h = 20}},
      {.timeMillis = 1100, .x = 30.0, .y = 60.0, .width = 160.0, .height = 80.0,
       .angleDegrees = 91.0, .rgba = {255, 64, 32, 255}, .acceleration = acceleration,
       .clip = SkinSourceRect{.x = 13, .y = 14, .w = 140, .h = 60}},
  };
  return body;
}

SkinDestinationEvaluationInputs inputs(std::int64_t nowMicros, std::int64_t startMicros,
                                       std::span<const bool> conditions = {},
                                       std::span<const ConfigOffset> offsets = {}) {
  return {.nowMicros = nowMicros, .timerStartMicros = startMicros,
          .optionConditions = conditions, .orderedOffsets = offsets};
}

void testTimerConditionAndFrameSelection() {
  auto body = animated();
  auto off = evaluateSkinDestinationAuthored(body, inputs(600'000, std::numeric_limits<std::int64_t>::min()));
  expect(!off.geometry, "INT64_MIN timer is the exact timer-off sentinel");

  auto pre = evaluateSkinDestinationAuthored(body, inputs(99'000, 0));
  expect(!pre.geometry, "before the first destination frame is suppressed");

  auto half = evaluateSkinDestinationAuthored(body, inputs(600'000, 0));
  expect(half.geometry.has_value(), "timer-relative middle frame evaluates");
  if (half.geometry) {
    expect(near(half.geometry->rect.x, 20.0) && near(half.geometry->rect.y, 40.0),
           "linear geometry interpolation follows destination milliseconds");
    expect(near(half.geometry->angleDegrees, 50.0), "angle interpolation truncates before offsets");
    expect(near(half.geometry->rgba[0], 0.5) && near(half.geometry->rgba[3], 191.5 / 255.0),
           "interpolated RGBA remains normalized and fractional");
    expect(half.geometry->clip && near(half.geometry->clip->x, 9.0) && near(half.geometry->clip->width, 100.0),
           "clips interpolate with the frame interval");
    expect(half.geometry->centerX == 0.5 && half.geometry->centerY == 0.0,
           "destination center id maps to the pinned pivot");
    expect(half.geometry->blend == SkinBlendMode::Additive && half.geometry->filter == SkinFilterMode::Linear,
           "presentation blend and filter carry through evaluation");
  }

  auto stopped = evaluateSkinDestinationAuthored(body, inputs(1'101'000, 0));
  expect(!stopped.geometry, "loop -1 suppresses strictly after the end frame");
}

void testLoopRateAndIndependentMicrosecondTruncation() {
  auto body = animated();
  body.loop = 400;
  auto looped = evaluateSkinDestinationAuthored(body, inputs(1'250'000, 0));
  expect(looped.geometry && near(looped.geometry->rect.x, 19.0), "loop point modulo is applied before frame selection");

  auto truncation = evaluateSkinDestinationAuthored(body, inputs(600'100, 900));
  expect(truncation.geometry && near(truncation.geometry->rect.x, 20.0),
         "now and timer micros are independently truncated toward zero before subtracting");

  for (const auto [acceleration, expected] : std::array<std::pair<int, double>, 4>{{{0, 15.0}, {1, 11.25}, {2, 18.75}, {3, 10.0}}}) {
    auto rateBody = animated(acceleration);
    const auto result = evaluateSkinDestinationAuthored(rateBody, inputs(350'000, 0));
    expect(result.geometry && near(result.geometry->rect.x, expected), "pinned acceleration transforms the raw interval rate");
  }
}

void testConditionsAndOrderedOffsets() {
  auto body = animated();
  body.conditions = {1, SkinBooleanPropertyId{2}};
  body.drawCondition = SkinBooleanPropertyId{3};
  const std::array<bool, 3> enabled = {true, true, true};
  const std::array<bool, 3> hidden = {true, false, true};
  auto visible = evaluateSkinDestinationAuthored(body, inputs(600'000, 0, enabled));
  expect(visible.geometry.has_value(), "conditions map in authored order plus draw condition");
  auto suppressed = evaluateSkinDestinationAuthored(body, inputs(600'000, 0, hidden));
  expect(!suppressed.geometry, "a false resolved condition suppresses drawing");
  const std::array<bool, 2> mismatch = {true, true};
  auto malformed = evaluateSkinDestinationAuthored(body, inputs(600'000, 0, mismatch));
  expect(!malformed.geometry && !malformed.diagnostics.empty(), "condition length mismatch diagnoses and suppresses");

  body.conditions.clear();
  body.drawCondition.reset();
  const std::array<ConfigOffset, 2> offsets = {{{.x = 14, .y = 7, .w = 20, .h = 10, .r = 10, .a = 0},
                                                  {.x = 0, .y = 0, .w = 0, .h = 0, .r = 5, .a = 100}}};
  auto adjusted = evaluateSkinDestinationAuthored(body, inputs(600'000, 0, {}, offsets));
  expect(adjusted.geometry && near(adjusted.geometry->rect.x, 24.0) && near(adjusted.geometry->rect.y, 42.0) &&
             near(adjusted.geometry->rect.width, 140.0) && near(adjusted.geometry->angleDegrees, 65.0),
         "ordered non-relative offsets adjust position size and angle before projection");
  expect(adjusted.geometry && near(adjusted.geometry->rgba[3], 191.5 / 255.0),
         "interpolated color preserves the reference alpha-offset early-return quirk");
}

void testSourceRegionStretchAndProjection() {
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 200.0, .height = 100.0}, {.x = 0.0, .y = 0.0, .width = 200.0, .height = 100.0}, {});
  AuthoredDestinationGeometry geometry;
  geometry.rect = {.x = 10.0, .y = 20.0, .width = 90.0, .height = 30.0};
  SkinSourceRegionGeometry source{.textureWidth = 400, .textureHeight = 300,
                                  .region = {.x = 100, .y = 50, .w = 60, .h = 40}};

  for (int id = 0; id <= 10; ++id) {
    geometry.stretch = static_cast<SkinStretchMode>(id);
    const auto projected = projectSkinDestinationToUi(geometry, source, viewport);
    expect(std::isfinite(projected.vertices[0][0]) && std::isfinite(projected.normalizedUvs[0][0]),
           "every supported stretch id produces finite geometry and UVs");
  }

  geometry.stretch = SkinStretchMode::KeepAspectRatioFitWidthTrimmed;
  const auto trimmed = projectSkinDestinationToUi(geometry, source, viewport);
  expect(near(trimmed.normalizedUvs[0][1], 60.0 / 300.0) && near(trimmed.normalizedUvs[2][1], 80.0 / 300.0),
         "trimmed cropping uses centered Java truncation inside a non-origin source region");
  geometry.stretch = SkinStretchMode::NoResize;
  const auto noResize = projectSkinDestinationToUi(geometry, source, viewport);
  expect(near(noResize.vertices[0][0], 25.0) && near(noResize.vertices[2][0], 85.0),
         "no-resize centers the source-sized destination without expansion");
}

} // namespace

int main() {
  testTimerConditionAndFrameSelection();
  testLoopRateAndIndependentMicrosecondTruncation();
  testConditionsAndOrderedOffsets();
  testSourceRegionStretchAndProjection();
  return failures == 0 ? 0 : 1;
}
