#include "ReplayPlayfieldPresentation.h"

#include "BeatorajaHiSpeedChart.h"

#include "BuiltInPlayfieldPresentation.h"
#include "../../ReplayGhostUtils.h"

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
      .showPastNormalNotes = configuration.showPastNotes,
      .constantScroll = configuration.constantScroll,
      .constantDurationMilliseconds =
          configuration.visibleTimeDurationMilliseconds,
      .constantFadeInMilliseconds = configuration.constantFadeInMilliseconds,
      .bpmGuideEnabled = configuration.bpmGuideEnabled,
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

std::array<SkinJudgeWindow, 5> replaySkinJudgeWindows(
    const std::map<Judgement, std::pair<long long, long long>> &windows) {
  constexpr std::array<Judgement, 5> order = {
      PGreat, Great, Good, Bad, Kpoor};
  std::array<SkinJudgeWindow, 5> result{};
  for (std::size_t index = 0; index < order.size(); ++index) {
    const Judgement judgement = order[index];
    const auto found = windows.find(judgement);
    result[index] =
        found == windows.end()
            ? SkinJudgeWindow{.judgement = judgement}
            : SkinJudgeWindow{
                  .judgement = judgement,
                  .minimumTimingMillis =
                      -static_cast<int>(found->second.second / 1000),
                  .maximumTimingMillis =
                      -static_cast<int>(found->second.first / 1000)};
  }
  return result;
}

} // namespace

ReplayPlayfieldPresentation::ReplayPlayfieldPresentation(
    std::unique_ptr<PlayfieldChartVisualModel> chartModel,
    std::unique_ptr<PlayfieldVisualStateStore> state,
    std::unique_ptr<PlayfieldProjection> projection,
    std::unique_ptr<PlayfieldPresentationCoordinator> coordinator,
    BMSRenderer *builtIn, PlayfieldAuthorityUpdate authority,
    PlayfieldPresentationConfig configuration, gameplay_hispeed::State hispeed,
    std::optional<skin::RuntimeSkinConfigurationSelection>
        runtimeSkinConfigurationSelection,
    std::array<SkinJudgeWindow, 5> judgeWindows,
    std::size_t gaugeHistoryCapacity)
    : chartModel_(std::move(chartModel)), state_(std::move(state)),
      projection_(std::move(projection)), coordinator_(std::move(coordinator)),
      builtIn_(builtIn), authority_(std::move(authority)),
      configuration_(std::move(configuration)), hispeed_(std::move(hispeed)),
      runtimeSkinConfigurationSelection_(
          std::move(runtimeSkinConfigurationSelection)) {
  if (coordinator_ == nullptr || builtIn_ == nullptr) {
    throw std::invalid_argument(
        "ReplayPlayfieldPresentation requires a coordinator and BMSRenderer");
  }
  skinGameplayGraph_.reset(
      chartModel_->skinGameplayGraph.judgementNotes,
      chartModel_->skinGameplayGraph.judgementDistributionSeconds, judgeWindows,
      gaugeHistoryCapacity);
  state_->applyGameplayGraphState(skinGameplayGraph_.state());
  timelineTimeById_.reserve(chartModel_->timelines.size());
  for (const auto &timeline : chartModel_->timelines) {
    timelineTimeById_.emplace(timeline.id, timeline.timeMicros);
  }
  notesById_.reserve(chartModel_->notes.size());
  replayNotesByTimeLaneAndSource_.reserve(chartModel_->notes.size());
  for (const auto &note : chartModel_->notes) {
    notesById_.emplace(note.id, &note);
    noteStates_.emplace(note.id, NotePresentationState{.id = note.id});
    lanePressed_.try_emplace(note.lane, false);
    if (const auto timelineTime = timelineTimeById_.find(note.timelineId);
        timelineTime != timelineTimeById_.end()) {
      // Match GamePlayScene::buildReplayNoteLookup(): the final parser note
      // for an identical replay key replaces an earlier one. Keeping this
      // per lane/source rather than per timeline preserves earlier lanes when
      // a later same-time row has no corresponding note.
      replayNotesByTimeLaneAndSource_.insert_or_assign(
          ReplayNoteLookupKey{.timeMicros = timelineTime->second,
                              .lane = note.lane,
                              .source = note.source},
          &note);
    }
    if (note.kind == ChartVisualNoteKind::LongHead &&
        note.longNoteMode == ChartLongNoteMode::HCN) {
      if (const auto timeline = timelineTimeById_.find(note.timelineId);
          timeline != timelineTimeById_.end()) {
        hcnPairs_.push_back({.headId = note.id,
                             .tailId = note.pairId,
                             .lane = note.lane,
                             .headTimeMicros = timeline->second});
      }
    }
    if (note.kind == ChartVisualNoteKind::LongTail &&
        note.source == ChartVisualNoteSource::Playable &&
        isClassicLongNote(note)) {
      classicLongTailIds_.push_back(note.id);
    }
  }
  std::sort(classicLongTailIds_.begin(), classicLongTailIds_.end(),
            [this](ChartVisualId leftId, ChartVisualId rightId) {
              const auto *left = notesById_.at(leftId);
              const auto *right = notesById_.at(rightId);
              const long long leftTime =
                  timelineTimeById_.at(left->timelineId);
              const long long rightTime =
                  timelineTimeById_.at(right->timelineId);
              return leftTime == rightTime ? left->lane < right->lane
                                           : leftTime < rightTime;
            });
  events_ = std::make_unique<PlayfieldPresentationEventFanout>(*state_,
                                                                 *coordinator_);
}

