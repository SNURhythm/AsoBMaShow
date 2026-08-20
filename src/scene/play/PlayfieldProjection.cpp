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

struct ProjectionScrollInterval {
  double minimum = 0.0;
  double maximum = 0.0;
};

std::optional<ProjectionScrollInterval>
visibleScrollInterval(const PlayfieldProjectionRequest &request) {
  if (request.visibleScrollBefore != 0.0 || request.visibleScrollAfter != 0.0) {
    return ProjectionScrollInterval{.minimum = -request.visibleScrollBefore,
                                    .maximum = request.visibleScrollAfter};
  }
  // LaneRenderer starts at the judgement line and walks forward only while
  // `y <= hu`. Its `rxhs` is `(hu - hl) * hispeed`, leaving exactly
  // `1 / hispeed` abstract scroll units in the visible lane. The generic DTO
  // feeds SkinNote, so use the captured traversal whenever the caller did not
  // provide an explicit projection window.
  if (request.builtInTraversal &&
      std::isfinite(request.builtInTraversal->hispeed) &&
      request.builtInTraversal->hispeed > 0.0F) {
    return ProjectionScrollInterval{
        .minimum = 0.0,
        .maximum = 1.0 / static_cast<double>(request.builtInTraversal->hispeed)};
  }
  return std::nullopt;
}

bool isVisible(
    double scrollDelta,
    const std::optional<ProjectionScrollInterval> &interval) {
  return !interval || (scrollDelta >= interval->minimum &&
                       scrollDelta <= interval->maximum);
}

bool isLongIntervalVisible(double headScrollDelta, double tailScrollDelta,
                           const std::optional<ProjectionScrollInterval>
                               &interval) {
  if (!interval) {
    return true;
  }
  return std::max(headScrollDelta, tailScrollDelta) >= interval->minimum &&
         std::min(headScrollDelta, tailScrollDelta) <= interval->maximum;
}

bool isWithinLatePoorWindow(
    long long timelineMicros, long long visualTimeMicros,
    const PlayfieldProjectionRequest &request) noexcept {
  const auto latePoorTiming = std::max<std::int64_t>(
      0, request.latePoorTimingMicros);
  return timelineMicros >= visualTimeMicros - latePoorTiming;
}

// LaneRenderer draws ordinary, mine, and hidden single notes only while their
// timeline is at or ahead of the current visual time. Long-note heads have a
// separate tail-span rule below. PlayerConfig.showpastnote is false by
// default; Aso does not currently expose that optional Beatoraja setting.
bool isSourceStateZero(const NotePresentationState *state) noexcept {
  // Aso carries the source normal-note state through its explicit resolved
  // flags: an unresolved note has neither a judged nor a dead transition.
  return state == nullptr || (!state->judged && !state->dead);
}

// Note.STATE_DEAD is in the source's >= 4 range. Every intermediate judged
// state is deliberately excluded from dst2's normal-note paths.
bool isPmsPoorEligibleNormal(const ChartVisualNote &note,
                             ChartVisualNoteSource source,
                             const NotePresentationState *state) noexcept {
  return source == ChartVisualNoteSource::Playable &&
         note.kind == ChartVisualNoteKind::Normal &&
         (state == nullptr || !state->judged || state->dead);
}

void advancePmsPoorCursor(
    std::size_t &cursor,
    const std::vector<const ChartVisualTimeline *> &orderedTimelines,
    const std::unordered_map<ChartVisualId,
                             std::vector<const ChartVisualNote *>>
        &notesByTimeline,
    const std::unordered_map<ChartVisualId, const ChartVisualNote *> &notes,
    const std::unordered_map<ChartVisualId, const ChartVisualTimeline *>
        &timelines,
    const PlayfieldVisualState &state, long long timeMicros,
    bool showPastNormalNotes) {
  if (cursor >= orderedTimelines.size()) {
    return;
  }
  for (std::size_t index = cursor; index < orderedTimelines.size(); ++index) {
    const auto *timeline = orderedTimelines[index];
    if (timeline->timeMicros >= timeMicros || cursor != index - 1U) {
      continue;
    }
    bool retainCursor = false;
    if (const auto row = notesByTimeline.find(timeline->id);
        row != notesByTimeline.end()) {
      for (const auto *note : row->second) {
        if (effectiveSource(*note) != ChartVisualNoteSource::Playable) {
          continue;
        }
        if (note->kind == ChartVisualNoteKind::LongHead) {
          const auto pair = notes.find(note->pairId);
          const auto pairTimeline =
              pair == notes.end() ? timelines.end()
                                  : timelines.find(pair->second->timelineId);
          retainCursor =
              pairTimeline != timelines.end() &&
              pairTimeline->second->timeMicros >= timeMicros;
        } else if (showPastNormalNotes &&
                   isSourceStateZero(state.noteState(note->id))) {
          retainCursor = true;
        }
        if (retainCursor) {
          break;
        }
      }
    }
    if (!retainCursor) {
      cursor = index;
    }
  }
}

struct PmsPoorTimelineDescent {
  ChartVisualId timelineId = 0;
  double yDisplacement = 0.0;
};

