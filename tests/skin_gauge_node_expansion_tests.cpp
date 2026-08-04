#include "skin/beatoraja/SkinGaugeNodeExpansion.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
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

SkinSpriteFrames identifiableSprite(std::uint32_t resource, int frameCount,
                                    int sourceIndex) {
  SkinSpriteFrames result;
  result.resource = resource;
  result.cycleMillis = 100 + sourceIndex;
  result.timer = SkinTimerPropertyId{static_cast<std::uint32_t>(sourceIndex + 1)};
  result.frames.reserve(static_cast<std::size_t>(frameCount));
  for (int frame = 0; frame < frameCount; ++frame) {
    result.frames.push_back({.x = sourceIndex * 1'000 + frame,
                             .y = sourceIndex * 2'000 + frame,
                             .w = 10 + frame,
                             .h = 20 + frame,
                             .gridColumn = frame,
                             .gridRow = sourceIndex,
                             .gridColumns = frameCount,
                             .gridRows = frameCount + 1});
  }
  return result;
}

SkinGaugeNodeExpansionInput inputFor(std::size_t nodeCount,
                                     int framesPerNode = 2) {
  SkinGaugeNodeExpansionInput input;
  input.parts = 50;
  input.animationType = 0;
  input.animationRange = 3;
  input.animationCycleMillis = 33;
  input.resultStartMillis = 0;
  input.resultEndMillis = 500;
  input.nodes.reserve(nodeCount);
  input.images.reserve(nodeCount);
  for (std::size_t index = 0; index < nodeCount; ++index) {
    const std::string id = "node-" + std::to_string(index);
    input.nodes.push_back(id);
    input.images.push_back(
        {.id = id,
         .sprite = identifiableSprite(static_cast<std::uint32_t>(100 + index),
                                      framesPerNode, static_cast<int>(index))});
  }
  return input;
}

std::array<int, SkinGaugeNodeExpansionPolicy::roleCount>
expectedSourceIndices(std::size_t nodeCount) {
  std::array<int, SkinGaugeNodeExpansionPolicy::roleCount> result{};
  result.fill(-1);
  const auto assign = [&](int source, std::initializer_list<int> roles) {
    for (const int role : roles) {
      result[static_cast<std::size_t>(role)] = source;
    }
  };
  switch (nodeCount) {
  case 4:
    assign(0, {0, 4, 6, 10, 12, 16, 18, 22, 24, 28, 30, 34});
    assign(1, {1, 5, 7, 11, 13, 17, 19, 23, 25, 29, 31, 35});
    assign(2, {2, 8, 14, 20, 26, 32});
    assign(3, {3, 9, 15, 21, 27, 33});
    break;
  case 8:
    assign(0, {12, 16, 18, 22});
    assign(1, {13, 17, 19, 23});
    assign(2, {14, 20});
    assign(3, {15, 21});
    assign(4, {0, 4, 6, 10, 24, 28, 30, 34});
    assign(5, {1, 5, 7, 11, 25, 29, 31, 35});
    assign(6, {2, 8, 26, 32});
    assign(7, {3, 9, 27, 33});
    break;
  case 12:
    assign(0, {12, 18});
    assign(1, {13, 19});
    assign(2, {14, 20});
    assign(3, {15, 21});
    assign(4, {0, 6, 24, 30});
    assign(5, {1, 7, 25, 31});
    assign(6, {2, 8, 26, 32});
    assign(7, {3, 9, 27, 33});
    assign(8, {16, 22});
    assign(9, {17, 23});
    assign(10, {4, 10, 28, 34});
    assign(11, {5, 11, 29, 35});
    break;
  case 36:
    for (std::size_t index = 0; index < result.size(); ++index) {
      result[index] = static_cast<int>(index);
    }
    break;
  default:
    break;
  }
  return result;
}

void expectsPinnedRoleMapping(std::size_t nodeCount) {
  const auto result = expandSkinGaugeNodes(inputFor(nodeCount));
  expect(result.gauge.has_value(), "supported cardinality expands");
  expect(result.error == SkinGaugeNodeExpansionError::None,
         "supported cardinality has no error");
  if (!result.gauge) {
    return;
  }

  expect(result.gauge->orderedNodes.size() ==
             SkinGaugeNodeExpansionPolicy::roleCount,
         "every supported cardinality produces exactly 36 roles");
  const auto expected = expectedSourceIndices(nodeCount);
  for (std::size_t role = 0; role < expected.size(); ++role) {
    const int source = expected[role];
    const auto &actual = result.gauge->orderedNodes[role];
    expect(source >= 0, "every role is mapped");
    expect(actual.resource == static_cast<SkinResourceId>(100 + source),
           "role retains the independently identifiable source sprite");
    expect(actual.frames.size() == 2,
           "role retains every source animation frame");
    if (actual.frames.size() == 2) {
      expect(actual.frames[0].x == source * 1'000 &&
                 actual.frames[1].x == source * 1'000 + 1,
             "role retains its source frame identity");
    }
    expect(actual.cycleMillis == 0 && !actual.timer.has_value(),
           "pinned Gauge construction ignores referenced Image timer and cycle");
  }
}