ReplayPlayfieldPresentation::~ReplayPlayfieldPresentation() {
#if defined(ASOBMASHOW_REPLAY_PLAYFIELD_PRESENTATION_TESTING)
  if (destructionObserverForTesting_) {
    destructionObserverForTesting_();
  }
#endif
}

std::optional<skin::SkinGameplayTiming>
ReplayPlayfieldPresentation::selectedSkinGameplayTiming() const {
  return coordinator_->selectedSkinGameplayTiming();
}

std::optional<skin::RuntimeSkinConfigurationSelection>
ReplayPlayfieldPresentation::runtimeSkinConfigurationSelection() const {
  return runtimeSkinConfigurationSelection_;
}

ReplayPlayfieldPresentationCreateResult ReplayPlayfieldPresentation::create(
    ReplayPlayfieldPresentationCreateInfo creation) {
  // The exporter supplies the chart after applying its replay-compatible long
  // note mode, so the chart metadata is the one immutable model authority.
  auto model = std::make_unique<PlayfieldChartVisualModel>(
      buildPlayfieldChartVisualModel(creation.chart, creation.chart.Meta.LnMode));
  auto state = std::make_unique<PlayfieldVisualStateStore>(*model);
  const auto graphJudgeWindows =
      replaySkinJudgeWindows(creation.timingWindows);
  const std::size_t graphGaugeHistoryCapacity = std::max<std::size_t>(
      4096, creation.replayData != nullptr ? creation.replayData->events.size()
                                           : 0);
  state->setConfiguration(creation.configuration);
  state->setReplayTouchSamples(creation.replayTouchSamples);
  if (creation.skinInput.initialState != nullptr) {
    state->setSceneStartMicros(creation.skinInput.initialState->sceneStartMicros);
    state->setPlayStartMicros(creation.skinInput.initialState->playStartMicros);
    state->applyAuthorityUpdate(creation.skinInput.initialState->authority);
  }

  auto builtIn = createBuiltInPlayfieldPresentation({
      .chart = creation.chart,
      .timingWindows = std::move(creation.timingWindows),
      .visibleTimeDurationMilliseconds =
          creation.configuration.visibleTimeDurationMilliseconds,
      .renderHud = true,
      .playbackRate = creation.playback,
      .replayData = creation.replayData,
      .replayGhostsEnabled = creation.configuration.replayGhostRenderingEnabled,
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
  const PlayfieldVisualState initialState =
      state->captureForPresentation(initialClock);
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

  std::optional<skin::RuntimeSkinConfigurationSelection>
      runtimeSkinConfigurationSelection;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  runtimeSkinConfigurationSelection = std::move(session.runtimeSelection);
#endif

  auto coordinator = std::make_unique<PlayfieldPresentationCoordinator>(
      PlayfieldPresentationCoordinatorDependencies{
          .builtIn = std::move(builtIn),
          .skin = {},
          .bga = creation.bga,
          .persistViewport = {},
          .recordFailure = std::move(creation.recordFailure),
          .replayGhostEvents =
              creation.configuration.replayGhostRenderingEnabled && creation.replayData
                  ? replay_ghost::buildReplayGhostEvents(*creation.replayData,
                                                         *model)
                  : std::vector<ReplayGhostEvent>{},
      });
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (session.disposition == GameplaySkinSessionDisposition::Ready) {
    coordinator->installSkinSession(std::move(session.session));
  }
#endif
  coordinator->configure(creation.configuration);

  return {.presentation = std::unique_ptr<ReplayPlayfieldPresentation>(
              new ReplayPlayfieldPresentation(
                  std::move(model), std::move(state), std::move(projection),
                  std::move(coordinator), renderer, {}, creation.configuration,
                  gameplay_hispeed::State(
                      {.mode = gameplay_hispeed::fixModeFromEncoded(
                           static_cast<int>(creation.settings.hispeedFixMode)),
                       .durationMilliseconds =
                           creation.settings.visibleTimeDurationMilliseconds,
                       .hispeed = creation.settings.gameplayHispeed,
                       .margin = creation.settings.hispeedMargin,
                       .laneCoverPercent =
                           creation.settings.noteStartPositionPercent,
                       .laneCoverEnabled = creation.settings.laneCoverEnabled},
                      gameplay_hispeed::summarizeChartBpm(creation.chart)),
                  std::move(runtimeSkinConfigurationSelection),
                  graphJudgeWindows, graphGaugeHistoryCapacity)),
          .failure = std::nullopt};
}

void ReplayPlayfieldPresentation::applyLaneCoverTransition(
    const ReplayLaneCoverTransition &transition, double bpm) {
  if (transition.changeKind == ReplayLaneCoverChangeKind::Enabled) {
    hispeed_.setLaneCoverEnabled(transition.enabled);
  } else {
    hispeed_.setLaneCover(transition.percent, bpm,
                          transition.resetVisibleTimeReference);
  }
  configuration_.configuredHispeed = hispeed_.hispeed();
  configuration_.noteStartPositionPercent = transition.percent;
  configuration_.laneCoverEnabled = transition.enabled;
  state_->setConfiguration(configuration_);
  coordinator_->configure(configuration_);
}

void ReplayPlayfieldPresentation::applyAuthorityUpdate(
    const PlayfieldAuthorityUpdate &authority) {
  authority_ = authority;
  if (authority_.laneCoverChanged) {
    applyLaneCoverTransition(
        {.percent = authority_.laneCoverPercent,
         .enabled = authority_.laneCoverEnabled,
         .changeKind = authority_.laneCoverChangeKind,
         .resetVisibleTimeReference =
             authority_.resetLaneCoverVisibleTimeReference},
        authority_.currentBpm);
  }
  authority_.stageCombo = stageCombo_;
  authority_.stagePassedNotes = stagePassedNotes_;
  state_->applyAuthorityUpdate(authority_);
  skinGameplayGraphDirty_ =
      skinGameplayGraph_.setGauge(authority_.gaugeType,
                                  authority_.gaugeRules) ||
      skinGameplayGraphDirty_;
}

const ChartVisualNote *
ReplayPlayfieldPresentation::replayNote(const ReplayEvent &event) const {
  if (event.noteTimeMicros < 0) {
    return nullptr;
  }
  const ChartVisualNoteSource expectedSource =
      event.action == ReplayEventAction::Mine ? ChartVisualNoteSource::Mine
                                               : ChartVisualNoteSource::Playable;
  const auto note = replayNotesByTimeLaneAndSource_.find(
      {.timeMicros = event.noteTimeMicros,
       .lane = event.lane,
       .source = expectedSource});
  return note == replayNotesByTimeLaneAndSource_.end() ? nullptr
                                                        : note->second;
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
  skinGameplayGraphDirty_ =
      skinGameplayGraph_.recordGauge(event.gauge, event.gaugeType,
                                     authority_.gaugeRules) ||
      skinGameplayGraphDirty_;
}

void ReplayPlayfieldPresentation::publishGameplayGraphState() {
  if (!skinGameplayGraphDirty_) {
    return;
  }
  state_->applyGameplayGraphState(skinGameplayGraph_.state());
  skinGameplayGraphDirty_ = false;
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

void ReplayPlayfieldPresentation::setHcnHolding(const ChartVisualNote &note,
                                                bool holding) {
  if (note.longNoteMode != ChartLongNoteMode::HCN || !isLongNote(note)) {
    return;
  }
  const ChartVisualId headId =
      note.kind == ChartVisualNoteKind::LongHead ? note.id : note.pairId;
  if (auto pair = std::ranges::find(hcnPairs_, headId,
                                    &HcnPairPlaybackState::headId);
      pair != hcnPairs_.end()) {
    pair->holding = holding;
  }
}

void ReplayPlayfieldPresentation::clearHcnHoldingOnLane(int lane) {
  for (auto &pair : hcnPairs_) {
    if (pair.lane == lane) {
      pair.holding = false;
    }
  }
}

void ReplayPlayfieldPresentation::updateHcnVisualStates(
    long long visualTimeMicros) {
  for (const auto &pair : hcnPairs_) {
    auto *headState = noteState(pair.headId);
    auto *tailState = noteState(pair.tailId);
    if (headState == nullptr || tailState == nullptr) {
      continue;
    }
    const bool headReachedJudge = headState->judged || headState->dead ||
                                  pair.headTimeMicros <= visualTimeMicros;
    const auto lane = lanePressed_.find(pair.lane);
    const bool laneDown = lane != lanePressed_.end() && lane->second;
    const bool reactive = headReachedJudge && laneDown;
    const bool active = pair.holding || reactive;
    const bool damaged = headReachedJudge && !active;
    for (auto *endpoint : {headState, tailState}) {
      endpoint->longReactive = reactive;
      endpoint->longActive = active;
      endpoint->longDamaged = damaged;
      publishNoteState(endpoint->id);
    }
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
      judgedTimeMicros < timelineTimeById_.at(note.timelineId);
  current->dead = !tailJudgedBeforeTiming;
  current->longActive = false;
  setHcnHolding(note, false);
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
  const ChartVisualNote *resolvedGraphNote = replayNote(event);
  const auto applyHud = [&]() -> bool {
    if (event.judgement == None) {
      return false;
    }
    ++stagePassedNotes_;
    if (recordedJudge.isComboBreak()) {
      stageCombo_ = 0;
    } else if (event.judgement != Kpoor) {
      ++stageCombo_;
    }
    progressiveMaximumCombo_ =
        std::max(progressiveMaximumCombo_, event.combo);
    if (resolvedGraphNote != nullptr) {
      skinGameplayGraph_.applyJudge(resolvedGraphNote->id, recordedJudge);
      skinGameplayGraphDirty_ = true;
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
    lanePressed_[event.lane] = true;
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
            setHcnHolding(*note, true);
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
    lanePressed_[event.lane] = false;
    clearHcnHoldingOnLane(event.lane);
    if (const auto *note = replayNote(event);
        note != nullptr && isLongNote(*note) && event.judgement != None &&
        note->kind == ChartVisualNoteKind::LongTail) {
      if (auto *current = noteState(note->id);
          current != nullptr && current->longActive) {
        current->judged = true;
        current->playedTimeMicros = event.judgeTimeMicros;
        current->longActive = false;
        publishNoteState(note->id);
        if (auto *paired = noteState(note->pairId); paired != nullptr) {
          paired->longActive = false;
          publishNoteState(note->pairId);
        }
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
    const auto *note = notesById_.at(tailId);
    const long long timelineTime = timelineTimeById_.at(note->timelineId);
    if (timelineTime > gameplayTimeMicros) {
      break;
    }
    if (auto *tail = noteState(tailId);
        tail != nullptr && !tail->judged && tail->longActive) {
      // This is the visual-state equivalent of LongNote::Release() in the
      // legacy exporter: the tail is played at its authored time and both
      // endpoints stop holding without an extra replay judgement.
      tail->judged = true;
      tail->playedTimeMicros = timelineTime;
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
  builtIn_->refreshGeometry();
  state_->applyAuthorityUpdate(authority_);
  publishGameplayGraphState();
  updateHcnVisualStates(clock.visualTimeMicros);
  PlayfieldVisualState state = state_->captureForPresentation(clock);
  PlayfieldProjectionRequest effectiveRequest = request;
  effectiveRequest.bpmGuideEnabled =
      effectiveRequest.bpmGuideEnabled || configuration_.bpmGuideEnabled;
  effectiveRequest.showPastNormalNotes =
      effectiveRequest.showPastNormalNotes || configuration_.showPastNotes;
  effectiveRequest.constantScroll = configuration_.constantScroll;
  effectiveRequest.constantDurationMilliseconds =
      configuration_.visibleTimeDurationMilliseconds;
  effectiveRequest.constantFadeInMilliseconds =
      configuration_.constantFadeInMilliseconds;
  // A selected skin consumes generic DTOs only. BMSRenderer's compatibility
  // plan remains necessary for built-in replay frames because parser note
  // state is intentionally immutable in this adapter.
  effectiveRequest.buildBuiltInPlan =
      coordinator_->activeMode() != PresentationMode::Skin;
  if (!effectiveRequest.builtInTraversal) {
    effectiveRequest.builtInTraversal = builtIn_->projectionTraversal();
  }
  if (effectiveRequest.latePoorTimingMicros == 0) {
    effectiveRequest.latePoorTimingMicros =
        builtIn_->projectionLatePoorTimingMicros();
  }
  if (!effectiveRequest.pmsPoorDestination) {
    effectiveRequest.pmsPoorDestination =
        coordinator_->pmsPoorDestinationGeometry();
  }
  const PlayfieldProjectionResult projection =
      projection_->project(*chartModel_, state, effectiveRequest);
#if defined(ASOBMASHOW_REPLAY_PLAYFIELD_PRESENTATION_TESTING)
  lastFrameBuiltBuiltInPlanForTesting_ = !projection.builtInPlan.entries.empty();
  lastProjectionForTesting_ = projection;
#endif
  (void)coordinator_->prepareFrame(state, projection);
  return coordinator_->render(context);
}

BMSRenderer &ReplayPlayfieldPresentation::builtInRenderer() noexcept {
  return *builtIn_;
}

#if defined(ASOBMASHOW_REPLAY_PLAYFIELD_PRESENTATION_TESTING)
PlayfieldVisualState ReplayPlayfieldPresentation::captureVisualStateForTesting(
    PlayfieldFrameClock clock) {
  publishGameplayGraphState();
  updateHcnVisualStates(clock.visualTimeMicros);
  return state_->capture(clock);
}
#endif
