#include "PlayfieldProjection.h"

#include "../../ReplayGhostUtils.h"

#include "GameplayChartEntityRenderBudget.h"
#include "GameplayNoteSubmissionOrder.h"
#include "GameplayScrollGeometry.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace {

ChartVisualNoteSource effectiveSource(const ChartVisualNote &note) {
  // Fixtures and persisted models created before source was introduced retain
  // their old kind-only encoding. New chart models always carry source.
  if (note.source == ChartVisualNoteSource::Playable) {
    if (note.kind == ChartVisualNoteKind::Invisible) {
      return ChartVisualNoteSource::Invisible;
    }
    if (note.kind == ChartVisualNoteKind::Mine) {
      return ChartVisualNoteSource::Mine;
    }
  }
  return note.source;
}

bool isVisible(double scrollDelta, const PlayfieldProjectionRequest &request) {
  if (request.visibleScrollBefore == 0.0 && request.visibleScrollAfter == 0.0) {
    return true;
  }
  return scrollDelta >= -request.visibleScrollBefore &&
         scrollDelta <= request.visibleScrollAfter;
}

bool isLongIntervalVisible(double headScrollDelta, double tailScrollDelta,
                           const PlayfieldProjectionRequest &request) {
  if (request.visibleScrollBefore == 0.0 && request.visibleScrollAfter == 0.0) {
    return true;
  }
  const double minimum = -request.visibleScrollBefore;
  const double maximum = request.visibleScrollAfter;
  return std::max(headScrollDelta, tailScrollDelta) >= minimum &&
         std::min(headScrollDelta, tailScrollDelta) <= maximum;
}

bool isWithinLatePoorWindow(
    long long timelineMicros, long long visualTimeMicros,
    const PlayfieldProjectionRequest &request) noexcept {
  const auto latePoorTiming = std::max<std::int64_t>(
      0, request.latePoorTimingMicros);
  return timelineMicros >= visualTimeMicros - latePoorTiming;
}

skin::SkinProjectedNoteKind toSkinNoteKind(ChartVisualNoteSource source) {
  switch (source) {
  case ChartVisualNoteSource::Invisible:
    return skin::SkinProjectedNoteKind::Invisible;
  case ChartVisualNoteSource::Mine:
    return skin::SkinProjectedNoteKind::Mine;
  case ChartVisualNoteSource::Playable:
    return skin::SkinProjectedNoteKind::Normal;
  }
  return skin::SkinProjectedNoteKind::Normal;
}

skin::SkinProjectedLongNoteMode toSkinLongNoteMode(ChartLongNoteMode mode) {
  switch (mode) {
  case ChartLongNoteMode::CN:
    return skin::SkinProjectedLongNoteMode::CN;
  case ChartLongNoteMode::HCN:
    return skin::SkinProjectedLongNoteMode::HCN;
  case ChartLongNoteMode::LN:
    return skin::SkinProjectedLongNoteMode::LN;
  }
  return skin::SkinProjectedLongNoteMode::LN;
}

skin::SkinProjectedLineKind toSkinLineKind(ProjectedLineKind kind) {
  switch (kind) {
  case ProjectedLineKind::Section:
    return skin::SkinProjectedLineKind::Group;
  case ProjectedLineKind::BpmChange:
    return skin::SkinProjectedLineKind::Bpm;
  case ProjectedLineKind::Stop:
    return skin::SkinProjectedLineKind::Stop;
  case ProjectedLineKind::Time:
    return skin::SkinProjectedLineKind::Time;
  }
  return skin::SkinProjectedLineKind::Time;
}

} // namespace

double scrollPositionAtTime(const PlayfieldChartVisualModel &model,
                            long long timeMicros) {
  std::vector<const ChartVisualTimeline *> retained;
  retained.reserve(model.timelines.size());
  for (const auto &timeline : model.timelines) {
    if (timeline.retainedForProjection) {
      retained.push_back(&timeline);
    }
  }
  std::stable_sort(
      retained.begin(), retained.end(),
      [](const auto *left, const auto *right) {
        const auto leftOrdinal =
            left->retainedOrdinal == kNoRetainedTimelineOrdinal
                ? left->authoredOrdinal
                : left->retainedOrdinal;
        const auto rightOrdinal =
            right->retainedOrdinal == kNoRetainedTimelineOrdinal
                ? right->authoredOrdinal
                : right->retainedOrdinal;
        return std::tie(leftOrdinal, left->authoredOrdinal, left->id) <
               std::tie(rightOrdinal, right->authoredOrdinal, right->id);
      });
  std::vector<gameplay_scroll_geometry::ScrollPositionTimeline> values;
  values.reserve(retained.size() + 1);
  for (const auto *timeline : retained) {
    values.push_back({.timeMicros = timeline->timeMicros,
                      .scrollPosition = timeline->scrollPosition,
                      .stopMicros = timeline->stopMicros,
                      .bpm = timeline->bpm,
                      .scrollRate = timeline->scrollRate});
  }
  if (model.terminalScrollAnchor.has_value() &&
      (values.empty() || values.back().timeMicros <
                             model.terminalScrollAnchor->timeMicros)) {
    values.push_back(*model.terminalScrollAnchor);
  }
  return gameplay_scroll_geometry::scrollPositionAtTime(values, timeMicros);
}

