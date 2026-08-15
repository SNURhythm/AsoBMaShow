#include "ReplayVideoGameplayPreflight.h"

#include "BeatorajaHiSpeedChart.h"

#include "../../ChartPlaybackDuration.h"
#include "Judge.h"
#include "GamePlayTiming.h"
#include "../../skin/beatoraja/GameplaySkinEndAnimation.h"
#include "../../rendering/common.h"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace replay_video_export {

skin::UiLogicalRect replayGameplayLogicalUiBounds(int exportWidth,
                                                  int exportHeight) noexcept {
  if (exportWidth <= 0 || exportHeight <= 0) {
    return {};
  }
  const double scale = static_cast<double>(exportWidth) /
                       static_cast<double>(rendering::design_width);
  return {.x = 0.0,
          .y = 0.0,
          .width = static_cast<double>(rendering::design_width),
          .height = static_cast<double>(exportHeight) / scale};
}

PlayfieldPresentationConfig replayGameplayPresentationConfig(
    const AppSettings &settings, float playAreaWidth,
    const bms_parser::Chart &chart,
    bool touchVisualizationEnabled,
    bool replayGhostRenderingEnabled,
    const CourseConstraintRules &constraints,
    const std::string &assistOption) noexcept {
  const bool noSpeed = constraints.noSpeed;
  const int visibleTimeDurationMilliseconds =
      settings.visibleTimeDurationMilliseconds;
  const int noteStartPositionPercent =
      noSpeed ? AppSettings::kDefaultNoteStartPositionPercent
              : settings.noteStartPositionPercent;
  const bool laneCoverEnabled = settings.laneCoverEnabled;
  const gameplay_hispeed::State hispeed(
      {.mode = noSpeed ? gameplay_hispeed::FixMode::Off
                       : gameplay_hispeed::fixModeFromEncoded(
                             static_cast<int>(settings.hispeedFixMode)),
       .durationMilliseconds = visibleTimeDurationMilliseconds,
       .hispeed = noSpeed ? 1.0F : settings.gameplayHispeed,
       .margin = settings.hispeedMargin,
       .laneCoverPercent = noteStartPositionPercent,
       .laneCoverEnabled = laneCoverEnabled},
      gameplay_hispeed::summarizeChartBpm(chart));
  return {
      .visibleTimeDurationMilliseconds = visibleTimeDurationMilliseconds,
      .configuredHispeed = hispeed.hispeed(),
      .hispeedMultiplier = 1.0F,
      .visibleTimeUseMilliseconds =
          !noSpeed && settings.visibleTimeUseMilliseconds,
      .hispeedFixMode = settings.hispeedFixMode,
      .playAreaWidth = playAreaWidth,
      .laneBeamsEnabled = true,
      .laneCoverHispeedFactor = 1.0F,
      .laneCoverEnabled = laneCoverEnabled,
      .laneBeamLengthPercent = settings.laneBeamLengthPercent,
      .noteStartPositionPercent = noteStartPositionPercent,
      .laneBeamClockUsesRenderTime = true,
      .showInvisibleNotes = settings.showInvisibleNotes,
      .bpmGuideEnabled = assist_options::isBpmGuide(assistOption),
      .markProcessedNotes = settings.markProcessedNotes,
      .judgementIndicatorEnabled = settings.judgementIndicatorEnabled,
      .judgementIndicatorY = settings.judgementIndicatorY,
      .judgementIndicatorWidthScale = settings.judgementIndicatorWidthScale,
      .judgementIndicatorHudMode =
          settings.judgementIndicatorRenderMode ==
          AppSettings::JudgementIndicatorRenderMode::Hud2D,
      .judgementIndicatorRangeMilliseconds =
          settings.judgementIndicatorRangeMilliseconds,
      .judgementTextY = settings.judgementTextY,
      .judgementCounterEnabled = settings.judgementCounterEnabled,
      .judgementCounterPosition = settings.judgementCounterPosition,
      .fastSlowCriteria = settings.judgementTimingFastSlowCriteria,
      .millisecondsCriteria = settings.judgementTimingMillisecondsCriteria,
      .gaugeBarPosition = settings.gaugeBarPosition,
      .touchVisualizationEnabled = touchVisualizationEnabled,
      .replayGhostRenderingEnabled = replayGhostRenderingEnabled,
  };
}