void testPinnedMappingsAndOutputTimingPolicy() {
  expectsPinnedRoleMapping(4);
  expectsPinnedRoleMapping(8);
  expectsPinnedRoleMapping(12);
  expectsPinnedRoleMapping(36);
}

void testGaugeConstructionFields() {
  for (int type = 0; type <= 3; ++type) {
    auto input = inputFor(36);
    input.parts = 40 + type;
    input.animationType = type;
    input.animationRange = 10 + type;
    input.animationCycleMillis =
        type == static_cast<int>(SkinGaugeAnimationType::Flicker) ? 20 + type
                                                                  : 0;
    input.resultStartMillis = -10 - type;
    input.resultEndMillis = 700 + type;
    const auto result = expandSkinGaugeNodes(input);
    expect(result.gauge.has_value(), "each pinned animation type expands");
    if (!result.gauge) {
      continue;
    }
    expect(result.gauge->parts == input.parts,
           "parts normalize directly to the source-neutral gauge");
    expect(static_cast<int>(result.gauge->animation) == input.animationType,
           "animation type preserves pinned values 0 through 3");
    expect(result.gauge->animationRange == input.animationRange,
           "range normalizes directly to the source-neutral gauge");
    expect(result.gauge->animationCycleMillis == input.animationCycleMillis,
           "cycle normalizes directly to the source-neutral gauge");
    expect(result.gauge->resultStartMillis == input.resultStartMillis &&
               result.gauge->resultEndMillis == input.resultEndMillis,
           "result start and end normalize directly to the source-neutral gauge");
  }
}

void testUnsafeAnimationArithmeticFailsClosed() {
  const auto expectInvalid = [](SkinGaugeNodeExpansionInput input,
                                std::string_view message) {
    const auto result = expandSkinGaugeNodes(input);
    expect(!result.gauge &&
               result.error ==
                   SkinGaugeNodeExpansionError::InvalidAnimationParameters,
           message);
  };

  auto invalid = inputFor(4);
  invalid.parts = 0;
  expectInvalid(invalid, "zero gauge parts cannot reach division");
  invalid = inputFor(4);
  invalid.parts =
      static_cast<int>(SkinGaugeNodeExpansionPolicy::maxParts) + 1;
  expectInvalid(invalid, "gauge parts are bounded before command expansion");
  invalid = inputFor(4);
  invalid.animationRange = -1;
  expectInvalid(invalid, "negative animation range cannot reach modulo");
  invalid = inputFor(4);
  invalid.animationRange =
      static_cast<int>(SkinGaugeNodeExpansionPolicy::maxAnimationRange) + 1;
  expectInvalid(invalid, "animation range is bounded before evaluation");
  invalid = inputFor(4);
  invalid.animationCycleMillis = -1;
  expectInvalid(invalid, "negative animation cycle cannot reach evaluation");
  invalid = inputFor(4);
  invalid.animationCycleMillis =
      SkinGaugeNodeExpansionPolicy::maxAnimationCycleMillis + 1;
  expectInvalid(invalid, "animation cycle has a fixed upper bound");
  invalid = inputFor(4);
  invalid.animationType = static_cast<int>(SkinGaugeAnimationType::Flicker);
  invalid.animationCycleMillis = 3;
  expectInvalid(invalid,
                "flicker cycle keeps both alpha-ramp denominators positive");
  invalid = inputFor(4);
  invalid.resultStartMillis =
      SkinGaugeNodeExpansionPolicy::minResultTimeMillis - 1;
  expectInvalid(invalid, "result animation has a fixed signed lower bound");
  invalid = inputFor(4);
  invalid.resultStartMillis = invalid.resultEndMillis;
  expectInvalid(invalid, "result animation requires a nonzero interval");
  invalid = inputFor(4);
  invalid.resultEndMillis =
      SkinGaugeNodeExpansionPolicy::maxResultTimeMillis + 1;
  expectInvalid(invalid, "result animation has a fixed signed upper bound");
}