PlayfieldProjectionResult
PlayfieldProjection::project(const PlayfieldChartVisualModel &model,
                             const PlayfieldVisualState &state,
                             const PlayfieldProjectionRequest &request) {
  PlayfieldProjectionResult result;
  result.frameSerial = state.clock.serial;
  result.builtInTraversal = request.builtInTraversal;
  const long long timeMicros = state.clock.visualTimeMicros;
  result.currentScrollPosition = scrollPositionAtTime(model, timeMicros);

  std::unordered_map<ChartVisualId, const ChartVisualTimeline *> timelines;
  timelines.reserve(model.timelines.size());
  for (const auto &timeline : model.timelines) {
    timelines.emplace(timeline.id, &timeline);
  }
  std::unordered_map<ChartVisualId, const ChartVisualNote *> notes;
  notes.reserve(model.notes.size());
  for (const auto &note : model.notes) {
    notes.emplace(note.id, &note);
  }
  std::unordered_map<ChartVisualId, const NotePresentationState *> noteStates;
  noteStates.reserve(state.notes.size());
  for (const auto &noteState : state.notes) {
    noteStates.emplace(noteState.id, &noteState);
  }
  const auto stateFor =
      [&noteStates](ChartVisualId id) -> const NotePresentationState * {
    const auto it = noteStates.find(id);
    return it == noteStates.end() ? nullptr : it->second;
  };

  gameplay_chart_entity_render_budget::Budget budget;
  const auto reserve = [&result, &budget](std::uint32_t cost) {
    if (!budget.tryConsume(cost)) {
      result.budgetExceeded = true;
      return false;
    }
    return true;
  };

  std::vector<const ChartVisualTimeline *> orderedTimelines;
  orderedTimelines.reserve(model.timelines.size());
  for (const auto &timeline : model.timelines) {
    orderedTimelines.push_back(&timeline);
  }
  std::stable_sort(orderedTimelines.begin(), orderedTimelines.end(),
                   [](const auto *left, const auto *right) {
                     return std::tie(left->authoredOrdinal, left->id) <
                            std::tie(right->authoredOrdinal, right->id);
                   });

  std::uint32_t lineOrdinal = 1;
  const auto appendLine = [&result, &reserve, &lineOrdinal](
                              const ChartVisualTimeline &timeline,
                              ProjectedLineKind kind, double scrollDelta) {
    if (!reserve(
            gameplay_chart_entity_render_budget::kSingleRectangleEntityCost)) {
      return;
    }
    result.lines.push_back({.timelineId = timeline.id,
                            .kind = kind,
                            .scrollDelta = scrollDelta,
                            .timeMicros = timeline.timeMicros,
                            .authoredOrdinal = timeline.authoredOrdinal,
                            .retainedOrdinal = timeline.retainedOrdinal,
                            .submissionOrdinal = lineOrdinal++});
  };

  const ChartVisualTimeline *previousTimeline = nullptr;
  for (const auto *timeline : orderedTimelines) {
    const double scrollDelta =
        timeline->scrollPosition - result.currentScrollPosition;
    if (timeline->retainedForProjection && isVisible(scrollDelta, request)) {
      if (request.maxTimelines != 0 &&
          result.timelines.size() >= request.maxTimelines) {
        result.budgetExceeded = true;
      } else {
        result.timelines.push_back(
            {.timelineId = timeline->id,
             .scrollDelta = scrollDelta,
             .timeMicros = timeline->timeMicros,
             .authoredOrdinal = timeline->authoredOrdinal,
             .retainedOrdinal = timeline->retainedOrdinal,
             .submissionOrdinal = timeline->authoredOrdinal});
        if (timeline->sectionLine) {
          appendLine(*timeline, ProjectedLineKind::Section, scrollDelta);
        }
        if (previousTimeline != nullptr &&
            timeline->bpm != previousTimeline->bpm) {
          appendLine(*timeline, ProjectedLineKind::BpmChange, scrollDelta);
        }
        if (timeline->stopMicros > 0) {
          appendLine(*timeline, ProjectedLineKind::Stop, scrollDelta);
        }
      }
    }
    previousTimeline = timeline;
  }

  std::vector<const ChartVisualNote *> orderedNotes;
  orderedNotes.reserve(model.notes.size());
  for (const auto &note : model.notes) {
    orderedNotes.push_back(&note);
  }
  std::stable_sort(orderedNotes.begin(), orderedNotes.end(),
                   [](const auto *left, const auto *right) {
                     return std::tie(left->authoredOrdinal, left->id) <
                            std::tie(right->authoredOrdinal, right->id);
                   });

  struct BuiltInRowDepths {
    std::optional<gameplay_note_submission_order::LongNoteOrder> longOrder;
    std::optional<std::uint32_t> primaryDepth;
    std::optional<std::uint32_t> invisibleDepth;
  };
  std::unordered_map<ChartVisualId, BuiltInRowDepths> builtInDepths;
  builtInDepths.reserve(orderedTimelines.size());
  gameplay_note_submission_order::Allocator builtInOrder;
  // BMSRenderer always reserves one shared order for long notes whose heads
  // have already passed the retained traversal window.
  const auto pastLongNoteOrder = builtInOrder.captureLongNote();

  std::unordered_map<ChartVisualId, std::vector<const ChartVisualNote *>>
      notesByTimeline;
  notesByTimeline.reserve(orderedTimelines.size());
  for (const auto *note : orderedNotes) {
    notesByTimeline[note->timelineId].push_back(note);
  }
  for (const auto *timeline : orderedTimelines) {
    const auto rowIt = notesByTimeline.find(timeline->id);
    if (rowIt == notesByTimeline.end()) {
      continue;
    }
    const double rowScrollDelta =
        timeline->scrollPosition - result.currentScrollPosition;
    const bool rowIsWithinLatePoorWindow =
        isWithinLatePoorWindow(timeline->timeMicros, timeMicros, request);
    const bool rowHasLongHead =
        std::ranges::any_of(rowIt->second, [](const ChartVisualNote *note) {
          return effectiveSource(*note) == ChartVisualNoteSource::Playable &&
                 note->kind == ChartVisualNoteKind::LongHead;
        });
    bool needsPrimaryDepth = false;
    bool needsInvisibleDepth = false;
    for (const auto *note : rowIt->second) {
      const auto *noteState = stateFor(note->id);
      const bool dead = noteState != nullptr && noteState->dead;
      if (effectiveSource(*note) == ChartVisualNoteSource::Invisible) {
        needsInvisibleDepth =
            needsInvisibleDepth || (request.includeInvisibleNotes && !dead &&
                                    timeline->timeMicros >= timeMicros &&
                                    isVisible(rowScrollDelta, request));
        continue;
      }
      if (effectiveSource(*note) == ChartVisualNoteSource::Mine) {
        needsPrimaryDepth =
            needsPrimaryDepth || (!dead && timeline->timeMicros >= timeMicros &&
                                  isVisible(rowScrollDelta, request));
        continue;
      }
      if (effectiveSource(*note) == ChartVisualNoteSource::Playable &&
          note->kind == ChartVisualNoteKind::LongTail) {
        continue;
      }
      if (effectiveSource(*note) == ChartVisualNoteSource::Playable &&
          note->kind == ChartVisualNoteKind::LongHead) {
        const auto pairIt = notes.find(note->pairId);
        const ChartVisualNote *tail =
            pairIt == notes.end() ? nullptr : pairIt->second;
        const auto tailTimelineIt = tail == nullptr
                                        ? timelines.end()
                                        : timelines.find(tail->timelineId);
        const bool validPair =
            tail != nullptr &&
            effectiveSource(*tail) == ChartVisualNoteSource::Playable &&
            tail->kind == ChartVisualNoteKind::LongTail &&
            tail->pairId == note->id && tail->lane == note->lane &&
            tail->longNoteMode == note->longNoteMode &&
            tailTimelineIt != timelines.end() &&
            timeline->timeMicros <= tailTimelineIt->second->timeMicros;
        if (validPair) {
          const double tailScrollDelta =
              tailTimelineIt->second->scrollPosition -
              result.currentScrollPosition;
          needsPrimaryDepth =
              needsPrimaryDepth ||
              (rowIsWithinLatePoorWindow &&
               isLongIntervalVisible(rowScrollDelta, tailScrollDelta, request));
          continue;
        }
      }
      needsPrimaryDepth =
          needsPrimaryDepth ||
          (!dead && rowIsWithinLatePoorWindow &&
           isVisible(rowScrollDelta, request));
    }

    auto &depths = builtInDepths[timeline->id];
    if (needsPrimaryDepth) {
      if (rowHasLongHead) {
        depths.longOrder = builtInOrder.captureLongNote();
        depths.primaryDepth = depths.longOrder->endpointDepth;
      } else {
        depths.primaryDepth = builtInOrder.next();
      }
    }
    if (needsInvisibleDepth) {
      depths.invisibleDepth = builtInOrder.next();
    }
  }

  gameplay_note_submission_order::Allocator order;
  std::unordered_set<ChartVisualId> consumedLongEndpoints;
  const auto atNoteLimit = [&result, &request]() {
    return request.maxNotes != 0 &&
           result.notes.size() + result.longNotes.size() >= request.maxNotes;
  };
  for (const auto *note : orderedNotes) {
    if (consumedLongEndpoints.contains(note->id)) {
      continue;
    }
    const auto timelineIt = timelines.find(note->timelineId);
    if (timelineIt == timelines.end()) {
      continue;
    }
    const auto *timeline = timelineIt->second;
    const double scrollDelta =
        timeline->scrollPosition - result.currentScrollPosition;
    if (std::ranges::find(model.laneOrder, note->lane) ==
        model.laneOrder.end()) {
      continue;
    }
    const auto *noteState = stateFor(note->id);
    const auto source = effectiveSource(*note);
    if (source == ChartVisualNoteSource::Invisible &&
        (!request.includeInvisibleNotes || timeline->timeMicros < timeMicros)) {
      continue;
    }

    if (effectiveSource(*note) == ChartVisualNoteSource::Playable &&
        note->kind == ChartVisualNoteKind::LongHead) {
      const auto pairIt = notes.find(note->pairId);
      const ChartVisualNote *tail =
          pairIt == notes.end() ? nullptr : pairIt->second;
      const auto tailTimelineIt =
          tail == nullptr ? timelines.end() : timelines.find(tail->timelineId);
      const bool validPair =
          tail != nullptr &&
          effectiveSource(*tail) == ChartVisualNoteSource::Playable &&
          tail->kind == ChartVisualNoteKind::LongTail &&
          tail->pairId == note->id && tail->lane == note->lane &&
          tail->longNoteMode == note->longNoteMode &&
          tailTimelineIt != timelines.end() &&
          timeline->timeMicros <= tailTimelineIt->second->timeMicros;
      if (validPair) {
        const auto *tailTimeline = tailTimelineIt->second;
        const double tailScrollDelta =
            tailTimeline->scrollPosition - result.currentScrollPosition;
        if (!isLongIntervalVisible(scrollDelta, tailScrollDelta, request)) {
          continue;
        }
        if (atNoteLimit()) {
          result.budgetExceeded = true;
          continue;
        }
        if (!reserve(gameplay_chart_entity_render_budget::
                         kLongNoteReservationCost)) {
          continue;
        }
        const auto *tailState = stateFor(tail->id);
        const bool tailReleasedEarly =
            tailState != nullptr && tailState->judged &&
            tailState->playedTimeMicros != kPlayfieldTimestampOff &&
            tailState->playedTimeMicros < tailTimeline->timeMicros;
        const bool headDead = noteState != nullptr && noteState->dead;
        const bool tailDead = tailState != nullptr && tailState->dead;
        const auto rowDepthIt = builtInDepths.find(note->timelineId);
        const auto builtInLongOrder =
            !isWithinLatePoorWindow(timeline->timeMicros, timeMicros,
                                    request)
                ? pastLongNoteOrder
                : rowDepthIt != builtInDepths.end() &&
                    rowDepthIt->second.longOrder.has_value()
                      ? *rowDepthIt->second.longOrder
                      : gameplay_note_submission_order::LongNoteOrder{};
        result.longNotes.push_back(
            {.headId = note->id,
             .tailId = tail->id,
             .headTimelineId = timeline->id,
             .tailTimelineId = tailTimeline->id,
             // JsonPlaySkinObjectLoader preserves the zero-based Note arrays,
             // and LaneRenderer draws them with TimeLine.getNote(lane).  Its
             // lane is therefore the BMS lane ID, not the scratch-first UI
             // display order used by the built-in renderer.
             .lane = note->lane,
             .mode = note->longNoteMode,
             .headSource = effectiveSource(*note),
             .tailSource = effectiveSource(*tail),
             .headScrollDelta = scrollDelta,
             .tailScrollDelta = tailScrollDelta,
             .headTimeMicros = timeline->timeMicros,
             .tailTimeMicros = tailTimeline->timeMicros,
             .headAuthoredOrdinal = note->authoredOrdinal,
             .tailAuthoredOrdinal = tail->authoredOrdinal,
             .headRetainedOrdinal = timeline->retainedOrdinal,
             .tailRetainedOrdinal = tailTimeline->retainedOrdinal,
             .active = (noteState != nullptr && noteState->longActive) ||
                       (tailState != nullptr && tailState->longActive),
             .damaged = (noteState != nullptr && noteState->longDamaged) ||
                        (tailState != nullptr && tailState->longDamaged),
             .reactive = (noteState != nullptr && noteState->longReactive) ||
                         (tailState != nullptr && tailState->longReactive),
             .headPlayed = noteState != nullptr && noteState->judged,
             .tailPlayed = tailState != nullptr && tailState->judged,
             .headJudged =
                 noteState != nullptr && (noteState->judged || noteState->dead),
             .tailJudged =
                 tailState != nullptr && (tailState->judged || tailState->dead),
             .headDead = headDead,
             .tailDead = tailDead,
             .headPlayedTimeMicros = noteState != nullptr
                                         ? noteState->playedTimeMicros
                                         : kPlayfieldTimestampOff,
             .tailPlayedTimeMicros = tailState != nullptr
                                         ? tailState->playedTimeMicros
                                         : kPlayfieldTimestampOff,
             .tailReleasedEarly = tailReleasedEarly,
             .tailMissedWithHead = headDead && !tailDead && tailReleasedEarly,
             .tailResolvedAtOrAfterTiming =
                 tailState != nullptr && tailState->judged &&
                 tailState->playedTimeMicros != kPlayfieldTimestampOff &&
                 tailState->playedTimeMicros >= tailTimeline->timeMicros,
             .submissionOrdinal = order.next(),
             .bodyDepth = builtInLongOrder.bodyDepth,
             .endpointDepth = builtInLongOrder.endpointDepth});
        consumedLongEndpoints.insert(note->id);
        consumedLongEndpoints.insert(tail->id);
        continue;
      }
    }

    if (!isVisible(scrollDelta, request)) {
      continue;
    }

    if (noteState != nullptr && noteState->dead) {
      continue;
    }
    if (atNoteLimit()) {
      result.budgetExceeded = true;
      continue;
    }
    if (!reserve(
            gameplay_chart_entity_render_budget::kSingleRectangleEntityCost)) {
      continue;
    }
    result.notes.push_back(
        {.noteId = note->id,
         .timelineId = timeline->id,
         .pairId = note->pairId,
         // See the direct BMS-lane contract above for long notes too.
         .lane = note->lane,
         .kind = note->kind,
         .source = source,
         .longNoteMode = note->longNoteMode,
         .mineDamage = note->mineDamage,
         .scrollDelta = scrollDelta,
         .timeMicros = timeline->timeMicros,
         .authoredOrdinal = note->authoredOrdinal,
         .retainedTimelineOrdinal = timeline->retainedOrdinal,
         .judged = noteState != nullptr && noteState->judged,
         .submissionOrdinal = order.next(),
         .builtInDepth = [&]() {
           const auto found = builtInDepths.find(note->timelineId);
           if (found == builtInDepths.end()) {
             return std::uint32_t{0};
           }
           return source == ChartVisualNoteSource::Invisible
                      ? found->second.invisibleDepth.value_or(0)
                      : found->second.primaryDepth.value_or(0);
         }()});
  }

  // Build the built-in execution plan independently of the generic skin DTO
  // caps.  It contains only immutable values, but its retained-row walk uses
  // the same incremental future geometry as the legacy renderer.
  auto &builtInPlan = result.builtInPlan;
  std::vector<const ChartVisualTimeline *> retainedTimelines;
  retainedTimelines.reserve(orderedTimelines.size());
  for (const auto *timeline : orderedTimelines) {
    if (timeline->retainedForProjection &&
        timeline->retainedOrdinal != kNoRetainedTimelineOrdinal) {
      retainedTimelines.push_back(timeline);
    }
  }
  std::unordered_map<ChartVisualId, float> builtInTimelineY;
  builtInTimelineY.reserve(retainedTimelines.size());
  std::size_t builtInStartIndex = 0;
  if (request.builtInTraversal.has_value()) {
    const auto requestedStart = request.builtInTraversal->startRetainedOrdinal;
    const auto start = std::ranges::find_if(
        retainedTimelines, [requestedStart](const auto *timeline) {
          return timeline->retainedOrdinal >= requestedStart;
        });
    builtInStartIndex = static_cast<std::size_t>(
        std::distance(retainedTimelines.begin(), start));
  }
  builtInPlan.nextStartRetainedOrdinal = request.builtInTraversal.has_value()
                                               ? request.builtInTraversal
                                                     ->startRetainedOrdinal
                                               : 0U;
  if (request.builtInTraversal.has_value()) {
    const auto &traversal = *request.builtInTraversal;
    double futureY = traversal.judgeY;
    bool futureTraversalStarted = false;
    for (std::size_t index = builtInStartIndex;
         index < retainedTimelines.size(); ++index) {
      const auto *timeline = retainedTimelines[index];
      const bool future = timeline->timeMicros >= timeMicros;
      if (future && futureTraversalStarted &&
          !gameplay_scroll_geometry::futureTimelineTraversalContinues(
              futureY, traversal.upperBound)) {
        break;
      }
      float y = traversal.judgeY;
      if (future) {
        if (index == 0U) {
          y = gameplay_scroll_geometry::initialFutureTimelineY(
              timeline->scrollPosition, result.currentScrollPosition,
              traversal.rxhs, traversal.judgeY);
        } else {
          const auto *previous = retainedTimelines[index - 1U];
          futureY = gameplay_scroll_geometry::advanceFutureTimelineY(
              futureY, timeline->beat - previous->beat, previous->scrollRate,
              previous->timeMicros, previous->stopMicros, timeline->timeMicros,
              timeMicros, traversal.rxhs);
          y = static_cast<float>(futureY);
        }
        futureTraversalStarted = true;
      } else {
        y = gameplay_scroll_geometry::renderY(
            timeline->scrollPosition, result.currentScrollPosition,
            traversal.rxhs, traversal.judgeY);
      }
      builtInPlan.traversedTimelineOrdinals.push_back(
          timeline->retainedOrdinal);
      builtInPlan.timelines.push_back(
          {.retainedOrdinal = timeline->retainedOrdinal,
           .timeMicros = timeline->timeMicros,
           .renderY = y,
           .future = future});
      if (timeline->timeMicros <
          timeMicros - std::max<std::int64_t>(
                           0, request.latePoorTimingMicros)) {
        builtInPlan.nextStartRetainedOrdinal = timeline->retainedOrdinal;
      }
      builtInTimelineY.emplace(timeline->id, y);
    }
  } else {
    for (std::size_t index = builtInStartIndex;
         index < retainedTimelines.size(); ++index) {
      const auto *timeline = retainedTimelines[index];
      builtInPlan.traversedTimelineOrdinals.push_back(
          timeline->retainedOrdinal);
      builtInPlan.timelines.push_back(
          {.retainedOrdinal = timeline->retainedOrdinal,
           .timeMicros = timeline->timeMicros,
           .renderY = std::numeric_limits<float>::quiet_NaN(),
           .future = timeline->timeMicros >= timeMicros});
      if (timeline->timeMicros <
          timeMicros - std::max<std::int64_t>(
                           0, request.latePoorTimingMicros)) {
        builtInPlan.nextStartRetainedOrdinal = timeline->retainedOrdinal;
      }
    }
  }
  const auto builtInTimelineWasTraversed = [&builtInPlan](
                                           std::uint32_t ordinal) {
    return std::ranges::find(builtInPlan.traversedTimelineOrdinals, ordinal) !=
           builtInPlan.traversedTimelineOrdinals.end();
  };
  for (const auto *timeline : retainedTimelines) {
    if (!builtInTimelineWasTraversed(timeline->retainedOrdinal) ||
        !timeline->sectionLine || timeline->timeMicros < timeMicros) {
      continue;
    }
    if (request.builtInTraversal.has_value()) {
      const auto y = builtInTimelineY.find(timeline->id);
      if (y == builtInTimelineY.end() ||
          !gameplay_scroll_geometry::shouldDrawMeasureLine(
              timeline->timeMicros, timeMicros, y->second,
              request.builtInTraversal->judgeY,
              request.builtInTraversal->upperBound)) {
        continue;
      }
    }
    builtInPlan.lines.push_back(
        {.timelineId = timeline->id,
         .kind = ProjectedLineKind::Section,
         .scrollDelta = timeline->scrollPosition - result.currentScrollPosition,
         .timeMicros = timeline->timeMicros,
         .authoredOrdinal = timeline->authoredOrdinal,
         .retainedOrdinal = timeline->retainedOrdinal,
         .submissionOrdinal =
             static_cast<std::uint32_t>(builtInPlan.entries.size())});
    builtInPlan.entries.push_back(
        {.kind = BuiltInRendererPlanEntryKind::SectionLine,
         .descriptorIndex =
             static_cast<std::uint32_t>(builtInPlan.lines.size() - 1U),
         .renderY = request.builtInTraversal.has_value()
                        ? builtInTimelineY.at(timeline->id)
                        : std::numeric_limits<float>::quiet_NaN()});
  }
  for (const auto *note : orderedNotes) {
    const auto timelineIt = timelines.find(note->timelineId);
    if (timelineIt == timelines.end()) {
      continue;
    }
    const auto *timeline = timelineIt->second;
    if (!builtInTimelineWasTraversed(timeline->retainedOrdinal) ||
        note->kind == ChartVisualNoteKind::LongHead ||
        note->kind == ChartVisualNoteKind::LongTail) {
      continue;
    }
    const auto source = effectiveSource(*note);
    const auto *noteState = stateFor(note->id);
    const bool dead = noteState != nullptr && noteState->dead;
    const bool eligible =
        source == ChartVisualNoteSource::Invisible
            ? request.includeInvisibleNotes && !dead &&
                  timeline->timeMicros >= timeMicros
            : source == ChartVisualNoteSource::Mine
                  ? !dead && timeline->timeMicros >= timeMicros
                  : !dead && isWithinLatePoorWindow(timeline->timeMicros,
                                                    timeMicros, request);
    if (!eligible) {
      continue;
    }
    const auto lane = std::ranges::find(model.laneOrder, note->lane);
    if (lane == model.laneOrder.end()) {
      continue;
    }
    const auto depth = builtInDepths.find(note->timelineId);
    builtInPlan.notes.push_back(
        {.noteId = note->id,
         .timelineId = timeline->id,
         .pairId = note->pairId,
         .lane = static_cast<int>(lane - model.laneOrder.begin()),
         .kind = note->kind,
         .source = source,
         .longNoteMode = note->longNoteMode,
         .mineDamage = note->mineDamage,
         .scrollDelta = timeline->scrollPosition - result.currentScrollPosition,
         .timeMicros = timeline->timeMicros,
         .authoredOrdinal = note->authoredOrdinal,
         .retainedTimelineOrdinal = timeline->retainedOrdinal,
         .judged = noteState != nullptr && noteState->judged,
         .submissionOrdinal =
             static_cast<std::uint32_t>(builtInPlan.entries.size()),
         .builtInDepth =
             depth == builtInDepths.end()
                 ? 0U
                 : source == ChartVisualNoteSource::Invisible
                       ? depth->second.invisibleDepth.value_or(0U)
                       : depth->second.primaryDepth.value_or(0U)});
    builtInPlan.entries.push_back(
        {.kind = BuiltInRendererPlanEntryKind::Note,
         .descriptorIndex =
             static_cast<std::uint32_t>(builtInPlan.notes.size() - 1U),
         .renderY = request.builtInTraversal.has_value()
                        ? builtInTimelineY.at(timeline->id)
                        : std::numeric_limits<float>::quiet_NaN()});
  }
  std::unordered_set<ChartVisualId> builtInLongHeads;
  for (const auto *head : orderedNotes) {
    if (head->kind != ChartVisualNoteKind::LongHead ||
        effectiveSource(*head) != ChartVisualNoteSource::Playable ||
        !builtInLongHeads.insert(head->id).second) {
      continue;
    }
    const auto headTimelineIt = timelines.find(head->timelineId);
    const auto tailIt = notes.find(head->pairId);
    if (headTimelineIt == timelines.end() || tailIt == notes.end()) {
      continue;
    }
    const auto *headTimeline = headTimelineIt->second;
    const auto *tail = tailIt->second;
    const auto tailTimelineIt = timelines.find(tail->timelineId);
    if (tailTimelineIt == timelines.end() ||
        effectiveSource(*tail) != ChartVisualNoteSource::Playable ||
        tail->kind != ChartVisualNoteKind::LongTail ||
        tail->pairId != head->id || tail->lane != head->lane ||
        tail->longNoteMode != head->longNoteMode ||
        headTimeline->timeMicros > tailTimelineIt->second->timeMicros) {
      continue;
    }
    const auto *tailTimeline = tailTimelineIt->second;
    const bool headTraversed =
        builtInTimelineWasTraversed(headTimeline->retainedOrdinal);
    const bool tailTraversed =
        builtInTimelineWasTraversed(tailTimeline->retainedOrdinal);
    const bool spansDerivedStart =
        headTimeline->timeMicros <
            timeMicros - std::max<std::int64_t>(
                             0, request.latePoorTimingMicros) &&
        tailTimeline->timeMicros >=
            timeMicros - std::max<std::int64_t>(
                             0, request.latePoorTimingMicros);
    if (!headTraversed && !tailTraversed && !spansDerivedStart) {
      continue;
    }
    const auto lane = std::ranges::find(model.laneOrder, head->lane);
    if (lane == model.laneOrder.end()) {
      continue;
    }
    const auto *headState = stateFor(head->id);
    const auto *tailState = stateFor(tail->id);
    const bool tailReleasedEarly =
        tailState != nullptr && tailState->judged &&
        tailState->playedTimeMicros != kPlayfieldTimestampOff &&
        tailState->playedTimeMicros < tailTimeline->timeMicros;
    const auto depth = builtInDepths.find(head->timelineId);
    const auto longOrder =
        !isWithinLatePoorWindow(headTimeline->timeMicros, timeMicros, request)
            ? pastLongNoteOrder
            : depth != builtInDepths.end() && depth->second.longOrder.has_value()
                  ? *depth->second.longOrder
                  : gameplay_note_submission_order::LongNoteOrder{};
    builtInPlan.longNotes.push_back(
        {.headId = head->id,
         .tailId = tail->id,
         .headTimelineId = headTimeline->id,
         .tailTimelineId = tailTimeline->id,
         .lane = static_cast<int>(lane - model.laneOrder.begin()),
         .mode = head->longNoteMode,
         .headSource = effectiveSource(*head),
         .tailSource = effectiveSource(*tail),
         .headScrollDelta =
             headTimeline->scrollPosition - result.currentScrollPosition,
         .tailScrollDelta =
             tailTimeline->scrollPosition - result.currentScrollPosition,
         .headTimeMicros = headTimeline->timeMicros,
         .tailTimeMicros = tailTimeline->timeMicros,
         .headAuthoredOrdinal = head->authoredOrdinal,
         .tailAuthoredOrdinal = tail->authoredOrdinal,
         .headRetainedOrdinal = headTimeline->retainedOrdinal,
         .tailRetainedOrdinal = tailTimeline->retainedOrdinal,
         .active = (headState != nullptr && headState->longActive) ||
                   (tailState != nullptr && tailState->longActive),
         .damaged = (headState != nullptr && headState->longDamaged) ||
                    (tailState != nullptr && tailState->longDamaged),
         .reactive = (headState != nullptr && headState->longReactive) ||
                     (tailState != nullptr && tailState->longReactive),
         .headPlayed = headState != nullptr && headState->judged,
         .tailPlayed = tailState != nullptr && tailState->judged,
         .headJudged = headState != nullptr &&
                        (headState->judged || headState->dead),
         .tailJudged = tailState != nullptr &&
                        (tailState->judged || tailState->dead),
         .headDead = headState != nullptr && headState->dead,
         .tailDead = tailState != nullptr && tailState->dead,
         .headPlayedTimeMicros =
             headState != nullptr ? headState->playedTimeMicros
                                  : kPlayfieldTimestampOff,
         .tailPlayedTimeMicros =
             tailState != nullptr ? tailState->playedTimeMicros
                                  : kPlayfieldTimestampOff,
         .tailReleasedEarly = tailReleasedEarly,
         .tailMissedWithHead =
             headState != nullptr && headState->dead &&
             !(tailState != nullptr && tailState->dead) && tailReleasedEarly,
         .tailResolvedAtOrAfterTiming =
             tailState != nullptr && tailState->judged &&
             tailState->playedTimeMicros != kPlayfieldTimestampOff &&
             tailState->playedTimeMicros >= tailTimeline->timeMicros,
         .submissionOrdinal =
             static_cast<std::uint32_t>(builtInPlan.entries.size()),
         .bodyDepth = longOrder.bodyDepth,
         .endpointDepth = longOrder.endpointDepth});
    const float headY = request.builtInTraversal.has_value()
                            ? (!headTraversed ||
                               !isWithinLatePoorWindow(headTimeline->timeMicros,
                                                       timeMicros, request)
                                   ? request.builtInTraversal->lowerBound
                                   : builtInTimelineY.at(headTimeline->id))
                            : std::numeric_limits<float>::quiet_NaN();
    const float tailY = request.builtInTraversal.has_value()
                            ? (tailTraversed
                                   ? builtInTimelineY.at(tailTimeline->id)
                                   : request.builtInTraversal->upperBound)
                            : std::numeric_limits<float>::quiet_NaN();
    builtInPlan.entries.push_back(
        {.kind = BuiltInRendererPlanEntryKind::LongNote,
         .descriptorIndex =
             static_cast<std::uint32_t>(builtInPlan.longNotes.size() - 1U),
         .tailAtUpperBound = !tailTraversed,
         .renderY = headY,
         .tailRenderY = tailY});
  }
  // Plan depth allocation deliberately ignores generic viewport/max DTO
  // filtering. It follows only the rows this immutable built-in walk visits.
  std::unordered_map<std::uint32_t, BuiltInRowDepths> planDepths;
  gameplay_note_submission_order::Allocator planOrder;
  const auto planPastLongOrder = planOrder.captureLongNote();
  for (const auto ordinal : builtInPlan.traversedTimelineOrdinals) {
    bool rowHasLongHead = false;
    bool needsPrimary = false;
    bool needsInvisible = false;
    for (const auto &longNote : builtInPlan.longNotes) {
      if (longNote.headRetainedOrdinal != ordinal) {
        continue;
      }
      rowHasLongHead = true;
      const std::uint32_t startOrdinal =
          request.builtInTraversal.has_value()
              ? request.builtInTraversal->startRetainedOrdinal
              : 0U;
      const bool isCursorCarriedPastHead =
          startOrdinal != 0U &&
          longNote.headRetainedOrdinal <= startOrdinal &&
          !isWithinLatePoorWindow(longNote.headTimeMicros, timeMicros,
                                  request);
      needsPrimary = needsPrimary || !isCursorCarriedPastHead;
    }
    for (const auto &note : builtInPlan.notes) {
      if (note.retainedTimelineOrdinal != ordinal) {
        continue;
      }
      if (effectiveSource(ChartVisualNote{.kind = note.kind,
                                          .source = note.source}) ==
          ChartVisualNoteSource::Invisible) {
        needsInvisible = true;
      } else {
        needsPrimary = true;
      }
    }
    auto &depth = planDepths[ordinal];
    if (needsPrimary) {
      if (rowHasLongHead) {
        depth.longOrder = planOrder.captureLongNote();
        depth.primaryDepth = depth.longOrder->endpointDepth;
      } else {
        depth.primaryDepth = planOrder.next();
      }
    }
    if (needsInvisible) {
      depth.invisibleDepth = planOrder.next();
    }
  }
  for (auto &note : builtInPlan.notes) {
    const auto depth = planDepths.find(note.retainedTimelineOrdinal);
    if (depth == planDepths.end()) {
      continue;
    }
    note.builtInDepth =
        effectiveSource(ChartVisualNote{.kind = note.kind,
                                        .source = note.source}) ==
                ChartVisualNoteSource::Invisible
            ? depth->second.invisibleDepth.value_or(0U)
            : depth->second.primaryDepth.value_or(0U);
  }
  for (auto &longNote : builtInPlan.longNotes) {
    const std::uint32_t startOrdinal =
        request.builtInTraversal.has_value()
            ? request.builtInTraversal->startRetainedOrdinal
            : 0U;
    const bool isCursorCarriedPastHead =
        startOrdinal != 0U &&
        longNote.headRetainedOrdinal <= startOrdinal &&
        !isWithinLatePoorWindow(longNote.headTimeMicros, timeMicros, request);
    if (isCursorCarriedPastHead) {
      longNote.bodyDepth = planPastLongOrder.bodyDepth;
      longNote.endpointDepth = planPastLongOrder.endpointDepth;
      continue;
    }
    const auto depth = planDepths.find(longNote.headRetainedOrdinal);
    if (depth != planDepths.end() && depth->second.longOrder.has_value()) {
      longNote.bodyDepth = depth->second.longOrder->bodyDepth;
      longNote.endpointDepth = depth->second.longOrder->endpointDepth;
    }
  }
  const auto entryTimelineOrdinal = [&builtInPlan](
                                        const BuiltInRendererPlanEntry &entry) {
    switch (entry.kind) {
    case BuiltInRendererPlanEntryKind::SectionLine:
      return builtInPlan.lines[entry.descriptorIndex].retainedOrdinal;
    case BuiltInRendererPlanEntryKind::Note:
      return builtInPlan.notes[entry.descriptorIndex].retainedTimelineOrdinal;
    case BuiltInRendererPlanEntryKind::LongNote:
      return entry.tailAtUpperBound
                 ? kNoRetainedTimelineOrdinal
                 : builtInPlan.longNotes[entry.descriptorIndex]
                       .tailRetainedOrdinal;
    }
    return kNoRetainedTimelineOrdinal;
  };
  const auto lanePhaseAndOrder = [&builtInPlan, &model, &request](
                                     const BuiltInRendererPlanEntry &entry) {
    if (entry.kind == BuiltInRendererPlanEntryKind::SectionLine) {
      return std::pair{0U, 0U};
    }
    if (entry.kind == BuiltInRendererPlanEntryKind::LongNote &&
        entry.tailAtUpperBound) {
      // BMSRenderer's legacy lookahead flush is keyed by long-head identity.
      // Its stable compatibility order is descending retained-head ordinal.
      return std::pair{4U,
                       std::numeric_limits<unsigned>::max() -
                           builtInPlan.longNotes[entry.descriptorIndex]
                               .headRetainedOrdinal};
    }
    const ProjectedPlayfieldNote *note = nullptr;
    int projectedLane = -1;
    ChartVisualNoteSource source = ChartVisualNoteSource::Playable;
    if (entry.kind == BuiltInRendererPlanEntryKind::LongNote) {
      const auto &longNote = builtInPlan.longNotes[entry.descriptorIndex];
      projectedLane = longNote.lane;
    } else {
      note = &builtInPlan.notes[entry.descriptorIndex];
      projectedLane = note->lane;
      source = effectiveSource(
          ChartVisualNote{.kind = note->kind, .source = note->source});
    }
    if (source == ChartVisualNoteSource::Mine) {
      return std::pair{2U, 0U};
    }
    if (source == ChartVisualNoteSource::Invisible) {
      return std::pair{3U, 0U};
    }
    const int rawLane = projectedLane >= 0 &&
                                static_cast<std::size_t>(projectedLane) <
                                    model.laneOrder.size()
                            ? model.laneOrder[static_cast<std::size_t>(projectedLane)]
                            : projectedLane;
    const auto &laneOrder = request.builtInTraversal.has_value() &&
                                    !request.builtInTraversal->playableLaneOrder.empty()
                                ? request.builtInTraversal->playableLaneOrder
                                : model.laneOrder;
    const auto found = std::ranges::find(laneOrder, rawLane);
    return std::pair{1U, static_cast<unsigned>(
                             std::distance(laneOrder.begin(), found))};
  };
  std::stable_sort(builtInPlan.entries.begin(), builtInPlan.entries.end(),
                   [&entryTimelineOrdinal, &lanePhaseAndOrder](const auto &left, const auto &right) {
                     const auto leftTimeline = entryTimelineOrdinal(left);
                     const auto rightTimeline = entryTimelineOrdinal(right);
                     if (leftTimeline != rightTimeline) {
                       return leftTimeline < rightTimeline;
                     }
                     return lanePhaseAndOrder(left) < lanePhaseAndOrder(right);
                   });
  return result;
}