void releaseUnsubmittedReplayGameplayBga(
    IGameplayBgaSubmitter &bga, const PresentationFrameResult &frame) noexcept {
  if (frame.preparedBga.has_value()) {
    bga.finalizePrepared(*frame.preparedBga);
  }
}

namespace {
constexpr std::int32_t kReplayPlaytimeMarginMillis = 5'000;

std::int32_t javaLongToInt(std::int64_t value) noexcept {
  return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

std::int32_t javaIntAdd(std::int32_t left, std::int32_t right) noexcept {
  return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(left) +
                                     static_cast<std::uint32_t>(right));
}
} // namespace

long long replayGameplayStatePlayDeadlineMicros(
    const bms_parser::Chart &chart, const ReplayData &replay) noexcept {
  (void)replay;
  return skin::beatorajaGameplayStatePlayDeadlineMicros(chart.Meta.PlayLength);
}

long long replayGameplayTransitionDurationMicros(
    const bms_parser::Chart &chart, const ReplayData &replay,
    const preparation::Plan &plan, long long audioOffsetMicros) noexcept {
  return plan.realTimeAtGameplayTime(
             replayGameplayStatePlayDeadlineMicros(chart, replay),
             audioOffsetMicros) +
         chart_playback_duration::kGameplayResultTransitionDelayMicros;
}

