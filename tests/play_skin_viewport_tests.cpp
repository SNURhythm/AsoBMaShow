#include "skin/beatoraja/PlaySkinViewport.h"
#include "skin/beatoraja/SkinDestinationEvaluator.h"
#include "rendering/common.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <string_view>

// Test-owned definitions for the only rendering globals read by the inline
// normalizedToUi -> screenToUi conversion exercised below.
namespace rendering {
int render_width = 0;
int render_height = 0;
float ui_scale_x = 1.0F;
float ui_scale_y = 1.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
} // namespace rendering

namespace {

using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool near(double actual, double expected, double epsilon = 1e-9) {
  return std::abs(actual - expected) <= epsilon;
}

std::array<double, 2> apply(const Affine2D &affine, double x, double y) {
  return {affine.m00 * x + affine.m01 * y + affine.tx,
          affine.m10 * x + affine.m11 * y + affine.ty};
}

UiLogicalRect screenRectToUi(double left, double top, double right,
                             double bottom, double offsetX, double offsetY,
                             double scaleX, double scaleY) {
  const double uiLeft = (left - offsetX) / scaleX;
  const double uiTop = (top - offsetY) / scaleY;
  return {.x = uiLeft, .y = uiTop,
          .width = (right - offsetX) / scaleX - uiLeft,
          .height = (bottom - offsetY) / scaleY - uiTop};
}

void testFitUsesSafeAreaAndBars() {
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 1600.0, .height = 900.0},
      {.x = 20.0, .y = 30.0, .width = 1200.0, .height = 900.0}, {});
  expect(viewport.valid, "fit viewport is valid for positive authored and safe sizes");
  expect(near(viewport.authoredToUi.m00, 0.75), "fit uses the limiting horizontal scale");
  expect(near(viewport.authoredToUi.m11, -0.75), "fit flips authored bottom-left y");
  expect(near(viewport.authoredToUi.tx, 20.0), "fit centers horizontal content in the safe area");
  expect(near(viewport.authoredToUi.ty, 817.5), "fit centers vertical content in the safe area");
  expect(near(viewport.drawableAuthoredBounds.width, 1600.0), "fit inverse bounds retain authored width");
  expect(near(viewport.drawableAuthoredBounds.height, 1200.0), "fit inverse bounds include letterbox extent");
}

void testStretchAndCustomComposeOverSelectedBase() {
  ViewportSettings stretch;
  stretch.mode = ViewportMode::Stretch;
  const auto stretched = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 100.0}, {.x = 10.0, .y = 20.0, .width = 300.0, .height = 200.0}, stretch);
  expect(near(stretched.authoredToUi.m00, 3.0) && near(stretched.authoredToUi.m11, -2.0),
         "stretch independently fills safe width and height");

  ViewportSettings custom = stretch;
  custom.mode = ViewportMode::Custom;
  custom.customBase = CustomViewportBase::Stretch;
  custom.scaleX = 2.0F;
  custom.scaleY = 0.5F;
  custom.translateX = 7.0F;
  custom.translateY = -9.0F;
  const auto transformed = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 100.0}, {.x = 10.0, .y = 20.0, .width = 300.0, .height = 200.0}, custom);
  expect(near(transformed.authoredToUi.m00, 6.0) && near(transformed.authoredToUi.m11, -1.0),
         "custom scale composes over stretch rather than replacing it");
  expect(near(transformed.authoredToUi.tx, -133.0) && near(transformed.authoredToUi.ty, 161.0),
         "custom scaling stays centered then applies bounded UI translation");
}

void testCustomFitClampingAndLogicalScaleEquivalence() {
  ViewportSettings fitCustom;
  fitCustom.mode = ViewportMode::Custom;
  fitCustom.customBase = CustomViewportBase::Fit;
  fitCustom.scaleX = 2.0F;
  fitCustom.scaleY = 1.5F;
  fitCustom.translateX = 10.0F;
  fitCustom.translateY = -20.0F;
  const auto customFit = evaluatePlaySkinViewport(
      {.width = 1600.0, .height = 900.0},
      {.x = 20.0, .y = 30.0, .width = 1200.0, .height = 900.0}, fitCustom);
  expect(near(customFit.authoredToUi.m00, 1.5) && near(customFit.authoredToUi.m11, -1.125) &&
             near(customFit.authoredToUi.tx, -570.0) && near(customFit.authoredToUi.ty, 966.25),
         "custom-over-fit scales around the nonzero safe-area center then translates");

  ViewportSettings clamped;
  clamped.mode = ViewportMode::Custom;
  clamped.customBase = CustomViewportBase::Stretch;
  clamped.scaleX = 100.0F;
  clamped.scaleY = 0.01F;
  clamped.translateX = 9'000.0F;
  clamped.translateY = -9'000.0F;
  const auto bounded = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 100.0},
      {.x = 10.0, .y = 20.0, .width = 300.0, .height = 200.0}, clamped);
  expect(near(bounded.authoredToUi.m00, 30.0, 1e-4) && near(bounded.authoredToUi.m11, -0.2, 1e-4) &&
             near(bounded.authoredToUi.tx, 6852.0, 1e-4) && near(bounded.authoredToUi.ty, -8062.0, 1e-4),
         "custom scale and translation use the profile policy min/max bounds");

  const auto oneXSafe = screenRectToUi(120.0, 70.0, 520.0, 270.0,
                                       20.0, 10.0, 2.0, 2.0);
  const auto twoXSafe = screenRectToUi(240.0, 140.0, 1040.0, 540.0,
                                       40.0, 20.0, 4.0, 4.0);
  const auto oneX = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 50.0}, oneXSafe, {});
  const auto twoX = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 50.0}, twoXSafe, {});
  expect(near(oneXSafe.x, twoXSafe.x) && near(oneXSafe.y, twoXSafe.y) &&
             near(oneXSafe.width, twoXSafe.width) && near(oneXSafe.height, twoXSafe.height) &&
             oneX.authoredToUi.m00 == twoX.authoredToUi.m00 &&
             oneX.authoredToUi.ty == twoX.authoredToUi.ty,
         "1x and 2x drawable safe rectangles convert to identical UI-logical viewports");
}

