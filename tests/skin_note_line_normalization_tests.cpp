#include "skin/beatoraja/SkinNoteLineNormalization.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
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

SkinSpriteFrames sprite(std::uint32_t resource, int marker, int cycle = 0) {
  return {.resource = resource,
          .frames = {{.x = marker,
                      .y = marker + 100,
                      .w = 10,
                      .h = 20,
                      .gridColumn = 0,
                      .gridRow = 0,
                      .gridColumns = 1,
                      .gridRows = 1}},
          .cycleMillis = cycle,
          .timer = SkinTimerPropertyId{resource + 10}};
}

SkinDestinationBody destination(int marker) {
  return {.timer = SkinTimerPropertyId{static_cast<std::uint32_t>(marker + 20)},
          .loop = marker + 30,
          .conditions = {marker + 40,
                         SkinBooleanPropertyId{
                             static_cast<std::uint32_t>(marker + 41)}},
          .offsetIds = {marker + 50},
          .drawCondition = SkinBooleanPropertyId{
              static_cast<std::uint32_t>(marker + 51)},
          .center = marker + 52,
          .mouseRect = SkinAuthoredRect{.x = marker + 0.25,
                                        .y = marker + 0.5,
                                        .width = marker + 0.75,
                                        .height = marker + 1.0},
          .frames = {{.timeMillis = marker,
                      .x = marker + 1.0,
                      .y = marker + 2.0,
                      .width = marker + 3.0,
                      .height = marker + 4.0,
                      .angleDegrees = marker + 5.0},
                     {.timeMillis = marker + 1,
                      .x = marker + 6.0,
                      .y = marker + 7.0,
                      .width = marker + 8.0,
                      .height = marker + 9.0,
                      .angleDegrees = marker + 10.0}},
          .authoredOrdinal = static_cast<std::uint32_t>(marker)};
}

SkinAuthoredNoteLineSlot slot(std::uint32_t resource, int marker) {
  return {.image = sprite(resource, marker, marker + 60),
          .destination = destination(marker)};
}

SkinNoteLineNormalizationInput completeInput() {
  SkinNoteLineNormalizationInput input;
  input.group = {slot(100, 10), slot(101, 20), slot(102, 30)};
  input.bpm = {slot(200, 40), slot(201, 50)};
  input.stop = {slot(300, 60), slot(301, 70), slot(302, 80),
                slot(303, 90)};
  input.time = {slot(400, 100), slot(401, 110), slot(402, 120)};
  return input;
}

const SkinNormalizedNoteLine *findLine(
    const SkinNormalizedNoteLines &lines, SkinNoteLineKind kind,
    std::size_t laneGroup) {
  for (const auto &line : lines.lines) {
    if (line.kind == kind && line.laneGroup == laneGroup) {
      return &line;
    }
  }
  return nullptr;
}

void testGroupOrderAndAuxiliaryPrefixesRemainIndependent() {
  const auto result = normalizeSkinNoteLines(completeInput());
  expect(result.lines.has_value(), "complete line input normalizes");
  expect(result.error == SkinNoteLineNormalizationError::None,
         "complete line input has no error");
  if (!result.lines) {
    return;
  }

  const auto &lines = *result.lines;
  expect(lines.groups.size() == 3, "group count defines three lane groups");
  expect(lines.lines.size() == 12,
         "group plus group-sized auxiliary arrays retain every indexed slot");
  constexpr SkinNoteLineKind expectedKinds[] = {
      SkinNoteLineKind::Group, SkinNoteLineKind::Bpm, SkinNoteLineKind::Stop,
      SkinNoteLineKind::Time};
  for (std::size_t kind = 0; kind < std::size(expectedKinds); ++kind) {
    for (std::size_t group = 0; group < lines.groups.size(); ++group) {
      const auto &line = lines.lines[kind * lines.groups.size() + group];
      expect(line.kind == expectedKinds[kind] && line.laneGroup == group,
             "line output retains loader kind and authored group order");
    }
  }
  for (std::size_t group = 0; group < lines.groups.size(); ++group) {
    expect(lines.groups[group].laneGroup == group,
           "group order retains its authored index");
    expect(lines.groups[group].laneRect.x == 11.0 + group * 10.0,
           "group lane rectangle comes from its own first destination frame");
    const auto *groupLine = findLine(lines, SkinNoteLineKind::Group, group);
    expect(groupLine != nullptr && groupLine->image && groupLine->destination,
           "every group retains independently resolved image and destination");
  }

  const auto *missingBpm = findLine(lines, SkinNoteLineKind::Bpm, 2);
  expect(missingBpm != nullptr && !missingBpm->image &&
             !missingBpm->destination,
         "short BPM input leaves a group-indexed suffix hole");
  expect(findLine(lines, SkinNoteLineKind::Stop, 0) != nullptr &&
             findLine(lines, SkinNoteLineKind::Stop, 1) != nullptr &&
             findLine(lines, SkinNoteLineKind::Stop, 2) != nullptr,
         "long stop input is bounded by the group prefix");
  expect(findLine(lines, SkinNoteLineKind::Time, 2) != nullptr,
         "time input retains the final matching group index");
}

