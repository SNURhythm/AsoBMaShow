#include "ReplayPlayfieldPresentation.h"

#include "BuiltInPlayfieldPresentation.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

PlayfieldProjectionRequest initialProjectionRequest(
    const PlayfieldPresentationConfig &configuration,
    const BMSRenderer &renderer) {
  return {
      .includeInvisibleNotes = configuration.showInvisibleNotes,
      .latePoorTimingMicros = renderer.projectionLatePoorTimingMicros(),
      .builtInTraversal = renderer.projectionTraversal(),
  };
}

bool isLongNote(const ChartVisualNote &note) noexcept {
  return note.kind == ChartVisualNoteKind::LongHead ||
         note.kind == ChartVisualNoteKind::LongTail;
}

bool isClassicLongNote(const ChartVisualNote &note) noexcept {
  return note.longNoteMode == ChartLongNoteMode::LN;
}

} // namespace

ReplayPlayfieldPresentation::ReplayPlayfieldPresentation(
    std::unique_ptr<PlayfieldChartVisualModel> chartModel,
    std::unique_ptr<PlayfieldVisualStateStore> state,
    std::unique_ptr<PlayfieldProjection> projection,
    std::unique_ptr<PlayfieldPresentationCoordinator> coordinator,
    BMSRenderer *builtIn, PlayfieldAuthorityUpdate authority)
    : chartModel_(std::move(chartModel)), state_(std::move(state)),
      projection_(std::move(projection)), coordinator_(std::move(coordinator)),
      builtIn_(builtIn), authority_(std::move(authority)) {
  if (coordinator_ == nullptr || builtIn_ == nullptr) {
    throw std::invalid_argument(
        "ReplayPlayfieldPresentation requires a coordinator and BMSRenderer");
  }
  for (const auto &note : chartModel_->notes) {
    noteStates_.emplace(note.id, NotePresentationState{.id = note.id});
    if (note.kind == ChartVisualNoteKind::LongTail &&
        note.source == ChartVisualNoteSource::Playable &&
        isClassicLongNote(note)) {
      classicLongTailIds_.push_back(note.id);
    }
  }
  std::sort(classicLongTailIds_.begin(), classicLongTailIds_.end(),
            [this](ChartVisualId leftId, ChartVisualId rightId) {
              const auto left = std::ranges::find(
                  chartModel_->notes, leftId, &ChartVisualNote::id);
              const auto right = std::ranges::find(
                  chartModel_->notes, rightId, &ChartVisualNote::id);
              const auto timelineTime = [this](const ChartVisualNote &note) {
                return std::ranges::find(chartModel_->timelines,
                                         note.timelineId,
                                         &ChartVisualTimeline::id)
                    ->timeMicros;
              };
              const long long leftTime = timelineTime(*left);
              const long long rightTime = timelineTime(*right);
              return leftTime == rightTime ? left->lane < right->lane
                                           : leftTime < rightTime;
            });
  events_ = std::make_unique<PlayfieldPresentationEventFanout>(*state_,
                                                                 *coordinator_);
}