void testInvalidInputsFailClosed() {
  for (const std::size_t count : {std::size_t{0}, std::size_t{3},
                                  std::size_t{5}, std::size_t{37}}) {
    const auto result = expandSkinGaugeNodes(inputFor(count));
    expect(!result.gauge.has_value(), "unsupported node count has no gauge");
    expect(result.error == SkinGaugeNodeExpansionError::UnsupportedNodeCount,
           "unsupported node count reports its dedicated error");
  }

  auto missing = inputFor(4);
  missing.nodes[2] = "missing-image";
  const auto missingResult = expandSkinGaugeNodes(missing);
  expect(!missingResult.gauge.has_value(), "missing node image has no gauge");
  expect(missingResult.error == SkinGaugeNodeExpansionError::MissingNodeImage,
         "missing node image reports its dedicated error");

  auto duplicateDefinition = inputFor(4);
  duplicateDefinition.images.push_back(duplicateDefinition.images.front());
  const auto duplicateDefinitionResult =
      expandSkinGaugeNodes(duplicateDefinition);
  expect(!duplicateDefinitionResult.gauge.has_value(),
         "duplicate image definitions have no gauge");
  expect(duplicateDefinitionResult.error ==
             SkinGaugeNodeExpansionError::AmbiguousNodeImage,
         "duplicate image definitions never silently select the first match");

  auto repeatedNodeReference = inputFor(4);
  repeatedNodeReference.nodes.assign(4, repeatedNodeReference.nodes.front());
  const auto repeatedNodeResult = expandSkinGaugeNodes(repeatedNodeReference);
  expect(repeatedNodeResult.gauge.has_value(),
         "duplicate node references remain legal mapping reuse");
  if (repeatedNodeResult.gauge) {
    for (const auto &node : repeatedNodeResult.gauge->orderedNodes) {
      expect(node.resource == repeatedNodeReference.images.front().sprite.resource,
             "duplicate node reference maps every role to its shared image");
    }
  }

  auto zeroFrames = inputFor(4);
  zeroFrames.images[2].sprite.frames.clear();
  const auto zeroFramesResult = expandSkinGaugeNodes(zeroFrames);
  expect(!zeroFramesResult.gauge.has_value(),
         "resolved zero-frame image has no gauge");
  expect(zeroFramesResult.error ==
             SkinGaugeNodeExpansionError::EmptyAnimationFrames,
         "resolved zero-frame image reports its dedicated error");

  auto mismatchedFrames = inputFor(4);
  mismatchedFrames.images[3].sprite.frames.push_back(
      {.x = 9, .y = 9, .w = 9, .h = 9});
  const auto mismatchResult = expandSkinGaugeNodes(mismatchedFrames);
  expect(!mismatchResult.gauge.has_value(),
         "mismatched animation frame counts have no gauge");
  expect(mismatchResult.error ==
             SkinGaugeNodeExpansionError::UnequalAnimationFrameCount,
         "mismatched animation frame counts report their dedicated error");

  for (const int type : {-1, 4}) {
    auto invalidType = inputFor(4);
    invalidType.animationType = type;
    const auto result = expandSkinGaugeNodes(invalidType);
    expect(!result.gauge.has_value(), "unsupported animation type has no gauge");
    expect(result.error == SkinGaugeNodeExpansionError::InvalidAnimationType,
           "unsupported animation type reports its dedicated error");
  }
}

void testHardFrameBoundPreventsExpansionOverflow() {
  auto input = inputFor(4, 1);
  input.images.resize(1);
  input.nodes.assign(4, input.images.front().id);
  input.images.front().sprite.frames.assign(
      SkinGaugeNodeExpansionPolicy::maxExpandedSpriteFrames /
              SkinGaugeNodeExpansionPolicy::roleCount +
          1,
      SkinSourceRect{});

  const auto result = expandSkinGaugeNodes(input);
  expect(!result.gauge.has_value(), "over-bound frame input has no gauge");
  expect(result.error == SkinGaugeNodeExpansionError::FrameLimitExceeded,
         "over-bound frame input fails before expanded allocation");
}

} // namespace

int main() {
  testPinnedMappingsAndOutputTimingPolicy();
  testGaugeConstructionFields();
  testInvalidInputsFailClosed();
  testUnsafeAnimationArithmeticFailsClosed();
  testHardFrameBoundPreventsExpansionOverflow();
  return failures == 0 ? 0 : 1;
}