void testNullAndUnresolvedAuxiliaryEntriesKeepExactIndexedHoles() {
  auto input = completeInput();
  input.group[2]->image.reset();
  input.bpm[0] = std::nullopt;
  input.bpm[1]->image.reset();
  input.stop[0]->destination.reset();
  input.time[2]->image.reset();

  const auto result = normalizeSkinNoteLines(input);
  expect(result.lines.has_value(), "sparse auxiliary input normalizes");
  if (!result.lines) {
    return;
  }
  const auto &lines = *result.lines;
  const auto *unresolvedGroup = findLine(lines, SkinNoteLineKind::Group, 2);
  expect(unresolvedGroup != nullptr && !unresolvedGroup->image &&
             unresolvedGroup->destination.has_value(),
         "unresolved group image retains its independently decoded destination");
  const auto *nullBpm = findLine(lines, SkinNoteLineKind::Bpm, 0);
  expect(nullBpm != nullptr && !nullBpm->image && !nullBpm->destination,
         "null BPM entry remains an exact indexed hole");
  const auto *unresolvedBpm = findLine(lines, SkinNoteLineKind::Bpm, 1);
  expect(unresolvedBpm != nullptr && !unresolvedBpm->image &&
             unresolvedBpm->destination.has_value(),
         "unresolved BPM image retains its independently decoded destination");
  const auto *missingBpm = findLine(lines, SkinNoteLineKind::Bpm, 2);
  expect(missingBpm != nullptr && !missingBpm->image &&
             !missingBpm->destination,
         "missing BPM suffix remains an exact indexed hole");
  const auto *destinationHole = findLine(lines, SkinNoteLineKind::Stop, 0);
  expect(destinationHole != nullptr && destinationHole->image.has_value() &&
             !destinationHole->destination,
         "missing stop destination retains its independently decoded image");
  const auto *unresolvedTime = findLine(lines, SkinNoteLineKind::Time, 2);
  expect(unresolvedTime != nullptr && !unresolvedTime->image &&
             unresolvedTime->destination.has_value(),
         "unresolved time image remains at its authored group index");
}

void testEmptyAuxiliaryArraysProduceGroupSizedHoleSets() {
  auto input = completeInput();
  input.bpm.clear();
  input.stop.clear();
  input.time.clear();

  const auto result = normalizeSkinNoteLines(input);
  expect(result.lines.has_value(), "empty auxiliary arrays normalize");
  if (!result.lines) {
    return;
  }
  expect(result.lines->lines.size() == 12,
         "empty auxiliary arrays still allocate every group-indexed line slot");
  for (const auto kind : {SkinNoteLineKind::Bpm, SkinNoteLineKind::Stop,
                          SkinNoteLineKind::Time}) {
    for (std::size_t group = 0; group < 3; ++group) {
      const auto *hole = findLine(*result.lines, kind, group);
      expect(hole != nullptr && !hole->image && !hole->destination,
             "empty auxiliary array retains a null indexed hole");
    }
  }
}