std::vector<PmsPoorTimelineDescent> pmsPoorTimelineDescents(
    std::span<const ChartVisualTimeline *const> orderedTimelines,
    std::size_t cursor, long long timeMicros, long long badTimeMicros,
    const PmsPoorDestinationGeometry &geometry, bool liftEnabled,
    float liftRatio) {
  std::vector<PmsPoorTimelineDescent> result;
  if (orderedTimelines.empty()) {
    return result;
  }

  const double originY =
      geometry.laneOriginY +
      (liftEnabled ? geometry.laneHeight * static_cast<double>(liftRatio)
                   : 0.0);
  // LaneRenderer's no-speed PMS descent uses hu - hl. Lift raises hl while
  // hu stays at the authored lane top, so the fall velocity must use the
  // remaining visible lane height rather than the full authored height.
  const double descentHeight =
      geometry.laneHeight - (originY - geometry.laneOriginY);
  double lowerY = geometry.secondaryDestinationY;
  if (lowerY < -geometry.laneHeight) {
    lowerY = -geometry.laneHeight;
  }
  if (lowerY > originY) {
    lowerY = originY;
  }

  std::size_t nowPosition = orderedTimelines.size() - 1U;
  for (std::size_t index = cursor; index < orderedTimelines.size(); ++index) {
    if (orderedTimelines[index]->timeMicros >= timeMicros) {
      nowPosition = index;
      break;
    }
  }

  constexpr double kQuarterNoteMicros = 240'000'000.0;
  for (std::size_t index = nowPosition;; --index) {
    const auto *timeline = orderedTimelines[index];
    double y = originY;
    const double releaseTime =
        static_cast<double>(timeline->timeMicros + timeline->stopMicros +
                            badTimeMicros);
    if (index + 1U < orderedTimelines.size()) {
      std::size_t segment = index;
      while (segment + 1U < orderedTimelines.size() &&
             orderedTimelines[segment + 1U]->timeMicros < timeMicros) {
        const auto *next = orderedTimelines[segment + 1U];
        if (static_cast<double>(next->timeMicros) > releaseTime) {
          const auto *current = orderedTimelines[segment];
          const double stopTime = std::max(
              releaseTime - static_cast<double>(current->timeMicros) -
                  static_cast<double>(current->stopMicros),
              0.0);
          y -= (static_cast<double>(next->timeMicros) -
                static_cast<double>(current->timeMicros) -
                static_cast<double>(current->stopMicros) - stopTime) *
               descentHeight * current->bpm / kQuarterNoteMicros;
        }
        ++segment;
      }
      const auto *current = orderedTimelines[segment];
      if (current->timeMicros + current->stopMicros < timeMicros &&
          static_cast<double>(timeMicros) > releaseTime) {
        const double stopTime = std::max(
            releaseTime - static_cast<double>(current->timeMicros) -
                static_cast<double>(current->stopMicros),
            0.0);
        y -= (static_cast<double>(timeMicros) -
              static_cast<double>(current->timeMicros) -
              static_cast<double>(current->stopMicros) - stopTime) *
             descentHeight * current->bpm / kQuarterNoteMicros;
      }
    } else if (timeline->timeMicros + timeline->stopMicros < timeMicros &&
               static_cast<double>(timeMicros) > releaseTime) {
      const double stopTime = std::max(
          releaseTime - static_cast<double>(timeline->timeMicros) -
              static_cast<double>(timeline->stopMicros),
          0.0);
      y -= (static_cast<double>(timeMicros) -
            static_cast<double>(timeline->timeMicros) -
            static_cast<double>(timeline->stopMicros) - stopTime) *
           descentHeight * timeline->bpm / kQuarterNoteMicros;
    }
    if (y < lowerY) {
      break;
    }
    result.push_back({.timelineId = timeline->id,
                      .yDisplacement = y - originY});
    if (index == 0U) {
      break;
    }
  }
  return result;
}

bool isVisibleSingleNote(const ChartVisualNote &note,
                         ChartVisualNoteSource source,
                         long long timelineMicros,
                         long long visualTimeMicros,
                         const NotePresentationState *state,
                         const PlayfieldProjectionRequest &request) noexcept {
  if (timelineMicros < visualTimeMicros) {
    return request.showPastNormalNotes &&
           source == ChartVisualNoteSource::Playable &&
           note.kind == ChartVisualNoteKind::Normal && isSourceStateZero(state);
  }
  return source != ChartVisualNoteSource::Invisible ||
         request.includeInvisibleNotes;
}

// The compatibility plan feeds BMSRenderer, whose normal-note loop retains
// playable notes until the active late-POOR deadline. Mines and invisible
// notes keep LaneRenderer's future-only rule. Skin DTOs intentionally retain
// the future-only policy above.
bool isVisibleBuiltInSingleNote(
    const ChartVisualNote &note, ChartVisualNoteSource source,
    long long timelineMicros, long long visualTimeMicros,
    const NotePresentationState *state,
    const PlayfieldProjectionRequest &request) noexcept {
  if (source != ChartVisualNoteSource::Playable) {
    return isVisibleSingleNote(note, source, timelineMicros, visualTimeMicros,
                               state, request);
  }
  return isWithinLatePoorWindow(timelineMicros, visualTimeMicros, request) ||
         (request.showPastNormalNotes &&
          note.kind == ChartVisualNoteKind::Normal && isSourceStateZero(state));
}

