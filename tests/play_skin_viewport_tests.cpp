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

void testFocusedPlayAreaUsesSafeAreaAndSharedInverse() {
  const AuthoredSize canvas{1920.0, 1080.0};
  const UiLogicalRect safe{20.0, 40.0, 360.0, 760.0};
  const AuthoredRect lanes{100.0, 80.0, 400.0, 800.0};
  ViewportSettings settings;
  settings.centerPlayArea = true;
  const auto viewport = evaluatePlaySkinViewport(canvas, safe, settings, lanes);
  const auto center = apply(viewport.authoredToUi, 300.0, 480.0);
  expect(viewport.valid && near(center[0], 200.0) && near(center[1], 420.0),
         "off-center lanes are centered in the portrait safe area");
  expect(near(viewport.authoredToUi.m00, 0.9) &&
             near(viewport.authoredToUi.m11, -0.9),
         "play area fits uniformly without stretching lanes");
  const auto touch = apply(viewport.uiToAuthored, center[0], center[1]);
  expect(near(touch[0], 300.0) && near(touch[1], 480.0),
         "focused drawing and touch mapping share an inverse");
  settings.playAreaZoom = 1.5F;
  const auto zoomed = evaluatePlaySkinViewport(canvas, safe, settings, lanes);
  const auto zoomCenter = apply(zoomed.authoredToUi, 300.0, 480.0);
  expect(near(zoomed.authoredToUi.m00, 1.35) &&
             near(zoomCenter[0], 200.0) &&
             near(apply(zoomed.authoredToUi, 300.0, 80.0)[1], 800.0),
         "oversized play area keeps its judgment line at the safe bottom");
  const auto zoomTouch = apply(zoomed.uiToAuthored, 200.0, 800.0);
  expect(near(zoomTouch[0], 300.0) && near(zoomTouch[1], 80.0),
         "anchored judgment line preserves inverse touch mapping");
  settings.playAreaBottomPaddingPercent = 10.0F;
  const auto padded = evaluatePlaySkinViewport(canvas, safe, settings, lanes);
  expect(near(apply(padded.authoredToUi, 300.0, 80.0)[1], 724.0),
         "bottom padding raises the zoomed judgment-line anchor");
  settings.playAreaBottomPaddingPercent = 0.0F;
  settings.playAreaZoom = std::numeric_limits<float>::quiet_NaN();
  const auto invalidZoom = evaluatePlaySkinViewport(canvas, safe, settings, lanes);
  expect(invalidZoom.valid && near(invalidZoom.authoredToUi.m00, 0.9),
         "invalid zoom falls back to a usable centered viewport");
  settings.playAreaZoom = 100.0F;
  const auto limitedZoom = evaluatePlaySkinViewport(canvas, safe, settings, lanes);
  expect(near(limitedZoom.authoredToUi.m00, 2.7),
         "zoom is bounded before drawing and touch projection");
  const auto fallback = evaluatePlaySkinViewport(canvas, safe, settings);
  const auto normal = evaluatePlaySkinViewport(canvas, safe, {});
  expect(near(fallback.authoredToUi.m00, normal.authoredToUi.m00) &&
             near(fallback.authoredToUi.tx, normal.authoredToUi.tx),
         "skins without usable lane geometry retain normal framing");
  const auto invalid = evaluatePlaySkinViewport(
      canvas, safe, settings, AuthoredRect{0.0, 0.0, 0.0, 100.0});
  expect(near(invalid.authoredToUi.m00, normal.authoredToUi.m00),
         "zero-sized lane geometry cannot break the viewport");
}

void testVisibleScrollUsesClippedPostLiftLane() {
  ValidatedBeatorajaSkinModel model;
  SkinNoteObject note;
  note.lanes = {{.authoredLane = 0,
                 .laneDestination = {.x = 100.0, .y = 20.0, .width = 200.0, .height = 500.0}}};
  model.model.objects.push_back({.id = 1, .payload = note});
  const auto area = playSkinAuthoredPlayArea(model);
  ViewportSettings settings;
  settings.centerPlayArea = true;
  settings.playAreaZoom = 2.0F;
  auto viewport = evaluatePlaySkinViewport({1280.0, 720.0}, {0.0, 0.0, 1280.0, 720.0}, settings, area);
  const auto cropped = playSkinVisibleScroll(model, viewport);
  expect(cropped && near(cropped->height, 250.0) && near(cropped->scale, 0.5) &&
             near(cropped->topCrop, 250.0), "crop metrics use the visible primary lane");
  const auto lifted = playSkinVisibleScroll(model, viewport, 0.2);
  expect(lifted && near(lifted->height, 150.0) && near(lifted->authoredHeight, 400.0) &&
             near(lifted->scale, 0.375), "Lift is removed before crop compensation");
  settings.playAreaZoom = 1.0F;
  viewport = evaluatePlaySkinViewport({1280.0, 720.0}, {0.0, 0.0, 1280.0, 720.0}, settings, area);
  expect(!playSkinVisibleScroll(model, viewport), "uncropped lanes preserve exact authored behavior");
  viewport.drawableAuthoredBounds.y = 100.0;
  viewport.drawableAuthoredBounds.height = 300.0;
  expect(!playSkinVisibleScroll(model, viewport),
         "offscreen judgment lines preserve the authored projection window");
}

void testPlayAreaBoundsFollowSelectedNoteSource() {
  ValidatedBeatorajaSkinModel model;
  SkinNoteObject notes;
  notes.lanes = {{.authoredLane = 0,
                  .laneDestination = {100.0, 20.0, 80.0, 500.0}},
                 {.authoredLane = 1,
                  .laneDestination = {200.0, 40.0, 100.0, 480.0}},
                 {.authoredLane = 7,
                  .laneDestination = {900.0, 20.0, 0.0, 500.0}}};
  model.model.objects.push_back({.id = 1, .payload = notes});
  const auto bounds = playSkinAuthoredPlayArea(model);
  expect(bounds && near(bounds->x, 100.0) && near(bounds->y, 20.0) &&
             near(bounds->width, 200.0) && near(bounds->height, 500.0),
         "play area unions usable lanes and ignores zero-width scratch");
  model.model.objects.push_back({.id = 2, .payload = SkinNoteObject{}});
  expect(!playSkinAuthoredPlayArea(model),
         "last enabled Note owns the lane layout even when empty");
  model.disabledOptionalObjects.push_back(2);
  expect(playSkinAuthoredPlayArea(model).has_value(),
         "disabled optional Note cannot replace the active lane layout");
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
  testFocusedPlayAreaUsesSafeAreaAndSharedInverse();
  testVisibleScrollUsesClippedPostLiftLane();
  testPlayAreaBoundsFollowSelectedNoteSource();
  testStretchAndCustomComposeOverSelectedBase();
  testCustomFitClampingAndLogicalScaleEquivalence();
  testNormalizedTouchUsesRenderingConversion();
  testInvalidSettingsBecomeFitAndInverseRoundTrips();
  testProjectionUsesBottomLeftOrderAndClockwiseUiHandedness();
  testOffsetsPrecedeViewportProjection();
  return failures == 0 ? 0 : 1;
}
