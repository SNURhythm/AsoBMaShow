#include "skin/beatoraja/SkinNoteNormalization.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
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

SkinSpriteFrames identifiableSprite(std::uint32_t resource, int source) {
  return {.resource = resource,
          .frames = {{.x = source,
                      .y = source + 100,
                      .w = 10,
                      .h = 20,
                      .gridColumn = 0,
                      .gridRow = 0,
                      .gridColumns = 1,
                      .gridRows = 1}}};
}

SkinAuthoredNoteVisualSlots authoredSlots(std::uint32_t resourceBase,
                                          int sourceBase,
                                          std::size_t laneCount = 2) {
  SkinAuthoredNoteVisualSlots slots;
  slots.reserve(laneCount);
  for (std::size_t lane = 0; lane < laneCount; ++lane) {
    slots.push_back(identifiableSprite(resourceBase + lane,
                                       sourceBase + static_cast<int>(lane)));
  }
  return slots;
}

SkinNoteNormalizationInput completeLegacyInput() {
  SkinNoteNormalizationInput input;
  input.note = authoredSlots(100, 1'000);
  input.mine = authoredSlots(200, 2'000);
  input.lnEnd = authoredSlots(300, 3'000);
  input.lnStart = authoredSlots(400, 4'000);
  input.lnBody = authoredSlots(500, 5'000);
  input.lnActive = authoredSlots(600, 6'000);
  input.hcnEnd = authoredSlots(700, 7'000);
  input.hcnStart = authoredSlots(800, 8'000);
  input.hcnBody = authoredSlots(900, 9'000);
  input.hcnActive = authoredSlots(1'000, 10'000);
  input.hcnDamage = authoredSlots(1'100, 11'000);
  input.hcnReactive = authoredSlots(1'200, 12'000);
  return input;
}

const SkinNormalizedNoteVisual &visual(const SkinNormalizedNoteLane &lane,
                                       SkinNoteVisualKind kind) {
  return lane.visuals[static_cast<std::size_t>(kind)];
}

void expectSprite(const SkinNormalizedNoteVisual &actual,
                  std::uint32_t resource, int source,
                  std::string_view message) {
  const auto *sprite = std::get_if<SkinSpriteFrames>(&actual);
  expect(sprite != nullptr, message);
  if (sprite == nullptr) {
    return;
  }
  expect(sprite->resource == resource, message);
  expect(sprite->frames.size() == 1 && sprite->frames[0].x == source,
         message);
}

void expectFallback(const SkinNormalizedNoteVisual &actual,
                    SkinNoteFallbackColor color,
                    SkinNoteFallbackShape shape,
                    std::string_view message) {
  const auto *fallback = std::get_if<SkinSynthesizedNoteFallback>(&actual);
  expect(fallback != nullptr, message);
  if (fallback == nullptr) {
    return;
  }
  expect(fallback->color == color && fallback->shape == shape, message);
}

void testLegacyArraysTransposeByLane() {
  const auto result = normalizeSkinNote(completeLegacyInput());
  expect(result.note.has_value(), "complete legacy input normalizes");
  expect(result.error == SkinNoteNormalizationError::None,
         "complete legacy input has no error");
  if (!result.note) {
    return;
  }

  expect(result.note->lanes.size() == 2,
         "normalization preserves the two authored lanes");
  for (std::size_t lane = 0; lane < result.note->lanes.size(); ++lane) {
    const auto &normalizedLane = result.note->lanes[lane];
    expect(normalizedLane.authoredLane == lane,
           "transposed lane retains its authored index");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::Normal),
                 100 + lane, 1'000 + static_cast<int>(lane),
                 "normal visual remains in its lane");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::Mine), 200 + lane,
                 2'000 + static_cast<int>(lane),
                 "mine visual remains in its lane");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::LnEnd), 300 + lane,
                 3'000 + static_cast<int>(lane),
                 "LN end transposes into its lane");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::LnStart),
                 400 + lane, 4'000 + static_cast<int>(lane),
                 "LN start transposes into its lane");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::LnBodyActive),
                 500 + lane, 5'000 + static_cast<int>(lane),
                 "legacy LN body supplies active slot");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::LnBodyInactive),
                 600 + lane, 6'000 + static_cast<int>(lane),
                 "legacy LN active supplies inactive slot");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::HcnEnd),
                 700 + lane, 7'000 + static_cast<int>(lane),
                 "HCN end transposes into its lane");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::HcnStart),
                 800 + lane, 8'000 + static_cast<int>(lane),
                 "HCN start transposes into its lane");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::HcnBodyActive),
                 900 + lane, 9'000 + static_cast<int>(lane),
                 "legacy HCN body supplies active slot");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::HcnBodyInactive),
                 1'000 + lane, 10'000 + static_cast<int>(lane),
                 "legacy HCN active supplies inactive slot");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::HcnDamage),
                 1'100 + lane, 11'000 + static_cast<int>(lane),
                 "legacy HCN damage retains its visual kind");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::HcnReactive),
                 1'200 + lane, 12'000 + static_cast<int>(lane),
                 "legacy HCN reactive retains its visual kind");
  }
}