ReplayPlayfieldPresentationCreateResult ReplayPlayfieldPresentation::create(
    ReplayPlayfieldPresentationCreateInfo creation) {
  // The exporter supplies the chart after applying its replay-compatible long
  // note mode, so the chart metadata is the one immutable model authority.
  auto model = std::make_unique<PlayfieldChartVisualModel>(
      buildPlayfieldChartVisualModel(creation.chart, creation.chart.Meta.LnMode));
  auto state = std::make_unique<PlayfieldVisualStateStore>(*model);
  state->setConfiguration(creation.configuration);
  state->setReplayTouchSamples(creation.replayTouchSamples);

  auto builtIn = createBuiltInPlayfieldPresentation({
      .chart = creation.chart,
      .timingWindows = std::move(creation.timingWindows),
      .visibleTimeGreenNumber = creation.configuration.visibleTimeGreenNumber,
      .renderHud = true,
      .playbackRate = creation.playback,
      .replayData = creation.replayData,
  });
  auto *renderer = dynamic_cast<BMSRenderer *>(builtIn.get());
  if (renderer == nullptr) {
    throw std::logic_error("built-in replay presentation is not BMSRenderer");
  }
  builtIn->configure(creation.configuration);

  const PlayfieldFrameClock initialClock =
      creation.skinInput.initialState != nullptr
          ? creation.skinInput.initialState->clock
          : PlayfieldFrameClock{};
  const PlayfieldVisualState initialState = state->capture(initialClock);
  auto projection = std::make_unique<PlayfieldProjection>();
  const PlayfieldProjectionResult initialProjection = projection->project(
      *model, initialState, initialProjectionRequest(creation.configuration,
                                                     *renderer));

  creation.skinInput.keyMode = creation.chart.Meta.KeyMode;
  creation.skinInput.chartModel = model.get();
  creation.skinInput.initialState = &initialState;
  creation.skinInput.initialProjection = &initialProjection;

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  GameplaySkinSessionResult session = createGameplaySkinSession(
      std::move(creation.skinServices), creation.skinInput);
  if (session.disposition == GameplaySkinSessionDisposition::Failed) {
    if (session.failure && creation.recordFailure) {
      creation.recordFailure(*session.failure);
    }
    return {.presentation = {}, .failure = std::move(session.failure)};
  }
#else
  (void)creation.skinServices;
#endif

  auto coordinator = std::make_unique<PlayfieldPresentationCoordinator>(
      PlayfieldPresentationCoordinatorDependencies{
          .builtIn = std::move(builtIn),
          .skin = {},
          .bga = creation.bga,
          .persistViewport = {},
          .recordFailure = std::move(creation.recordFailure),
          .allowBuiltInFallback = false,
      });
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (session.disposition == GameplaySkinSessionDisposition::Ready) {
    coordinator->installSkinSession(std::move(session.session));
  }
#endif
  coordinator->configure(creation.configuration);

  return {.presentation = std::unique_ptr<ReplayPlayfieldPresentation>(
              new ReplayPlayfieldPresentation(std::move(model),
                                              std::move(state),
                                              std::move(projection),
                                              std::move(coordinator), renderer,
                                              {})),
          .failure = std::nullopt};
}

void ReplayPlayfieldPresentation::applyAuthorityUpdate(
    const PlayfieldAuthorityUpdate &authority) {
  authority_ = authority;
  state_->applyAuthorityUpdate(authority_);
}

const ChartVisualNote *
ReplayPlayfieldPresentation::replayNote(const ReplayEvent &event) const {
  if (event.noteTimeMicros < 0) {
    return nullptr;
  }
  const auto timeline = std::ranges::find_if(
      chartModel_->timelines, [&event](const ChartVisualTimeline &value) {
        return value.timeMicros == event.noteTimeMicros;
      });
  if (timeline == chartModel_->timelines.end()) {
    return nullptr;
  }
  const auto note = std::ranges::find_if(
      chartModel_->notes, [&event, timeline](const ChartVisualNote &value) {
        return value.timelineId == timeline->id && value.lane == event.lane &&
               value.source == ChartVisualNoteSource::Playable;
      });
  return note == chartModel_->notes.end() ? nullptr : &*note;
}

NotePresentationState *
ReplayPlayfieldPresentation::noteState(ChartVisualId id) noexcept {
  const auto it = noteStates_.find(id);
  return it == noteStates_.end() ? nullptr : &it->second;
}

void ReplayPlayfieldPresentation::publishNoteState(ChartVisualId id) {
  if (const auto *current = noteState(id); current != nullptr) {
    state_->setNoteState(*current);
  }
}

void ReplayPlayfieldPresentation::setReplayGauge(const ReplayEvent &event) {
  authority_.gaugeType = event.gaugeType;
  authority_.currentGauge = event.gauge;
  state_->applyAuthorityUpdate(authority_);
}