std::optional<double>
constantOpacity(long long timelineMicros, long long visualTimeMicros,
                const PlayfieldProjectionRequest &request) noexcept {
  if (!request.constantScroll) {
    return 1.0;
  }
  const long long targetTime =
      visualTimeMicros +
      static_cast<long long>(request.constantDurationMilliseconds) * 1'000LL;
  const long long timeDifference = timelineMicros - targetTime;
  const long long alphaLimit =
      static_cast<long long>(request.constantFadeInMilliseconds) * 1'000LL;
  if (alphaLimit >= 0) {
    if (timelineMicros < targetTime) {
      return 1.0;
    }
    if (timeDifference >= alphaLimit) {
      return std::nullopt;
    }
    return static_cast<double>(alphaLimit - timeDifference) /
           static_cast<double>(alphaLimit);
  }
  if (timelineMicros >= targetTime) {
    return std::nullopt;
  }
  if (timeDifference > alphaLimit) {
    return 1.0 - static_cast<double>(alphaLimit - timeDifference) /
                     static_cast<double>(alphaLimit);
  }
  return 1.0;
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

// This is the value-only form of the gameplay timeline walk. The resulting
// positions are shared by the built-in plan and skin DTO: a skin scales the
// same position into its own lane geometry rather than recomputing a separate
// scroll-position approximation.
struct TimelinePositionWalk {
  std::size_t startIndex = 0;
  std::vector<float> renderYs;
  std::unordered_map<ChartVisualId, std::size_t> renderIndexByTimelineId;
  std::uint32_t nextStartRetainedOrdinal = 0;
};

TimelinePositionWalk walkTimelinePositions(
    const std::vector<const ChartVisualTimeline *> &retainedTimelines,
    const std::optional<BuiltInRendererTraversal> &traversal,
    long long timeMicros, double currentScrollPosition,
    std::int64_t latePoorTimingMicros) {
  TimelinePositionWalk result;
  if (retainedTimelines.empty()) {
    return result;
  }

  if (traversal) {
    const auto start = std::lower_bound(
        retainedTimelines.begin(), retainedTimelines.end(),
        traversal->startRetainedOrdinal,
        [](const ChartVisualTimeline *timeline, std::uint32_t ordinal) {
          return timeline->retainedOrdinal < ordinal;
        });
    result.startIndex = static_cast<std::size_t>(
        std::distance(retainedTimelines.begin(), start));
    result.nextStartRetainedOrdinal = traversal->startRetainedOrdinal;
  }
  if (!traversal) {
    for (std::size_t index = result.startIndex; index < retainedTimelines.size();
         ++index) {
      const auto *timeline = retainedTimelines[index];
      result.renderIndexByTimelineId.emplace(timeline->id,
                                             result.renderYs.size());
      result.renderYs.push_back(std::numeric_limits<float>::quiet_NaN());
      if (timeline->timeMicros < timeMicros - latePoorTimingMicros) {
        result.nextStartRetainedOrdinal = timeline->retainedOrdinal;
      }
    }
    return result;
  }

  double futureY = traversal->judgeY;
  bool futureTraversalStarted = false;
  for (std::size_t index = result.startIndex; index < retainedTimelines.size();
       ++index) {
    const auto *timeline = retainedTimelines[index];
    const bool future = timeline->timeMicros >= timeMicros;
    if (future && futureTraversalStarted &&
        !gameplay_scroll_geometry::futureTimelineTraversalContinues(
            futureY, traversal->upperBound)) {
      break;
    }
    float y = traversal->judgeY;
    if (future) {
      if (index == 0U) {
        y = gameplay_scroll_geometry::initialFutureTimelineY(
            timeline->scrollPosition, currentScrollPosition, traversal->rxhs,
            traversal->judgeY);
      } else {
        const auto *previous = retainedTimelines[index - 1U];
        futureY = gameplay_scroll_geometry::advanceFutureTimelineY(
            futureY, timeline->beat - previous->beat, previous->scrollRate,
            previous->timeMicros, previous->stopMicros, timeline->timeMicros,
            timeMicros, traversal->rxhs);
        y = static_cast<float>(futureY);
      }
      futureTraversalStarted = true;
    } else {
      y = gameplay_scroll_geometry::renderY(
          timeline->scrollPosition, currentScrollPosition, traversal->rxhs,
          traversal->judgeY);
    }
    result.renderIndexByTimelineId.emplace(timeline->id,
                                           result.renderYs.size());
    result.renderYs.push_back(y);
    if (timeline->timeMicros < timeMicros - latePoorTimingMicros) {
      result.nextStartRetainedOrdinal = timeline->retainedOrdinal;
    }
  }
  return result;
}

std::vector<const ChartVisualTimeline *>
orderedProjectionTimelines(const PlayfieldChartVisualModel &model) {
  std::vector<const ChartVisualTimeline *> result;
  result.reserve(model.timelines.size());
  for (const auto &timeline : model.timelines) {
    result.push_back(&timeline);
  }
  std::stable_sort(result.begin(), result.end(),
                   [](const auto *left, const auto *right) {
                     return std::tie(left->authoredOrdinal, left->id) <
                            std::tie(right->authoredOrdinal, right->id);
                   });
  return result;
}

std::vector<const ChartVisualTimeline *>
retainedScrollTimelines(const PlayfieldChartVisualModel &model) {
  std::vector<const ChartVisualTimeline *> result;
  result.reserve(model.timelines.size());
  for (const auto &timeline : model.timelines) {
    if (timeline.retainedForProjection) {
      result.push_back(&timeline);
    }
  }
  std::stable_sort(
      result.begin(), result.end(), [](const auto *left, const auto *right) {
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
  return result;
}

std::vector<gameplay_scroll_geometry::ScrollPositionTimeline>
scrollPositionTimelines(
    std::span<const ChartVisualTimeline *const> orderedTimelines,
    const std::optional<gameplay_scroll_geometry::ScrollPositionTimeline>
        &terminalScrollAnchor) {
  std::vector<gameplay_scroll_geometry::ScrollPositionTimeline> result;
  result.reserve(orderedTimelines.size() + 1U);
  for (const auto *timeline : orderedTimelines) {
    if (!timeline->retainedForProjection) {
      continue;
    }
    result.push_back({.timeMicros = timeline->timeMicros,
                      .scrollPosition = timeline->scrollPosition,
                      .stopMicros = timeline->stopMicros,
                      .bpm = timeline->bpm,
                      .scrollRate = timeline->scrollRate});
  }
  if (terminalScrollAnchor.has_value() &&
      (result.empty() ||
       result.back().timeMicros < terminalScrollAnchor->timeMicros)) {
    result.push_back(*terminalScrollAnchor);
  }
  return result;
}

} // namespace

double scrollPositionAtTime(const PlayfieldChartVisualModel &model,
                            long long timeMicros) {
  const auto orderedTimelines = retainedScrollTimelines(model);
  const auto scrollTimelines = scrollPositionTimelines(
      orderedTimelines, model.terminalScrollAnchor);
  return gameplay_scroll_geometry::scrollPositionAtTime(scrollTimelines,
                                                         timeMicros);
}

void PlayfieldProjection::rebuildIndex(const PlayfieldChartVisualModel &model) {
  index_ = {};
  pmsPoorCursor_ = 0;
  index_.model = &model;
  index_.timelinesById.reserve(model.timelines.size());
  index_.previousTimelinesById.reserve(model.timelines.size());
  index_.retainedTimelinesByOrdinal.reserve(model.timelines.size());
  index_.orderedTimelines.reserve(model.timelines.size());
  for (const auto &timeline : model.timelines) {
    index_.timelinesById.emplace(timeline.id, &timeline);
    index_.orderedTimelines.push_back(&timeline);
  }
  std::stable_sort(index_.orderedTimelines.begin(),
                   index_.orderedTimelines.end(),
                   [](const auto *left, const auto *right) {
                     return std::tie(left->authoredOrdinal, left->id) <
                            std::tie(right->authoredOrdinal, right->id);
                   });
  const ChartVisualTimeline *previousTimeline = nullptr;
  for (const auto *timeline : index_.orderedTimelines) {
    index_.previousTimelinesById.emplace(timeline->id, previousTimeline);
    previousTimeline = timeline;
  }
  index_.retainedTimelines.reserve(index_.orderedTimelines.size());
  for (const auto *timeline : index_.orderedTimelines) {
    if (!timeline->retainedForProjection) {
      continue;
    }
    if (timeline->retainedOrdinal != kNoRetainedTimelineOrdinal) {
      index_.retainedTimelines.push_back(timeline);
      index_.retainedTimelinesByOrdinal.emplace(timeline->retainedOrdinal,
                                                timeline);
    }
  }
  index_.scrollTimelines =
      scrollPositionTimelines(index_.orderedTimelines,
                              model.terminalScrollAnchor);

  index_.notesById.reserve(model.notes.size());
  index_.orderedNotes.reserve(model.notes.size());
  index_.notesByTimeline.reserve(index_.orderedTimelines.size());
  for (const auto &note : model.notes) {
    index_.notesById.emplace(note.id, &note);
    index_.orderedNotes.push_back(&note);
  }
  std::stable_sort(index_.orderedNotes.begin(), index_.orderedNotes.end(),
                   [](const auto *left, const auto *right) {
                     return std::tie(left->authoredOrdinal, left->id) <
                            std::tie(right->authoredOrdinal, right->id);
                   });
  for (const auto *note : index_.orderedNotes) {
    index_.notesByTimeline[note->timelineId].push_back(note);
  }
}

PlayfieldProjectionResult
PlayfieldProjection::project(const PlayfieldChartVisualModel &model,
                             const PlayfieldVisualState &state,
                             const PlayfieldProjectionRequest &request) {
#if defined(ASOBMASHOW_PLAYFIELD_PROJECTION_TESTING)
  PlayfieldProjectionWorkStats workStats;
  const auto publishWorkStats = [this, &workStats]() {
    lastWorkStats_ = workStats;
  };
#endif
  if (index_.model != &model) {
    rebuildIndex(model);
  }
  PlayfieldProjectionResult result;
  result.frameSerial = state.clock.serial;
  result.builtInTraversal = request.builtInTraversal;
  const long long timeMicros = request.noteDisplayTimeMicros.value_or(
      state.clock.visualTimeMicros);
  result.currentScrollPosition = gameplay_scroll_geometry::scrollPositionAtTime(
      index_.scrollTimelines, timeMicros);
  const auto &retainedTimelines = index_.retainedTimelines;
  const TimelinePositionWalk timelinePositionWalk = walkTimelinePositions(
      retainedTimelines, request.builtInTraversal, timeMicros,
      result.currentScrollPosition,
      std::max<std::int64_t>(0, request.latePoorTimingMicros));
  // A selected skin intentionally omits the built-in draw plan, but it still
  // consumes the same LaneRenderer cursor on the next frame.
  result.builtInPlan.nextStartRetainedOrdinal =
      timelinePositionWalk.nextStartRetainedOrdinal;
  const auto visibleInterval = visibleScrollInterval(request);

  const auto &timelines = index_.timelinesById;
  const auto &notes = index_.notesById;
  const auto stateFor = [&state](ChartVisualId id) {
    return state.noteState(id);
  };
  const auto isBeforeVisibleNoteStart =
      [&notes, &request, &timelines](const ChartVisualNote *note) {
        if (!request.minimumVisibleNoteTimeMicros || note == nullptr) {
          return false;
        }
        const auto isBeforeStart = [&timelines, &request](
                                       const ChartVisualNote *candidate) {
          if (candidate == nullptr) {
            return false;
          }
          const auto timeline = timelines.find(candidate->timelineId);
          return timeline != timelines.end() &&
                 timeline->second->timeMicros <
                     *request.minimumVisibleNoteTimeMicros;
        };
        if (isBeforeStart(note)) {
          return true;
        }
        if (note->kind != ChartVisualNoteKind::LongHead &&
            note->kind != ChartVisualNoteKind::LongTail) {
          return false;
        }
        const auto pair = notes.find(note->pairId);
        return pair != notes.end() && isBeforeStart(pair->second);
      };

  gameplay_chart_entity_render_budget::Budget budget;
  const auto reserve = [&result, &budget](std::uint32_t cost) {
    if (!budget.tryConsume(cost)) {
      result.budgetExceeded = true;
      return false;
    }
    return true;
  };

  const auto &orderedTimelines = index_.orderedTimelines;
  const auto &notesByTimeline = index_.notesByTimeline;
  const bool hasPmsPoorDestination = request.pmsPoorDestination.has_value();
  // A captured LaneRenderer traversal is the normal gameplay, replay-watch,
  // and export path. Reuse its bounded retained row walk for every DTO and
  // depth pass. The no-capture fallback preserves the existing absolute
  // scroll behavior for callers that do not have lane geometry.
  const auto forEachProjectionTimeline =
      [&timelinePositionWalk, &retainedTimelines, &orderedTimelines,
       &request](auto &&visit) {
        if (!request.builtInTraversal) {
          for (const auto *timeline : orderedTimelines) {
            visit(timeline);
          }
          return;
        }
        for (std::size_t offset = 0;
             offset < timelinePositionWalk.renderYs.size(); ++offset) {
          visit(retainedTimelines[timelinePositionWalk.startIndex + offset]);
        }
      };
  const auto forEachProjectionNote =
      [&forEachProjectionTimeline, &orderedNotes = index_.orderedNotes,
       &notesByTimeline, &request](auto &&visit) {
        if (!request.builtInTraversal) {
          for (const auto *note : orderedNotes) {
            visit(note);
          }
          return;
        }
        forEachProjectionTimeline([&notesByTimeline, &visit](
                                      const ChartVisualTimeline *timeline) {
          const auto notes = notesByTimeline.find(timeline->id);
          if (notes == notesByTimeline.end()) {
            return;
          }
          for (const auto *note : notes->second) {
            visit(note);
          }
        });
      };
  const auto positionedScrollDelta =
      [&timelinePositionWalk, &retainedTimelines, &request, &result](
          const ChartVisualTimeline &timeline) -> std::optional<double> {
    const double absoluteDelta =
        timeline.scrollPosition - result.currentScrollPosition;
    if (!request.builtInTraversal) {
      return absoluteDelta;
    }
    const auto rendered =
        timelinePositionWalk.renderIndexByTimelineId.find(timeline.id);
    if (rendered == timelinePositionWalk.renderIndexByTimelineId.end()) {
      return std::nullopt;
    }
    const auto &traversal = *request.builtInTraversal;
    const float y = timelinePositionWalk.renderYs[rendered->second];
    if (!std::isfinite(y)) {
      return std::nullopt;
    }
    if (!std::isfinite(traversal.rxhs) ||
        std::abs(traversal.rxhs) <= 0.0001F) {
      // A caller with only a Hi-Speed has no captured lane geometry to
      // normalize the shared y position. Preserve the existing abstract
      // projection in that incomplete-capture case.
      return absoluteDelta;
    }
    return (static_cast<double>(y) - static_cast<double>(traversal.judgeY)) /
           static_cast<double>(traversal.rxhs);
  };

  std::uint32_t lineOrdinal = 1;
  const auto appendLine = [&result, &reserve, &lineOrdinal](
                              const ChartVisualTimeline &timeline,
                              ProjectedLineKind kind, double scrollDelta,
                              double opacity) {
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
                            .opacity = opacity,
                            .submissionOrdinal = lineOrdinal++});
  };

  forEachProjectionTimeline([&](const ChartVisualTimeline *timeline) {
#if defined(ASOBMASHOW_PLAYFIELD_PROJECTION_TESTING)
    ++workStats.timelineRowsExamined;
#endif
    const auto scrollDelta = positionedScrollDelta(*timeline);
    const auto opacity = constantOpacity(timeline->timeMicros, timeMicros,
                                         request);
    if (timeline->retainedForProjection && scrollDelta && opacity &&
        isVisible(*scrollDelta, visibleInterval)) {
      if (request.maxTimelines != 0 &&
          result.timelines.size() >= request.maxTimelines) {
        result.budgetExceeded = true;
      } else {
        result.timelines.push_back(
            {.timelineId = timeline->id,
             .scrollDelta = *scrollDelta,
             .timeMicros = timeline->timeMicros,
             .authoredOrdinal = timeline->authoredOrdinal,
             .retainedOrdinal = timeline->retainedOrdinal,
             .submissionOrdinal = timeline->authoredOrdinal});
        if (timeline->sectionLine) {
          appendLine(*timeline, ProjectedLineKind::Section, *scrollDelta,
                     *opacity);
        }
        const auto previous = index_.previousTimelinesById.find(timeline->id);
        if (request.bpmGuideEnabled &&
            previous != index_.previousTimelinesById.end() &&
            previous->second != nullptr && timeline->bpm != previous->second->bpm) {
          appendLine(*timeline, ProjectedLineKind::BpmChange, *scrollDelta,
                     *opacity);
        }
        if (request.bpmGuideEnabled && timeline->stopMicros > 0) {
          appendLine(*timeline, ProjectedLineKind::Stop, *scrollDelta,
                     *opacity);
        }
      }
    }
  });

  struct BuiltInRowDepths {
    std::optional<gameplay_note_submission_order::LongNoteOrder> longOrder;
    std::optional<std::uint32_t> primaryDepth;
    std::optional<std::uint32_t> invisibleDepth;
  };
  std::unordered_map<ChartVisualId, BuiltInRowDepths> builtInDepths;
  gameplay_note_submission_order::LongNoteOrder pastLongNoteOrder;
  // BMSRenderer always reserves one shared order for long notes whose heads
  // have already passed the retained traversal window.
  if (request.buildBuiltInPlan) {
    builtInDepths.reserve(timelinePositionWalk.renderYs.size());
    gameplay_note_submission_order::Allocator builtInOrder;
    pastLongNoteOrder = builtInOrder.captureLongNote();
    forEachProjectionTimeline([&](const ChartVisualTimeline *timeline) {
#if defined(ASOBMASHOW_PLAYFIELD_PROJECTION_TESTING)
      ++workStats.timelineRowsExamined;
#endif
      const auto rowIt = notesByTimeline.find(timeline->id);
      if (rowIt == notesByTimeline.end()) {
        return;
      }
      const auto rowScrollDelta = positionedScrollDelta(*timeline);
      if (!rowScrollDelta) {
        return;
      }
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
#if defined(ASOBMASHOW_PLAYFIELD_PROJECTION_TESTING)
        ++workStats.noteDescriptorsExamined;
#endif
        if (isBeforeVisibleNoteStart(note)) {
          continue;
        }
        const auto source = effectiveSource(*note);
        const auto *noteState = stateFor(note->id);
        if (source == ChartVisualNoteSource::Invisible) {
          needsInvisibleDepth = needsInvisibleDepth ||
                                (isVisibleSingleNote(
                                     *note, source, timeline->timeMicros,
                                     timeMicros, noteState, request) &&
                                 isVisible(*rowScrollDelta, visibleInterval));
          continue;
        }
        if (source == ChartVisualNoteSource::Mine) {
          needsPrimaryDepth = needsPrimaryDepth ||
                              (isVisibleSingleNote(
                                   *note, source, timeline->timeMicros,
                                   timeMicros, noteState, request) &&
                               isVisible(*rowScrollDelta, visibleInterval));
          continue;
        }
        if (source == ChartVisualNoteSource::Playable &&
            note->kind == ChartVisualNoteKind::LongTail) {
          continue;
        }
        if (source == ChartVisualNoteSource::Playable &&
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
            const auto tailScrollDelta =
                positionedScrollDelta(*tailTimelineIt->second);
            if (!tailScrollDelta) {
              continue;
            }
            needsPrimaryDepth =
                needsPrimaryDepth ||
                (rowIsWithinLatePoorWindow &&
                 isLongIntervalVisible(*rowScrollDelta, *tailScrollDelta,
                                       visibleInterval));
            continue;
          }
        }
        needsPrimaryDepth =
            needsPrimaryDepth ||
            (isVisibleBuiltInSingleNote(*note, source, timeline->timeMicros,
                                        timeMicros, noteState, request) &&
             isVisible(*rowScrollDelta, visibleInterval));
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
    });
  }

  gameplay_note_submission_order::Allocator order;
  std::unordered_set<ChartVisualId> consumedLongEndpoints;
  std::vector<const ChartVisualNote *> candidateLongHeads;
  std::unordered_set<ChartVisualId> candidateLongHeadIds;
  const auto appendCandidateLongHead =
      [&candidateLongHeads, &candidateLongHeadIds,
       &isBeforeVisibleNoteStart](const ChartVisualNote *head) {
        if (head != nullptr && !isBeforeVisibleNoteStart(head) &&
            candidateLongHeadIds.insert(head->id).second) {
          candidateLongHeads.push_back(head);
        }
      };
  std::optional<std::uint32_t> retainedCursorLongNoteBlocker;
  const auto atNoteLimit = [&result, &request]() {
    return request.maxNotes != 0 &&
           result.notes.size() + result.longNotes.size() >= request.maxNotes;
  };
  forEachProjectionNote([&](const ChartVisualNote *note) {
#if defined(ASOBMASHOW_PLAYFIELD_PROJECTION_TESTING)
    ++workStats.noteDescriptorsExamined;
#endif
    if (consumedLongEndpoints.contains(note->id)) {
      return;
    }
    if (isBeforeVisibleNoteStart(note)) {
      return;
    }
    const auto timelineIt = timelines.find(note->timelineId);
    if (timelineIt == timelines.end()) {
      return;
    }
    const auto *timeline = timelineIt->second;
    const auto scrollDelta = positionedScrollDelta(*timeline);
    if (!scrollDelta) {
      return;
    }
    if (std::ranges::find(model.laneOrder, note->lane) ==
        model.laneOrder.end()) {
      return;
    }
    const auto *noteState = stateFor(note->id);
    const auto source = effectiveSource(*note);
    const auto opacity = constantOpacity(timeline->timeMicros, timeMicros,
                                         request);
    if (!opacity) {
      return;
    }

    if (request.buildBuiltInPlan &&
        source == ChartVisualNoteSource::Playable) {
      if (note->kind == ChartVisualNoteKind::LongHead) {
        appendCandidateLongHead(note);
      } else if (note->kind == ChartVisualNoteKind::LongTail) {
        const auto head = notes.find(note->pairId);
        if (head != notes.end()) {
          appendCandidateLongHead(head->second);
        }
      }
    }

    if (source == ChartVisualNoteSource::Playable &&
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
        if (request.builtInTraversal &&
            timeline->timeMicros < timeMicros &&
            tailTimeline->timeMicros >= timeMicros) {
          retainedCursorLongNoteBlocker =
              !retainedCursorLongNoteBlocker
                  ? timeline->retainedOrdinal
                  : std::min(*retainedCursorLongNoteBlocker,
                             timeline->retainedOrdinal);
        }
        auto tailScrollDelta = positionedScrollDelta(*tailTimeline);
        if (!tailScrollDelta && request.builtInTraversal &&
            tailTimeline->timeMicros >= timeMicros) {
          const auto &traversal = *request.builtInTraversal;
          if (std::isfinite(traversal.upperBound) &&
              std::isfinite(traversal.judgeY) &&
              std::isfinite(traversal.rxhs) &&
              std::abs(traversal.rxhs) > 0.0001F) {
            // LaneRenderer draws the long body immediately and its bounded
            // forward walk clips the unresolved tail at the lane's upper
            // edge. Mirror the built-in plan's tail-at-upper-bound fallback
            // for the skin DTO, whose renderer scales this abstract delta
            // into its own lane geometry.
            tailScrollDelta =
                (static_cast<double>(traversal.upperBound) -
                 static_cast<double>(traversal.judgeY)) /
                static_cast<double>(traversal.rxhs);
          }
        }
        if (!tailScrollDelta ||
            !isLongIntervalVisible(*scrollDelta, *tailScrollDelta,
                                   visibleInterval)) {
          return;
        }
        if (atNoteLimit()) {
          result.budgetExceeded = true;
          return;
        }
        if (!reserve(gameplay_chart_entity_render_budget::
                         kLongNoteReservationCost)) {
          return;
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
             .headScrollDelta = *scrollDelta,
             .tailScrollDelta = *tailScrollDelta,
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
             .opacity = *opacity,
             .submissionOrdinal = order.next(),
             .bodyDepth = builtInLongOrder.bodyDepth,
             .endpointDepth = builtInLongOrder.endpointDepth});
        consumedLongEndpoints.insert(note->id);
        consumedLongEndpoints.insert(tail->id);
        return;
      }
    }

    if (!isVisible(*scrollDelta, visibleInterval)) {
      return;
    }

    if (hasPmsPoorDestination &&
        effectiveSource(*note) == ChartVisualNoteSource::Playable &&
        note->kind == ChartVisualNoteKind::Normal &&
        (timeline->timeMicros < timeMicros ||
         !isPmsPoorEligibleNormal(*note, source, noteState))) {
      return;
    }

    if (!isVisibleSingleNote(*note, source, timeline->timeMicros, timeMicros,
                             noteState, request)) {
      return;
    }
    if (atNoteLimit()) {
      result.budgetExceeded = true;
      return;
    }
    if (!reserve(
            gameplay_chart_entity_render_budget::kSingleRectangleEntityCost)) {
      return;
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
         .scrollDelta = *scrollDelta,
         .timeMicros = timeline->timeMicros,
         .authoredOrdinal = note->authoredOrdinal,
         .retainedTimelineOrdinal = timeline->retainedOrdinal,
         .judged = noteState != nullptr && noteState->judged,
         .opacity = *opacity,
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
  });

  // LaneRenderer resets its sprite before the primary note pass, then leaves
  // that pass's last Constant color in place for the following PMS dst2
  // descent.  This is intentionally not the individual falling note's
  // Constant value: it is the final drawable row's inherited sprite state.
  double pmsPoorOpacity = 1.0;
  if (hasPmsPoorDestination && request.constantScroll) {
    forEachProjectionTimeline([&](const ChartVisualTimeline *timeline) {
      if (timeline->timeMicros < timeMicros) {
        return;
      }
      if (const auto opacity =
              constantOpacity(timeline->timeMicros, timeMicros, request)) {
        pmsPoorOpacity = *opacity;
      }
    });
  }

  if (hasPmsPoorDestination) {
    advancePmsPoorCursor(pmsPoorCursor_, orderedTimelines, notesByTimeline,
                          notes, timelines, state, timeMicros,
                          request.showPastNormalNotes);
    const auto descents = pmsPoorTimelineDescents(
        orderedTimelines, pmsPoorCursor_, timeMicros,
        std::max<std::int64_t>(0, request.latePoorTimingMicros),
        *request.pmsPoorDestination, state.authority.liftEnabled,
        state.authority.liftRatio);
    for (const auto &descent : descents) {
      const auto row = notesByTimeline.find(descent.timelineId);
      const auto timeline = timelines.find(descent.timelineId);
      if (row == notesByTimeline.end() || timeline == timelines.end()) {
        continue;
      }
      for (const auto *note : row->second) {
        if (isBeforeVisibleNoteStart(note)) {
          continue;
        }
        const auto source = effectiveSource(*note);
        const auto *noteState = stateFor(note->id);
        if (timeline->second->timeMicros > timeMicros ||
            !isPmsPoorEligibleNormal(*note, source, noteState)) {
          continue;
        }
        if (atNoteLimit()) {
          result.budgetExceeded = true;
          break;
        }
        if (!reserve(
                gameplay_chart_entity_render_budget::kSingleRectangleEntityCost)) {
          break;
        }
        result.notes.push_back(
            {.noteId = note->id,
             .timelineId = timeline->second->id,
             .pairId = note->pairId,
             .lane = note->lane,
             .kind = note->kind,
             .source = source,
             .longNoteMode = note->longNoteMode,
             .mineDamage = note->mineDamage,
             .scrollDelta = 0.0,
             .timeMicros = timeline->second->timeMicros,
             .authoredOrdinal = note->authoredOrdinal,
             .retainedTimelineOrdinal = timeline->second->retainedOrdinal,
             .judged = noteState != nullptr && noteState->judged,
             .opacity = pmsPoorOpacity,
             .pmsPoorYDisplacement = descent.yDisplacement,
             .submissionOrdinal = order.next()});
      }
    }
  }

  if (retainedCursorLongNoteBlocker) {
    result.builtInPlan.nextStartRetainedOrdinal = std::min(
        result.builtInPlan.nextStartRetainedOrdinal,
        *retainedCursorLongNoteBlocker);
  }

  if (!request.buildBuiltInPlan) {
#if defined(ASOBMASHOW_PLAYFIELD_PROJECTION_TESTING)
    publishWorkStats();
#endif
    return result;
  }

  // The built-in execution plan consumes the common position snapshot used to
  // place skin DTOs above.
  auto &builtInPlan = result.builtInPlan;
  std::unordered_map<ChartVisualId, float> builtInTimelineY;
  builtInTimelineY.reserve(timelinePositionWalk.renderYs.size());
  for (std::size_t offset = 0;
       offset < timelinePositionWalk.renderYs.size(); ++offset) {
    const auto *timeline =
        retainedTimelines[timelinePositionWalk.startIndex + offset];
    const float y = timelinePositionWalk.renderYs[offset];
    builtInPlan.traversedTimelineOrdinals.push_back(timeline->retainedOrdinal);
    builtInPlan.timelines.push_back(
        {.retainedOrdinal = timeline->retainedOrdinal,
         .timeMicros = timeline->timeMicros,
         .renderY = y,
         .future = timeline->timeMicros >= timeMicros});
    builtInTimelineY.emplace(timeline->id, y);
  }
  std::unordered_set<std::uint32_t> builtInTraversedOrdinals(
      builtInPlan.traversedTimelineOrdinals.begin(),
      builtInPlan.traversedTimelineOrdinals.end());
  const auto builtInTimelineWasTraversed =
      [&builtInTraversedOrdinals](std::uint32_t ordinal) {
        return builtInTraversedOrdinals.contains(ordinal);
      };
  const auto appendBuiltInNote = [&](const ChartVisualNote &note,
                                     const ChartVisualTimeline &timeline) {
    if (isBeforeVisibleNoteStart(&note)) {
      return;
    }
    if (note.kind == ChartVisualNoteKind::LongHead ||
        note.kind == ChartVisualNoteKind::LongTail) {
      return;
    }
    const auto source = effectiveSource(note);
    const auto *noteState = stateFor(note.id);
    if (!isVisibleBuiltInSingleNote(note, source, timeline.timeMicros,
                                    timeMicros, noteState, request)) {
      return;
    }
    const auto lane = std::ranges::find(model.laneOrder, note.lane);
    if (lane == model.laneOrder.end()) {
      return;
    }
    const auto depth = builtInDepths.find(note.timelineId);
    builtInPlan.notes.push_back(
        {.noteId = note.id,
         .timelineId = timeline.id,
         .pairId = note.pairId,
         .lane = static_cast<int>(lane - model.laneOrder.begin()),
         .kind = note.kind,
         .source = source,
         .longNoteMode = note.longNoteMode,
         .mineDamage = note.mineDamage,
         .scrollDelta = timeline.scrollPosition - result.currentScrollPosition,
         .timeMicros = timeline.timeMicros,
         .authoredOrdinal = note.authoredOrdinal,
         .retainedTimelineOrdinal = timeline.retainedOrdinal,
         .judged = noteState != nullptr && noteState->judged,
         .submissionOrdinal =
             static_cast<std::uint32_t>(builtInPlan.entries.size()),
         .builtInDepth = depth == builtInDepths.end() ? 0U
                         : source == ChartVisualNoteSource::Invisible
                             ? depth->second.invisibleDepth.value_or(0U)
                             : depth->second.primaryDepth.value_or(0U)});
    builtInPlan.entries.push_back(
        {.kind = BuiltInRendererPlanEntryKind::Note,
         .descriptorIndex =
             static_cast<std::uint32_t>(builtInPlan.notes.size() - 1U),
         .renderY = request.builtInTraversal.has_value()
                        ? builtInTimelineY.at(timeline.id)
                        : std::numeric_limits<float>::quiet_NaN()});
  };
  for (const auto &planTimeline : builtInPlan.timelines) {
    const auto timelineIt =
        index_.retainedTimelinesByOrdinal.find(planTimeline.retainedOrdinal);
    if (timelineIt == index_.retainedTimelinesByOrdinal.end()) {
      continue;
    }
    const auto *timeline = timelineIt->second;
    if (timeline->sectionLine && timeline->timeMicros >= timeMicros) {
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
           .scrollDelta =
               timeline->scrollPosition - result.currentScrollPosition,
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
    const auto notesIt = notesByTimeline.find(timeline->id);
    if (notesIt == notesByTimeline.end()) {
      continue;
    }
    for (const auto *note : notesIt->second) {
      appendBuiltInNote(*note, *timeline);
    }
  }
  for (const auto *head : candidateLongHeads) {
#if defined(ASOBMASHOW_PLAYFIELD_PROJECTION_TESTING)
    ++workStats.longHeadsExamined;
#endif
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
    if (isBeforeVisibleNoteStart(head)) {
      continue;
    }
    const auto *tailTimeline = tailTimelineIt->second;
    const bool headTraversed =
        builtInTimelineWasTraversed(headTimeline->retainedOrdinal);
    const bool tailTraversed =
        builtInTimelineWasTraversed(tailTimeline->retainedOrdinal);
    const bool spansDerivedStart =
        headTimeline->timeMicros <
            timeMicros -
                std::max<std::int64_t>(0, request.latePoorTimingMicros) &&
        tailTimeline->timeMicros >=
            timeMicros -
                std::max<std::int64_t>(0, request.latePoorTimingMicros);
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
         .headJudged =
             headState != nullptr && (headState->judged || headState->dead),
         .tailJudged =
             tailState != nullptr && (tailState->judged || tailState->dead),
         .headDead = headState != nullptr && headState->dead,
         .tailDead = tailState != nullptr && tailState->dead,
         .headPlayedTimeMicros = headState != nullptr
                                     ? headState->playedTimeMicros
                                     : kPlayfieldTimestampOff,
         .tailPlayedTimeMicros = tailState != nullptr
                                     ? tailState->playedTimeMicros
                                     : kPlayfieldTimestampOff,
         .tailReleasedEarly = tailReleasedEarly,
         .tailMissedWithHead = headState != nullptr && headState->dead &&
                               !(tailState != nullptr && tailState->dead) &&
                               tailReleasedEarly,
         .tailResolvedAtOrAfterTiming =
             tailState != nullptr && tailState->judged &&
             tailState->playedTimeMicros != kPlayfieldTimestampOff &&
             tailState->playedTimeMicros >= tailTimeline->timeMicros,
         .submissionOrdinal =
             static_cast<std::uint32_t>(builtInPlan.entries.size()),
         .bodyDepth = longOrder.bodyDepth,
         .endpointDepth = longOrder.endpointDepth});
    const float headY = request.builtInTraversal.has_value()
                            ? (!headTraversed || !isWithinLatePoorWindow(
                                                     headTimeline->timeMicros,
                                                     timeMicros, request)
                                   ? request.builtInTraversal->lowerBound
                                   : builtInTimelineY.at(headTimeline->id))
                            : std::numeric_limits<float>::quiet_NaN();
    const float tailY =
        request.builtInTraversal.has_value()
            ? (tailTraversed ? builtInTimelineY.at(tailTimeline->id)
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
          startOrdinal != 0U && longNote.headRetainedOrdinal <= startOrdinal &&
          !isWithinLatePoorWindow(longNote.headTimeMicros, timeMicros, request);
      needsPrimary = needsPrimary || !isCursorCarriedPastHead;
    }
    for (const auto &note : builtInPlan.notes) {
      if (note.retainedTimelineOrdinal != ordinal) {
        continue;
      }
      if (effectiveSource(
              ChartVisualNote{.kind = note.kind, .source = note.source}) ==
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
    note.builtInDepth = effectiveSource(ChartVisualNote{
                            .kind = note.kind, .source = note.source}) ==
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
        startOrdinal != 0U && longNote.headRetainedOrdinal <= startOrdinal &&
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
      return std::pair{
          4U,
          std::numeric_limits<unsigned>::max() -
              builtInPlan.longNotes[entry.descriptorIndex].headRetainedOrdinal};
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
    const int rawLane =
        projectedLane >= 0 &&
                static_cast<std::size_t>(projectedLane) < model.laneOrder.size()
            ? model.laneOrder[static_cast<std::size_t>(projectedLane)]
            : projectedLane;
    const auto &laneOrder =
        request.builtInTraversal.has_value() &&
                !request.builtInTraversal->playableLaneOrder.empty()
            ? request.builtInTraversal->playableLaneOrder
            : model.laneOrder;
    const auto found = std::ranges::find(laneOrder, rawLane);
    return std::pair{
        1U, static_cast<unsigned>(std::distance(laneOrder.begin(), found))};
  };
  std::stable_sort(builtInPlan.entries.begin(), builtInPlan.entries.end(),
                   [&entryTimelineOrdinal,
                   &lanePhaseAndOrder](const auto &left, const auto &right) {
                     const auto leftTimeline = entryTimelineOrdinal(left);
                     const auto rightTimeline = entryTimelineOrdinal(right);
                     if (leftTimeline != rightTimeline) {
                       return leftTimeline < rightTimeline;
                     }
                     return lanePhaseAndOrder(left) < lanePhaseAndOrder(right);
                   });
#if defined(ASOBMASHOW_PLAYFIELD_PROJECTION_TESTING)
  publishWorkStats();
#endif
  return result;
}