void testModernArraysTakePrecedenceIndependently() {
  auto input = completeLegacyInput();
  input.lnBodyActive = authoredSlots(1'300, 13'000);
  input.hcnBodyActive = authoredSlots(1'400, 14'000);
  input.hcnBodyReactive = authoredSlots(1'500, 15'000);
  input.hcnBodyMiss = authoredSlots(1'600, 16'000);

  const auto result = normalizeSkinNote(input);
  expect(result.note.has_value(), "modern input normalizes");
  if (!result.note) {
    return;
  }
  for (std::size_t lane = 0; lane < result.note->lanes.size(); ++lane) {
    const auto &normalizedLane = result.note->lanes[lane];
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::LnBodyActive),
                 1'300 + lane, 13'000 + static_cast<int>(lane),
                 "non-empty modern LN active takes precedence");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::LnBodyInactive),
                 500 + lane, 5'000 + static_cast<int>(lane),
                 "modern LN body supplies inactive slot");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::HcnBodyActive),
                 1'400 + lane, 14'000 + static_cast<int>(lane),
                 "non-empty modern HCN active takes precedence");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::HcnBodyInactive),
                 900 + lane, 9'000 + static_cast<int>(lane),
                 "modern HCN body supplies inactive slot");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::HcnDamage),
                 1'600 + lane, 16'000 + static_cast<int>(lane),
                 "modern HCN miss maps to damage visual kind");
    expectSprite(visual(normalizedLane, SkinNoteVisualKind::HcnReactive),
                 1'500 + lane, 15'000 + static_cast<int>(lane),
                 "modern HCN reactive maps to reactive visual kind");
  }
}