void ReplayPlayfieldPresentation::updateLongVisualState(
    const ChartVisualNote &note) {
  if (!isLongNote(note)) {
    return;
  }
  const auto *self = noteState(note.id);
  const auto *paired = noteState(note.pairId);
  const bool active = (self != nullptr && self->longActive) ||
                      (paired != nullptr && paired->longActive);
  if (auto *current = noteState(note.id); current != nullptr) {
    current->longActive = active;
    publishNoteState(note.id);
  }
  if (auto *pairedState = noteState(note.pairId); pairedState != nullptr) {
    pairedState->longActive = active;
    publishNoteState(note.pairId);
  }
}

void ReplayPlayfieldPresentation::markReplayMissedNote(
    const ChartVisualNote &note, long long judgedTimeMicros) {
  auto *current = noteState(note.id);
  if (current == nullptr) {
    return;
  }
  current->judged = true;
  current->playedTimeMicros = judgedTimeMicros;
  if (!isLongNote(note)) {
    current->dead = true;
    publishNoteState(note.id);
    return;
  }

  // This is the visual equivalent of the export reducer's
  // longNoteTailJudgedBeforeTiming()/markReplayMissedNote().  A tail missed
  // before its authored time stays non-dead; both endpoints lose holding.
  const bool tailJudgedBeforeTiming =
      note.kind == ChartVisualNoteKind::LongTail &&
      judgedTimeMicros < std::ranges::find(chartModel_->timelines, note.timelineId,
                                            &ChartVisualTimeline::id)
                             ->timeMicros;
  current->dead = !tailJudgedBeforeTiming;
  current->longActive = false;
  publishNoteState(note.id);
  if (auto *paired = noteState(note.pairId); paired != nullptr) {
    paired->longActive = false;
    publishNoteState(note.pairId);
  }
}

bool ReplayPlayfieldPresentation::applyReplayEvent(
    const ReplayEvent &event, const PlayfieldJudgeEventClock &clock,
    bool /*recordTimingSample*/) {
  const JudgeResult recordedJudge(event.judgement, event.diffMicros);
  const auto applyHud = [&]() -> bool {
    if (event.judgement == None) {
      return false;
    }
    events_->onJudge(recordedJudge, event.combo, event.score, clock,
                     event.action != ReplayEventAction::Miss);
    setReplayGauge(event);
    return true;
  };

  switch (event.action) {
  case ReplayEventAction::MultiBad: {
    if (const auto *note = replayNote(event); note != nullptr) {
      if (auto *current = noteState(note->id); current != nullptr) {
        current->judged = true;
        current->playedTimeMicros = event.judgeTimeMicros;
        publishNoteState(note->id);
      }
      if (note->kind == ChartVisualNoteKind::LongHead) {
        if (auto *tail = noteState(note->pairId);
            tail != nullptr && !tail->judged) {
          tail->judged = true;
          tail->playedTimeMicros = event.judgeTimeMicros;
          publishNoteState(note->pairId);
        }
      }
    }
    return applyHud();
  }
  case ReplayEventAction::Press: {
    bool suppressHudForLongNoteHead = false;
    if (const auto *note = replayNote(event); note != nullptr) {
      if (isLongNote(*note)) {
        suppressHudForLongNoteHead =
            note->kind == ChartVisualNoteKind::LongHead &&
            recordedJudge.isNotePlayed() && isClassicLongNote(*note);
        if (recordedJudge.isNotePlayed() &&
            note->kind == ChartVisualNoteKind::LongHead) {
          if (auto *current = noteState(note->id); current != nullptr) {
            current->judged = true;
            current->playedTimeMicros = event.judgeTimeMicros;
            current->longActive = true;
            publishNoteState(note->id);
            updateLongVisualState(*note);
          }
        }
      } else if (recordedJudge.isNotePlayed()) {
        if (auto *current = noteState(note->id); current != nullptr) {
          current->judged = true;
          current->playedTimeMicros = event.judgeTimeMicros;
          publishNoteState(note->id);
        }
      }
    }
    if (!suppressHudForLongNoteHead) {
      const bool appliedHud = applyHud();
      events_->onLanePressed(event.lane, recordedJudge,
                             clock.visualTimeMicros);
      return appliedHud;
    }
    events_->onLanePressed(event.lane, recordedJudge, clock.visualTimeMicros);
    return false;
  }
  case ReplayEventAction::Release: {
    if (const auto *note = replayNote(event);
        note != nullptr && isLongNote(*note) && event.judgement != None &&
        note->kind == ChartVisualNoteKind::LongTail) {
      if (auto *current = noteState(note->id);
          current != nullptr && current->longActive) {
        current->judged = true;
        current->playedTimeMicros = event.judgeTimeMicros;
        current->longActive = false;
        publishNoteState(note->id);
        updateLongVisualState(*note);
      }
    }
    const bool appliedHud = applyHud();
    events_->onLaneReleased(event.lane, clock.visualTimeMicros);
    return appliedHud;
  }
  case ReplayEventAction::Miss:
    if (const auto *note = replayNote(event); note != nullptr) {
      markReplayMissedNote(*note, event.judgeTimeMicros);
    }
    return applyHud();
  case ReplayEventAction::Mine:
    if (const auto *note = replayNote(event); note != nullptr) {
      if (auto *current = noteState(note->id); current != nullptr) {
        current->judged = true;
        current->dead = true;
        current->playedTimeMicros = event.judgeTimeMicros;
        publishNoteState(note->id);
      }
    }
    setReplayGauge(event);
    return false;
  case ReplayEventAction::Gauge:
    setReplayGauge(event);
    return false;
  }
  return false;
}