void PlayfieldProjection::reset() noexcept {
  index_ = {};
  pmsPoorCursor_ = 0;
}

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
    // The projected descriptor carries both the canonical chart kind and the
    // source family.  Preserve the chart's Mine/Invisible meaning when a
    // caller leaves the optional source at its ordinary playable default.
    // This is the same normalization used while projecting ChartVisualNote.
    const auto source = effectiveSource(
        ChartVisualNote{.kind = note.kind, .source = note.source});
    result.notes.push_back({.visualId = note.noteId,
                            .lane = note.lane,
                            .kind = toSkinNoteKind(source),
                            .scrollSpeed = scrollSpeed,
                            .authoredYDisplacement = note.scrollDelta,
                            .pmsPoorYDisplacement = note.pmsPoorYDisplacement,
                            .judged = note.judged,
                            .opacity = note.opacity,
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
         .opacity = longNote.opacity,
         .submissionOrdinal = longNote.submissionOrdinal});
  }
  for (const auto &line : projection.lines) {
    result.lines.push_back({.timelineVisualId = line.timelineId,
                            .kind = toSkinLineKind(line.kind),
                            .scrollSpeed = scrollSpeed,
                            .authoredYDisplacement = line.scrollDelta,
                            .opacity = line.opacity,
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
  const auto orderedTimelines = retainedScrollTimelines(model);
  std::vector<long long> timelineTimes;
  timelineTimes.reserve(orderedTimelines.size());
  for (const auto *timeline : orderedTimelines) {
    timelineTimes.push_back(timeline->timeMicros);
  }
  std::ranges::sort(timelineTimes);
  timelineTimes.erase(std::unique(timelineTimes.begin(), timelineTimes.end()),
                      timelineTimes.end());
  const auto scrollTimelines = scrollPositionTimelines(
      orderedTimelines, model.terminalScrollAnchor);
  return detail::buildReplayGhostEvents(
      replayData,
      [&playableLanes](int lane) {
        return playableLanes.find(lane) != playableLanes.end();
      },
      [&timelineTimes](long long time) {
        return std::binary_search(timelineTimes.begin(), timelineTimes.end(),
                                  time);
      },
      [&scrollTimelines](long long time) {
        return gameplay_scroll_geometry::scrollPositionAtTime(scrollTimelines,
                                                               time);
      });
}

} // namespace replay_ghost