void testLnAndHcnCompatibilitySwitchesAreIndependent() {
  auto lnModern = completeLegacyInput();
  lnModern.lnBodyActive = authoredSlots(1'300, 13'000);
  const auto lnModernResult = normalizeSkinNote(lnModern);
  expect(lnModernResult.note.has_value(),
         "modern LN with legacy HCN normalizes");
  if (lnModernResult.note) {
    const auto &lane = lnModernResult.note->lanes.front();
    expectSprite(visual(lane, SkinNoteVisualKind::LnBodyActive), 1'300,
                 13'000, "modern LN switch selects modern active body");
    expectSprite(visual(lane, SkinNoteVisualKind::HcnDamage), 1'100, 11'000,
                 "modern LN switch leaves HCN on its legacy damage field");
  }

  auto hcnModern = completeLegacyInput();
  hcnModern.hcnBodyActive = authoredSlots(1'400, 14'000);
  hcnModern.hcnBodyReactive = authoredSlots(1'500, 15'000);
  hcnModern.hcnBodyMiss = authoredSlots(1'600, 16'000);
  const auto hcnModernResult = normalizeSkinNote(hcnModern);
  expect(hcnModernResult.note.has_value(),
         "legacy LN with modern HCN normalizes");
  if (hcnModernResult.note) {
    const auto &lane = hcnModernResult.note->lanes.front();
    expectSprite(visual(lane, SkinNoteVisualKind::LnBodyActive), 500, 5'000,
                 "modern HCN switch leaves LN on its legacy body field");
    expectSprite(visual(lane, SkinNoteVisualKind::HcnDamage), 1'600, 16'000,
                 "modern HCN switch selects miss damage field");
    expectSprite(visual(lane, SkinNoteVisualKind::HcnReactive), 1'500,
                 15'000, "modern HCN switch selects reactive field");
  }
}

void testEmptyModernArraysUseLegacyPrecedence() {
  auto input = completeLegacyInput();
  input.lnBodyActive = SkinAuthoredNoteVisualSlots{};
  input.hcnBodyActive = SkinAuthoredNoteVisualSlots{};
  input.hcnBodyReactive = authoredSlots(1'500, 15'000);
  input.hcnBodyMiss = authoredSlots(1'600, 16'000);

  const auto result = normalizeSkinNote(input);
  expect(result.note.has_value(), "empty modern arrays retain legacy path");
  if (!result.note) {
    return;
  }
  const auto &lane = result.note->lanes.front();
  expectSprite(visual(lane, SkinNoteVisualKind::LnBodyActive), 500, 5'000,
               "empty LN modern array does not override legacy body");
  expectSprite(visual(lane, SkinNoteVisualKind::LnBodyInactive), 600, 6'000,
               "empty LN modern array keeps legacy active field");
  expectSprite(visual(lane, SkinNoteVisualKind::HcnDamage), 1'100, 11'000,
               "empty HCN modern array does not override legacy damage");
  expectSprite(visual(lane, SkinNoteVisualKind::HcnReactive), 1'200, 12'000,
               "empty HCN modern array keeps legacy reactive field");
}

void testNullSlotsSynthesizePinnedFallbacks() {
  auto input = completeLegacyInput();
  input.note[0].reset();
  input.mine[0].reset();
  input.lnEnd[0].reset();
  input.hcnDamage[0].reset();

  const auto result = normalizeSkinNote(input);
  expect(result.note.has_value(), "null authored slots synthesize fallbacks");
  if (!result.note) {
    return;
  }
  const auto &lane = result.note->lanes.front();
  expectFallback(visual(lane, SkinNoteVisualKind::Normal),
                 SkinNoteFallbackColor::White, SkinNoteFallbackShape::Solid,
                 "null normal slot is white solid fallback");
  expectFallback(visual(lane, SkinNoteVisualKind::Mine),
                 SkinNoteFallbackColor::Red, SkinNoteFallbackShape::Solid,
                 "null mine slot is red solid fallback");
  expectFallback(visual(lane, SkinNoteVisualKind::LnEnd),
                 SkinNoteFallbackColor::Yellow, SkinNoteFallbackShape::Solid,
                 "null LN slot is yellow solid fallback");
  expectFallback(visual(lane, SkinNoteVisualKind::HcnDamage),
                 SkinNoteFallbackColor::Yellow, SkinNoteFallbackShape::Solid,
                 "null HCN slot is yellow solid fallback");
  expectFallback(visual(lane, SkinNoteVisualKind::Hidden),
                 SkinNoteFallbackColor::Orange,
                 SkinNoteFallbackShape::DoubleOutline,
                 "hidden visual is always orange double-outline fallback");
  expectFallback(visual(lane, SkinNoteVisualKind::Processed),
                 SkinNoteFallbackColor::Cyan,
                 SkinNoteFallbackShape::DoubleOutline,
                 "processed visual is always cyan double-outline fallback");
}

void testUnsafeCardinalityFailsClosed() {
  auto input = completeLegacyInput();
  input.hcnReactive.pop_back();
  const auto mismatch = normalizeSkinNote(input);
  expect(!mismatch.note.has_value(),
         "selected visual cardinality mismatch produces no partial lanes");
  expect(mismatch.error == SkinNoteNormalizationError::UnsafeCardinality,
         "selected visual cardinality mismatch has structured error");

  input = completeLegacyInput();
  input.note.resize(SkinNoteNormalizationPolicy::maxLanes + 1);
  const auto overBound = normalizeSkinNote(input);
  expect(!overBound.note.has_value(), "over-bound lane input produces no note");
  expect(overBound.error == SkinNoteNormalizationError::LaneLimitExceeded,
         "over-bound lane input has structured error");
}

} // namespace

int main() {
  testLegacyArraysTransposeByLane();
  testModernArraysTakePrecedenceIndependently();
  testLnAndHcnCompatibilitySwitchesAreIndependent();
  testEmptyModernArraysUseLegacyPrecedence();
  testNullSlotsSynthesizePinnedFallbacks();
  testUnsafeCardinalityFailsClosed();
  return failures == 0 ? 0 : 1;
}
