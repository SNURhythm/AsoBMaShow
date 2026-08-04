#include "PlayfieldProjection.h"

#include "GameplayChartEntityRenderBudget.h"
#include "GameplayNoteSubmissionOrder.h"
#include "GameplayScrollGeometry.h"

#include <algorithm>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace {

bool isLongEndpoint(ChartVisualNoteKind kind) {
  return kind == ChartVisualNoteKind::LongHead ||
         kind == ChartVisualNoteKind::LongTail;
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
  if (request.visibleScrollBefore == 0.0 &&
      request.visibleScrollAfter == 0.0) {
    return true;
  }
  const double minimum = -request.visibleScrollBefore;
  const double maximum = request.visibleScrollAfter;
  return std::max(headScrollDelta, tailScrollDelta) >= minimum &&
         std::min(headScrollDelta, tailScrollDelta) <= maximum;
}

skin::SkinProjectedNoteKind toSkinNoteKind(ChartVisualNoteKind kind) {
  switch (kind) {
  case ChartVisualNoteKind::Invisible:
    return skin::SkinProjectedNoteKind::Invisible;
  case ChartVisualNoteKind::Mine:
    return skin::SkinProjectedNoteKind::Mine;
  case ChartVisualNoteKind::Normal:
  case ChartVisualNoteKind::LongHead:
  case ChartVisualNoteKind::LongBody:
  case ChartVisualNoteKind::LongTail:
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
  if (model.timelines.empty()) {
    return 0.0;
  }
  const auto next = std::upper_bound(
      model.timelines.begin(), model.timelines.end(), timeMicros,
      [](long long time, const ChartVisualTimeline &timeline) {
        return time < timeline.timeMicros;
      });
  if (next == model.timelines.begin()) {
    return next->scrollPosition;
  }
  const auto &previous = *std::prev(next);
  if (next == model.timelines.end() ||
      next->timeMicros == previous.timeMicros) {
    return previous.scrollPosition;
  }
  const long long stopEnd = previous.timeMicros + previous.stopMicros;
  if (timeMicros <= stopEnd) {
    return previous.scrollPosition;
  }
  const long long travelDuration =
      next->timeMicros - previous.timeMicros - previous.stopMicros;
  if (travelDuration <= 0) {
    return next->scrollPosition;
  }
  const double progress = std::clamp(
      static_cast<double>(timeMicros - stopEnd) /
          static_cast<double>(travelDuration),
      0.0, 1.0);
  return previous.scrollPosition +
         (next->scrollPosition - previous.scrollPosition) * progress;
}

PlayfieldProjectionResult PlayfieldProjection::project(
    const PlayfieldChartVisualModel &model, const PlayfieldVisualState &state,
    const PlayfieldProjectionRequest &request) {
  PlayfieldProjectionResult result;
  result.frameSerial = state.clock.serial;
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
  const auto stateFor = [&noteStates](ChartVisualId id)
      -> const NotePresentationState * {
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
                              ChartVisualId timelineId, ProjectedLineKind kind,
                              double scrollDelta) {
    if (!reserve(gameplay_chart_entity_render_budget::kSingleRectangleEntityCost)) {
      return;
    }
    result.lines.push_back({.timelineId = timelineId,
                            .kind = kind,
                            .scrollDelta = scrollDelta,
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
             .submissionOrdinal = timeline->authoredOrdinal});
        if (timeline->sectionLine) {
          appendLine(timeline->id, ProjectedLineKind::Section, scrollDelta);
        }
        if (previousTimeline != nullptr && timeline->bpm != previousTimeline->bpm) {
          appendLine(timeline->id, ProjectedLineKind::BpmChange, scrollDelta);
        }
        if (timeline->stopMicros > 0) {
          appendLine(timeline->id, ProjectedLineKind::Stop, scrollDelta);
        }
        appendLine(timeline->id, ProjectedLineKind::Time, scrollDelta);
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
  (void)builtInOrder.captureLongNote();

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
    const bool rowHasLongHead = std::ranges::any_of(
        rowIt->second, [](const ChartVisualNote *note) {
          return note->kind == ChartVisualNoteKind::LongHead;
        });
    bool needsPrimaryDepth = false;
    bool needsInvisibleDepth = false;
    for (const auto *note : rowIt->second) {
      const auto *noteState = stateFor(note->id);
      const bool dead = noteState != nullptr && noteState->dead;
      if (note->kind == ChartVisualNoteKind::Invisible) {
        needsInvisibleDepth =
            needsInvisibleDepth ||
            (request.includeInvisibleNotes && !dead &&
             timeline->timeMicros >= timeMicros &&
             isVisible(rowScrollDelta, request));
        continue;
      }
      if (note->kind == ChartVisualNoteKind::Mine) {
        needsPrimaryDepth =
            needsPrimaryDepth ||
            (!dead && timeline->timeMicros >= timeMicros &&
             isVisible(rowScrollDelta, request));
        continue;
      }
      if (note->kind == ChartVisualNoteKind::LongTail) {
        continue;
      }
      if (note->kind == ChartVisualNoteKind::LongHead) {
        const auto pairIt = notes.find(note->pairId);
        const ChartVisualNote *tail =
            pairIt == notes.end() ? nullptr : pairIt->second;
        const auto tailTimelineIt =
            tail == nullptr ? timelines.end() : timelines.find(tail->timelineId);
        const bool validPair =
            tail != nullptr && tail->kind == ChartVisualNoteKind::LongTail &&
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
              isLongIntervalVisible(rowScrollDelta, tailScrollDelta, request);
          continue;
        }
      }
      needsPrimaryDepth =
          needsPrimaryDepth || (!dead && isVisible(rowScrollDelta, request));
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
    const auto lane = std::ranges::find(model.laneOrder, note->lane);
    if (lane == model.laneOrder.end()) {
      continue;
    }
    const auto *noteState = stateFor(note->id);
    if (note->kind == ChartVisualNoteKind::Invisible &&
        (!request.includeInvisibleNotes || timeline->timeMicros < timeMicros)) {
      continue;
    }

    if (note->kind == ChartVisualNoteKind::LongHead) {
      const auto pairIt = notes.find(note->pairId);
      const ChartVisualNote *tail =
          pairIt == notes.end() ? nullptr : pairIt->second;
      const auto tailTimelineIt =
          tail == nullptr ? timelines.end() : timelines.find(tail->timelineId);
      const bool validPair = tail != nullptr &&
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
        if (!reserve(gameplay_chart_entity_render_budget::kLongNoteReservationCost)) {
          continue;
        }
        const auto *tailState = stateFor(tail->id);
        const auto rowDepthIt = builtInDepths.find(note->timelineId);
        const auto builtInLongOrder =
            rowDepthIt != builtInDepths.end() &&
                    rowDepthIt->second.longOrder.has_value()
                ? *rowDepthIt->second.longOrder
                : gameplay_note_submission_order::LongNoteOrder{};
        result.longNotes.push_back(
            {.headId = note->id,
             .tailId = tail->id,
             .lane = static_cast<int>(lane - model.laneOrder.begin()),
             .mode = note->longNoteMode,
             .headScrollDelta = scrollDelta,
             .tailScrollDelta = tailScrollDelta,
             .active = (noteState != nullptr && noteState->longActive) ||
                       (tailState != nullptr && tailState->longActive),
             .damaged = (noteState != nullptr && noteState->longDamaged) ||
                        (tailState != nullptr && tailState->longDamaged),
             .reactive = (noteState != nullptr && noteState->longReactive) ||
                         (tailState != nullptr && tailState->longReactive),
             .headJudged = noteState != nullptr &&
                            (noteState->judged || noteState->dead),
             .tailJudged = tailState != nullptr &&
                            (tailState->judged || tailState->dead),
             .headDead = noteState != nullptr && noteState->dead,
             .tailDead = tailState != nullptr && tailState->dead,
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
    if (!reserve(gameplay_chart_entity_render_budget::kSingleRectangleEntityCost)) {
      continue;
    }
    result.notes.push_back(
        {.noteId = note->id,
         .lane = static_cast<int>(lane - model.laneOrder.begin()),
         .kind = note->kind,
         .scrollDelta = scrollDelta,
         .judged = noteState != nullptr && noteState->judged,
         .submissionOrdinal = order.next(),
         .builtInDepth = [&]() {
           const auto found = builtInDepths.find(note->timelineId);
           if (found == builtInDepths.end()) {
             return std::uint32_t{0};
           }
           return note->kind == ChartVisualNoteKind::Invisible
                      ? found->second.invisibleDepth.value_or(0)
                      : found->second.primaryDepth.value_or(0);
         }()});
  }
  return result;
}

void PlayfieldProjection::reset() noexcept {}

PlayfieldSkinProjectionViews
adaptPlayfieldProjectionForSkin(const PlayfieldProjectionResult &projection) {
  PlayfieldSkinProjectionViews result;
  result.notes.reserve(projection.notes.size());
  result.longNotes.reserve(projection.longNotes.size());
  result.lines.reserve(projection.lines.size());
  for (const auto &note : projection.notes) {
    result.notes.push_back({.visualId = note.noteId,
                            .lane = note.lane,
                            .kind = toSkinNoteKind(note.kind),
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
                            .authoredYDisplacement = line.scrollDelta,
                            .submissionOrdinal = line.submissionOrdinal});
  }
  return result;
}