void PlayfieldProjection::reset() noexcept {}

PlayfieldSkinProjectionViews
adaptPlayfieldProjectionForSkin(const PlayfieldProjectionResult &projection) {
  PlayfieldSkinProjectionViews result;
  result.notes.reserve(projection.notes.size());
  result.longNotes.reserve(projection.longNotes.size());
  result.lines.reserve(projection.lines.size());
  // SkinNote/LaneRenderer calculates its own rxhs from `lanes[0].region`.
  // Keep the abstract scroll delta and publish only the captured unitless
  // hispeed; Skin2DRenderer then applies each skin's note.dst lane height.
  const std::optional<double> scrollSpeed =
      projection.builtInTraversal &&
              std::isfinite(projection.builtInTraversal->hispeed) &&
              projection.builtInTraversal->hispeed > 0.0F
          ? std::optional<double>{projection.builtInTraversal->hispeed}
          : std::nullopt;
  for (const auto &note : projection.notes) {
    result.notes.push_back({.visualId = note.noteId,
                            .lane = note.lane,
                            .kind = toSkinNoteKind(note.source),
                            .scrollSpeed = scrollSpeed,
                            .authoredYDisplacement = note.scrollDelta,
                            .judged = note.judged,
                            .submissionOrdinal = note.submissionOrdinal});
  }
  for (const auto &longNote : projection.longNotes) {
    result.longNotes.push_back(
        {.headVisualId = longNote.headId,
         .tailVisualId = longNote.tailId,
         .lane = longNote.lane,
         .mode = toSkinLongNoteMode(longNote.mode),
         .scrollSpeed = scrollSpeed,
         .headAuthoredYDisplacement = longNote.headScrollDelta,
         .tailAuthoredYDisplacement = longNote.tailScrollDelta,
         .active = longNote.active,
         .damaged = longNote.damaged,
         .reactive = longNote.reactive,
         .headJudged = longNote.headJudged,
         .tailJudged = longNote.tailJudged,
         .submissionOrdinal = longNote.submissionOrdinal});
  }
  for (const auto &line : projection.lines) {
    result.lines.push_back({.timelineVisualId = line.timelineId,
                            .kind = toSkinLineKind(line.kind),
                            .scrollSpeed = scrollSpeed,
                            .authoredYDisplacement = line.scrollDelta,
                            .submissionOrdinal = line.submissionOrdinal});
  }
  return result;
}

namespace replay_ghost {

std::vector<ReplayGhostEvent>
buildReplayGhostEvents(const ReplayData &replayData,
                       const PlayfieldChartVisualModel &model) {
  std::unordered_set<int> playableLanes(model.laneOrder.begin(),
                                        model.laneOrder.end());
  std::vector<long long> timelineTimes;
  timelineTimes.reserve(model.timelines.size());
  for (const auto &timeline : model.timelines) {
    // Match BMSRenderer's ordered timeline input: discarded chart rows do not
    // participate in either note projection or replay-ghost lookup.
    if (timeline.retainedForProjection) {
      timelineTimes.push_back(timeline.timeMicros);
    }
  }
  std::ranges::sort(timelineTimes);
  timelineTimes.erase(std::unique(timelineTimes.begin(), timelineTimes.end()),
                      timelineTimes.end());
  return detail::buildReplayGhostEvents(
      replayData,
      [&playableLanes](int lane) {
        return playableLanes.find(lane) != playableLanes.end();
      },
      [&timelineTimes](long long time) {
        return std::binary_search(timelineTimes.begin(), timelineTimes.end(),
                                  time);
      },
      [&model](long long time) { return scrollPositionAtTime(model, time); });
}

} // namespace replay_ghost
