#include "ReplayPlayfieldPresentation.h"

#include "BuiltInPlayfieldPresentation.h"

#include <algorithm>
#include <limits>
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

bool isJudgeEvent(const ReplayEvent &event) noexcept {
  return event.judgement != None;
}

bool isLongHead(const ChartVisualNote &note) noexcept {
  return note.kind == ChartVisualNoteKind::LongHead;
}

bool isLongTail(const ChartVisualNote &note) noexcept {
  return note.kind == ChartVisualNoteKind::LongTail;
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

  auto builtIn = createBuiltInPlayfieldPresentation({
      .chart = creation.chart,
      .timingWindows = std::move(creation.timingWindows),
      .visibleTimeGreenNumber = creation.configuration.visibleTimeGreenNumber,
      .renderHud = true,
      .playbackRate = creation.playback,
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

std::optional<ChartVisualId>
ReplayPlayfieldPresentation::replayNoteId(const ReplayEvent &event) const {
  const auto timeline = std::ranges::find_if(
      chartModel_->timelines, [&event](const ChartVisualTimeline &value) {
        return value.timeMicros == event.noteTimeMicros;
      });
  if (timeline == chartModel_->timelines.end()) {
    return std::nullopt;
  }
  const auto note = std::ranges::find_if(
      chartModel_->notes, [&event, timeline](const ChartVisualNote &value) {
        return value.timelineId == timeline->id && value.lane == event.lane &&
               value.source == ChartVisualNoteSource::Playable;
      });
  return note == chartModel_->notes.end() ? std::nullopt
                                         : std::optional<ChartVisualId>(note->id);
}

void ReplayPlayfieldPresentation::applyReplayNote(const ReplayEvent &event,
                                                   bool judged, bool dead,
                                                   bool longActive) {
  const auto id = replayNoteId(event);
  if (!id) {
    return;
  }
  state_->setNoteState({.id = *id,
                       .judged = judged,
                       .dead = dead,
                       .playedTimeMicros = judged ? event.judgeTimeMicros
                                                   : kPlayfieldTimestampOff,
                       .longActive = longActive});
}

void ReplayPlayfieldPresentation::applyReplayEvent(
    const ReplayEvent &event, const PlayfieldJudgeEventClock &clock,
    bool recordTimingSample) {
  const JudgeResult judge(event.judgement, event.diffMicros);
  const auto applyJudge = [&] {
    if (isJudgeEvent(event)) {
      events_->onJudge(judge, event.combo, event.score, clock,
                       recordTimingSample);
    }
  };

  switch (event.action) {
  case ReplayEventAction::MultiBad:
    applyReplayNote(event, true, true, false);
    applyJudge();
    break;
  case ReplayEventAction::Press: {
    const auto id = replayNoteId(event);
    bool longHead = false;
    if (id) {
      const auto note = std::ranges::find(chartModel_->notes, *id,
                                          &ChartVisualNote::id);
      longHead = note != chartModel_->notes.end() && isLongHead(*note);
    }
    if (judge.isNotePlayed()) {
      applyReplayNote(event, !longHead, !longHead, longHead);
    }
    applyJudge();
    events_->onLanePressed(event.lane, judge, clock.visualTimeMicros);
    break;
  }
  case ReplayEventAction::Release:
    applyReplayNote(event, isJudgeEvent(event), isJudgeEvent(event), false);
    applyJudge();
    events_->onLaneReleased(event.lane, clock.visualTimeMicros);
    break;
  case ReplayEventAction::Miss:
    applyReplayNote(event, true, true, false);
    applyJudge();
    break;
  case ReplayEventAction::Mine:
    applyReplayNote(event, true, true, false);
    break;
  case ReplayEventAction::Gauge:
    break;
  }

  authority_.gaugeType = event.gaugeType;
  authority_.currentGauge = event.gauge;
  state_->applyAuthorityUpdate(authority_);
}

PresentationFailure ReplayPlayfieldPresentation::makeReplayFrameFailure(
    std::uint64_t serial) const {
  if (const auto failure = coordinator_->lastFailure()) {
    return *failure;
  }
  return {.diagnostic = {.code = "skin.presentation.prepare_failed",
                         .message = "The gameplay skin could not prepare the "
                                    "replay frame.",
                         .severity = skin::DiagnosticSeverity::Error},
          .frameSerial = serial};
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