ReplayGameplayFrameState replayGameplayFrameState(
    const preparation::Plan &plan, const bms_parser::Chart &chart,
    const ReplayData &replay, const AppSettings &settings,
    std::uint64_t serial, long long realTimeMicros) noexcept {
  (void)replay;
  const long long audioOffsetMicros =
      static_cast<long long>(settings.audioOffsetMs) * 1'000LL;
  const long long visualOffsetMicros =
      static_cast<long long>(settings.visualOffsetMs) * 1'000LL;
  const auto timing = gameplay_timing::frameTiming(
      plan.chartTimeAtRealTime(realTimeMicros), audioOffsetMicros,
      visualOffsetMicros);
  const auto sceneStart = gameplay_timing::frameTiming(
      plan.skinAnimationStartTimeMicros(), audioOffsetMicros,
      visualOffsetMicros);
  const long long terminalMicros = chart.Meta.PlayLength;
  const std::int32_t playtimeMillis = javaIntAdd(
      javaLongToInt(terminalMicros / 1'000), kReplayPlaytimeMarginMillis);
  return {
      .clock = {.serial = serial,
                .visualTimeMicros = timing.visualTimeMicros,
                .gameplayTimeMicros = timing.gameplayTimeMicros,
                .replayTouchTimeMicros = timing.gameplayTimeMicros,
                .bgaTimeMicros = timing.bgaTimeMicros,
                .playTimer = {.active = timing.gameplayTimeMicros >= 0,
                              .startMicros = 0,
                              .elapsedMillisExact = true,
                              .playtimeMillis = playtimeMillis}},
      .sceneStartMicros = sceneStart.visualTimeMicros,
      .playStartMicros = 0,
  };
}

long long replayGameplayDurationWithSelectedSkinAnimation(
    const bms_parser::Chart &chart, const ReplayData &replay,
    const preparation::Plan &plan, long long audioOffsetMicros, int fps,
    long long requestedDurationMicros, bool stoppedOnGaugeFailure,
    const ReplayPlayfieldPresentation *presentation) {
  return replayGameplayDurationWithSkinTiming(
      chart, replay, plan, audioOffsetMicros, fps, requestedDurationMicros,
      stoppedOnGaugeFailure,
      presentation ? presentation->selectedSkinGameplayTiming()
                   : std::optional<skin::SkinGameplayTiming>{});
}

long long replayGameplayDurationWithSkinTiming(
    const bms_parser::Chart &chart, const ReplayData &replay,
    const preparation::Plan &plan, long long audioOffsetMicros, int fps,
    long long requestedDurationMicros, bool stoppedOnGaugeFailure,
    std::optional<skin::SkinGameplayTiming> timing) noexcept {
  (void)replay;
  if (stoppedOnGaugeFailure || fps <= 0) {
    return std::max(0LL, requestedDurationMicros);
  }
  if (!timing.has_value()) {
    return std::max(0LL, requestedDurationMicros);
  }
  const long long terminalMicros = chart.Meta.PlayLength;
  const long long deadlineMicros =
      skin::gameplaySkinAnimationCompletionDeadlineMicros(terminalMicros,
                                                            *timing);
  const long long frameMicros = (1'000'000LL + fps - 1) / fps;
  // BMSPlayer changes STATE_PLAY -> STATE_FINISHED, starts fadeout, then
  // consumes fadeout on successive updates. Retain three sampled frames so
  // zero-valued authored margins still expose all three timer phases.
  const long long sampledDeadline =
      plan.realTimeAtGameplayTime(deadlineMicros, audioOffsetMicros) +
      3 * frameMicros;
  return std::max({0LL, requestedDurationMicros, sampledDeadline});
}

ReplayLaneCoverFrameState ReplayLaneCoverPlayback::advance(
    std::span<const ReplayLaneCoverEvent> events, long long songTimeMicros) {
  bool resetVisibleTimeReference = false;
  bool changed = false;
  ReplayLaneCoverChangeKind changeKind = ReplayLaneCoverChangeKind::Value;
  std::vector<ReplayLaneCoverTransition> transitions;
  while (cursor_ < events.size() &&
         events[cursor_].songTimeMicros <= songTimeMicros) {
    percent_ = events[cursor_].noteStartPositionPercent;
    enabled_ = events[cursor_].laneCoverEnabled;
    changed = true;
    changeKind = events[cursor_].changeKind;
    resetVisibleTimeReference = events[cursor_].resetVisibleTimeReference;
    transitions.push_back({.percent = percent_,
                           .enabled = enabled_,
                           .changeKind = changeKind,
                           .resetVisibleTimeReference =
                               resetVisibleTimeReference});
    ++cursor_;
  }
  return {.percent = percent_,
          .enabled = enabled_,
          .changed = changed,
          .changeKind = changeKind,
          .resetVisibleTimeReference = resetVisibleTimeReference,
          .transitions = std::move(transitions)};
}

ReplayJudgementAuthorityPlayback::ReplayJudgementAuthorityPlayback() {
  for (int value = 0; value < JudgementCount; ++value) {
    const auto judgement = static_cast<Judgement>(value);
    judgementCounters_.emplace(judgement, 0);
    judgementFastSlowCounters_.emplace(judgement,
                                       PlayfieldJudgementFastSlowCount{});
  }
}

void ReplayJudgementAuthorityPlayback::recordApplied(const ReplayEvent &event) {
  if (event.judgement == None) {
    return;
  }
  ++judgementCounters_[event.judgement];
  const JudgeResult judge(event.judgement, event.diffMicros);
  if (judge.isComboBreak()) {
    ++comboBreak_;
  }
  if (event.judgement == Kpoor) {
    return;
  }
  if (event.diffMicros < 0) {
    ++judgementFastSlowCounters_[event.judgement].fast;
  } else if (event.diffMicros > 0) {
    ++judgementFastSlowCounters_[event.judgement].slow;
  }
}

int ReplayCourseMaximumComboPlayback::observe(
    const ReplayPlayfieldPresentation &presentation) noexcept {
  maximumCombo_ =
      std::max(maximumCombo_, presentation.progressiveMaximumCombo());
  return maximumCombo_;
}

std::string skinExportFailureMessage(const PresentationFailure &failure) {
  std::string message = failure.diagnostic.message;
  if (!failure.diagnostic.code.empty()) {
    if (!message.empty()) {
      message.append("\n\n");
    }
    message.push_back('[');
    message.append(failure.diagnostic.code);
    message.push_back(']');
  }
  return message.empty() ? "The selected gameplay skin could not be used."
                         : message;
}

std::optional<ReplayVideoExportResult> preflightReplayGameplayPresentation(
    bms_parser::Chart &chart, const ReplayData &replay,
    const AppSettings &settings, const preparation::Plan &plan,
    const PlayfieldPresentationConfig &configuration, int exportWidth,
    int exportHeight, IGameplayBgaSubmitter &bga,
    GameplaySkinSessionServices skinServices,
    display::RendererAccessCoordinator &rendererAccess,
    std::unique_ptr<ReplayPlayfieldPresentation> &presentation,
    const skin::RuntimeSkinConfigurationSelection *pinnedRuntimeSelection) {
  auto rendererReservation = rendererAccess.acquireExport();
  const ReplayGameplayFrameState frame =
      replayGameplayFrameState(plan, chart, replay, settings, 1, 0);
  PlayfieldVisualState initialState;
  initialState.clock = frame.clock;
  initialState.sceneStartMicros = frame.sceneStartMicros;
  initialState.playStartMicros = frame.playStartMicros;
  // This is the same initially-selected timeline state that LaneRenderer
  // presents before its first frame.  It lets a skin's initial properties
  // (notably 312/313) agree with the configured live Hi-Speed immediately.
  initialState.authority.currentBpm = chart.Meta.Bpm;
  initialState.authority.currentScrollRate = 1.0;
  initialState.authority.laneCoverPercent =
      configuration.noteStartPositionPercent;
  initialState.authority.laneCoverEnabled = configuration.laneCoverEnabled;
  Judge judge(chart.Meta.Rank);
  auto created = ReplayPlayfieldPresentation::create({
      .chart = chart,
      .timingWindows = judge.timingWindows,
      .configuration = configuration,
      .settings = settings,
      .playback = plan.playback,
      .bga = bga,
      .skinServices = std::move(skinServices),
      .skinInput = {.initialState = &initialState,
                    .safeUiBounds = replayGameplayLogicalUiBounds(
                        exportWidth, exportHeight),
                    .pinnedRuntimeSelection =
                        pinnedRuntimeSelection
                            ? std::optional<skin::RuntimeSkinConfigurationSelection>(
                                  *pinnedRuntimeSelection)
                            : std::nullopt},
      .replayData = &replay,
      .replayTouchSamples = replay.touchSamples,
      .recordFailure = {},
  });
  if (created.presentation != nullptr) {
    presentation = std::move(created.presentation);
    return std::nullopt;
  }

  const PresentationFailure failure = created.failure.value_or(
      PresentationFailure{.diagnostic =
                              {.code = "skin.lifecycle.activation_unavailable",
                               .message =
                                   "The selected gameplay skin is unavailable."}});
  return ReplayVideoExportResult{
      .success = false, .message = skinExportFailureMessage(failure)};
}

void destroyReplayGameplayPresentation(
    display::RendererAccessCoordinator &rendererAccess,
    std::unique_ptr<ReplayPlayfieldPresentation> &presentation) {
  if (presentation == nullptr) {
    return;
  }
  auto rendererReservation = rendererAccess.acquireExport();
  presentation.reset();
}

std::optional<ReplayVideoExportResult>
preflightCourseReplayGameplayPresentations(
    std::vector<CourseReplayGameplayPreflightStage> &stages,
    IGameplayBgaSubmitter &bga, const AppSettings &settings,
    display::RendererAccessCoordinator &rendererAccess) {
  for (auto &stage : stages) {
    stage.selectedSkinTiming.reset();
    stage.runtimeSelection.reset();
    if (const auto failure = preflightReplayGameplayPresentation(
            stage.chart, stage.replay, settings, stage.preparationPlan,
            stage.configuration, stage.exportWidth, stage.exportHeight, bga,
            std::move(stage.skinServices), rendererAccess,
            stage.presentation)) {
      for (auto &prepared : stages) {
        destroyReplayGameplayPresentation(rendererAccess,
                                           prepared.presentation);
      }
      return failure;
    }
    stage.selectedSkinTiming =
        stage.presentation ? stage.presentation->selectedSkinGameplayTiming()
                           : std::optional<skin::SkinGameplayTiming>{};
    stage.runtimeSelection =
        stage.presentation
            ? stage.presentation->runtimeSkinConfigurationSelection()
            : std::optional<skin::RuntimeSkinConfigurationSelection>{};
    destroyReplayGameplayPresentation(rendererAccess, stage.presentation);
  }
  return std::nullopt;
}

} // namespace replay_video_export