void testNormalizedTouchUsesRenderingConversion() {
  rendering::render_width = 1'600;
  rendering::render_height = 1'200;
  rendering::ui_offset_x = 100;
  rendering::ui_offset_y = 50;
  rendering::ui_scale_x = 2.0F;
  rendering::ui_scale_y = 2.0F;
  float uiX = 0.0F;
  float uiY = 0.0F;
  rendering::normalizedToUi(0.5F, 0.25F, uiX, uiY);
  expect(near(uiX, 350.0) && near(uiY, 125.0),
         "normalized touch conversion uses drawable dimensions, UI offset, and scale");
}

void testInvalidSettingsBecomeFitAndInverseRoundTrips() {
  ViewportSettings invalid;
  invalid.mode = static_cast<ViewportMode>(99);
  invalid.customBase = static_cast<CustomViewportBase>(99);
  invalid.scaleX = -1.0F;
  invalid.scaleY = std::numeric_limits<float>::infinity();
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 200.0, .height = 100.0}, {.x = 0.0, .y = 0.0, .width = 400.0, .height = 400.0}, invalid);
  expect(viewport.valid, "invalid persisted viewport is defensively reset to fit");
  const std::array<std::array<double, 2>, 4> corners = {
      {{0.0, 0.0}, {200.0, 0.0}, {200.0, 100.0}, {0.0, 100.0}}};
  for (const auto point : corners) {
    const auto ui = apply(viewport.authoredToUi, point[0], point[1]);
    const auto authored = apply(viewport.uiToAuthored, ui[0], ui[1]);
    expect(near(authored[0], point[0]) && near(authored[1], point[1]),
           "viewport inverse round trips each authored corner and touch point");
  }

  const auto invalidBounds = evaluatePlaySkinViewport(
      {.width = 0.0, .height = 100.0}, {.x = 0.0, .y = 0.0, .width = 100.0, .height = 100.0}, {});
  expect(!invalidBounds.valid, "degenerate authored bounds deny inverse interaction");
  const auto invalidSafe = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 100.0}, {.x = 0.0, .y = 0.0, .width = 0.0, .height = 100.0}, {});
  expect(!invalidSafe.valid, "degenerate UI safe bounds deny inverse interaction");
}

void testProjectionUsesBottomLeftOrderAndClockwiseUiHandedness() {
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 100.0}, {.x = 0.0, .y = 0.0, .width = 100.0, .height = 100.0}, {});
  AuthoredDestinationGeometry geometry;
  geometry.rect = {.x = 10.0, .y = 20.0, .width = 30.0, .height = 40.0};
  geometry.centerX = 0.5;
  geometry.centerY = 0.5;
  geometry.angleDegrees = 90.0;
  geometry.clip = SkinAuthoredRect{.x = 10.0, .y = 20.0, .width = 30.0, .height = 40.0};
  const auto projected = projectSkinDestinationToUi(
      geometry, {.textureWidth = 100, .textureHeight = 100, .region = {.x = 10, .y = 20, .w = 30, .h = 40}}, viewport);
  expect(near(projected.vertices[0][0], 45.0) && near(projected.vertices[0][1], 75.0),
         "first vertex starts at rotated authored bottom-left");
  expect(near(projected.vertices[1][0], 45.0) && near(projected.vertices[1][1], 45.0),
         "positive authored CCW rotation becomes clockwise in top-left UI");
  expect(near(projected.vertices[2][0], 5.0) && near(projected.vertices[2][1], 45.0),
         "vertices preserve BL BR TR TL order through projection");
  expect(projected.clip.has_value() && near(projected.clip->x, 10.0) && near(projected.clip->y, 40.0),
         "unrotated authored clip is converted to top-left UI coordinates");
}

void testOffsetsPrecedeViewportProjection() {
  const auto viewport = evaluatePlaySkinViewport(
      {.width = 100.0, .height = 100.0}, {.x = 0.0, .y = 0.0, .width = 200.0, .height = 200.0}, {});
  AuthoredDestinationGeometry geometry;
  geometry.rect = {.x = 10.0, .y = 0.0, .width = 10.0, .height = 10.0};
  const auto before = projectSkinDestinationToUi(
      geometry, {.textureWidth = 10, .textureHeight = 10, .region = {.w = 10, .h = 10}}, viewport);
  geometry.rect.x += 10.0;
  const auto after = projectSkinDestinationToUi(
      geometry, {.textureWidth = 10, .textureHeight = 10, .region = {.w = 10, .h = 10}}, viewport);
  expect(near(after.vertices[0][0] - before.vertices[0][0], 20.0),
         "authored offsets scale before the viewport");
}

} // namespace

int main() {
  testFitUsesSafeAreaAndBars();
  testStretchAndCustomComposeOverSelectedBase();
  testCustomFitClampingAndLogicalScaleEquivalence();
  testNormalizedTouchUsesRenderingConversion();
  testInvalidSettingsBecomeFitAndInverseRoundTrips();
  testProjectionUsesBottomLeftOrderAndClockwiseUiHandedness();
  testOffsetsPrecedeViewportProjection();
  return failures == 0 ? 0 : 1;
}