void ReplayPlayfieldPresentation::releaseDueClassicLongNoteTails(
    long long gameplayTimeMicros) {
  while (classicLongTailCursor_ < classicLongTailIds_.size()) {
    const ChartVisualId tailId = classicLongTailIds_[classicLongTailCursor_];
    const auto note = std::ranges::find(chartModel_->notes, tailId,
                                        &ChartVisualNote::id);
    const auto timeline = std::ranges::find(chartModel_->timelines,
                                            note->timelineId,
                                            &ChartVisualTimeline::id);
    if (timeline->timeMicros > gameplayTimeMicros) {
      break;
    }
    if (auto *tail = noteState(tailId);
        tail != nullptr && !tail->judged && tail->longActive) {
      // This is the visual-state equivalent of LongNote::Release() in the
      // legacy exporter: the tail is played at its authored time and both
      // endpoints stop holding without an extra replay judgement.
      tail->judged = true;
      tail->playedTimeMicros = timeline->timeMicros;
      tail->longActive = false;
      publishNoteState(tailId);
      if (auto *head = noteState(note->pairId); head != nullptr) {
        head->longActive = false;
        publishNoteState(note->pairId);
      }
    }
    ++classicLongTailCursor_;
  }
}

PresentationFrameResult ReplayPlayfieldPresentation::renderFrame(
    RenderContext &context, PlayfieldFrameClock clock,
    const PlayfieldProjectionRequest &request) {
  state_->applyAuthorityUpdate(authority_);
  PlayfieldVisualState state = state_->capture(clock);
  PlayfieldProjectionRequest effectiveRequest = request;
  if (!effectiveRequest.builtInTraversal) {
    effectiveRequest.builtInTraversal = builtIn_->projectionTraversal();
  }
  if (effectiveRequest.latePoorTimingMicros == 0) {
    effectiveRequest.latePoorTimingMicros =
        builtIn_->projectionLatePoorTimingMicros();
  }
  const PlayfieldProjectionResult projection =
      projection_->project(*chartModel_, state, effectiveRequest);
  (void)coordinator_->prepareFrame(state, projection);
  return coordinator_->render(context);
}

BMSRenderer &ReplayPlayfieldPresentation::builtInRenderer() noexcept {
  return *builtIn_;
}

#if defined(ASOBMASHOW_REPLAY_PLAYFIELD_PRESENTATION_TESTING)
PlayfieldVisualState ReplayPlayfieldPresentation::captureVisualStateForTesting(
    PlayfieldFrameClock clock) const {
  return state_->capture(clock);
}
#endif