void testNestedSpriteAndDestinationTimingBindingsAreCopied() {
  auto input = completeInput();
  input.time[1]->image->cycleMillis = 777;
  input.time[1]->image->timer = SkinTimerPropertyId{888};
  input.time[1]->destination->timer = SkinTimerPropertyId{999};
  input.time[1]->destination->frames[1].timeMillis = 1'111;
  input.time[1]->destination->conditions = {
      SkinBooleanPropertyId{1'112}, 1'113};

  const auto result = normalizeSkinNoteLines(input);
  expect(result.lines.has_value(), "timed nested line input normalizes");
  if (!result.lines) {
    return;
  }
  const auto *line = findLine(*result.lines, SkinNoteLineKind::Time, 1);
  expect(line != nullptr && line->image && line->destination,
         "timed line has both independently decoded children");
  if (line == nullptr || !line->image || !line->destination) {
    return;
  }
  expect(line->image->cycleMillis == 777 &&
             line->image->timer == SkinTimerPropertyId{888},
         "nested image timing is retained");
  expect(line->destination->timer == SkinTimerPropertyId{999} &&
             line->destination->frames[1].timeMillis == 1'111 &&
             line->destination->conditions.size() == 2,
         "nested destination frames and bindings are retained");
}

void testMissingFirstGroupDestinationFailsClosed() {
  auto input = completeInput();
  input.group[1]->destination->frames.clear();

  const auto result = normalizeSkinNoteLines(input);
  expect(!result.lines.has_value(),
         "group without a first destination frame produces no partial output");
  expect(result.error == SkinNoteLineNormalizationError::MissingGroupLaneRect,
         "missing group first destination frame has a structured error");
}

void testUnsafeInputBudgetsAndGeometryFailClosed() {
  auto overGroups = completeInput();
  overGroups.group.resize(SkinNoteLineNormalizationPolicy::maxGroups + 1);
  const auto groupResult = normalizeSkinNoteLines(overGroups);
  expect(!groupResult.lines.has_value() &&
             groupResult.error == SkinNoteLineNormalizationError::GroupLimitExceeded,
         "over-bound groups fail closed before output allocation");

  auto overFrames = completeInput();
  overFrames.group[0]->image->frames.assign(
      SkinNoteLineNormalizationPolicy::maxFramesPerSprite + 1, {});
  const auto frameResult = normalizeSkinNoteLines(overFrames);
  expect(!frameResult.lines.has_value() &&
             frameResult.error == SkinNoteLineNormalizationError::FrameLimitExceeded,
         "over-bound sprite frames fail closed");

  auto overCumulative = completeInput();
  overCumulative.group.assign(49, slot(500, 130));
  for (auto &group : overCumulative.group) {
    const auto imageFrame = group->image->frames.front();
    group->image->frames.assign(
        SkinNoteLineNormalizationPolicy::maxFramesPerSprite, imageFrame);
    const auto destinationFrame = group->destination->frames.front();
    group->destination->frames.assign(
        SkinNoteLineNormalizationPolicy::maxFramesPerDestination,
        destinationFrame);
  }
  const auto cumulativeResult = normalizeSkinNoteLines(overCumulative);
  expect(!cumulativeResult.lines.has_value() &&
             cumulativeResult.error ==
                 SkinNoteLineNormalizationError::FrameLimitExceeded,
         "cumulative materialized sprite frame budget fails closed");

  auto oversizedBindings = completeInput();
  oversizedBindings.group[0]->destination->conditions.assign(
      SkinNoteLineNormalizationPolicy::maxDestinationConditions + 1, 7);
  const auto bindingResult = normalizeSkinNoteLines(oversizedBindings);
  expect(!bindingResult.lines.has_value() &&
             bindingResult.error ==
                 SkinNoteLineNormalizationError::UnsafeCardinality,
         "over-bound destination bindings fail closed");

  auto nonfinite = completeInput();
  nonfinite.stop[1]->destination->frames[0].width =
      std::numeric_limits<double>::infinity();
  const auto geometryResult = normalizeSkinNoteLines(nonfinite);
  expect(!geometryResult.lines.has_value() &&
             geometryResult.error == SkinNoteLineNormalizationError::NonFiniteGeometry,
         "non-finite nested destination geometry fails closed");
}

} // namespace

int main() {
  testGroupOrderAndAuxiliaryPrefixesRemainIndependent();
  testNullAndUnresolvedAuxiliaryEntriesKeepExactIndexedHoles();
  testEmptyAuxiliaryArraysProduceGroupSizedHoleSets();
  testNestedSpriteAndDestinationTimingBindingsAreCopied();
  testMissingFirstGroupDestinationFailsClosed();
  testUnsafeInputBudgetsAndGeometryFailClosed();
  return failures == 0 ? 0 : 1;
}
