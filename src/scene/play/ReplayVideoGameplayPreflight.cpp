#include "ReplayVideoGameplayPreflight.h"

#include "Judge.h"
#include "GamePlayTiming.h"
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

ReplayGameplayFrameState replayGameplayFrameState(
    const preparation::Plan &plan, const bms_parser::Chart &chart,
    const ReplayData &replay, const AppSettings &settings,
    std::uint64_t serial, long long realTimeMicros) noexcept {
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
  const long long terminalMicros =
      replay.autoPlay ? chart.Meta.TotalLength : chart.Meta.PlayLength;
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

ReplayLaneCoverFrameState ReplayLaneCoverPlayback::advance(
    std::span<const ReplayLaneCoverEvent> events, long long songTimeMicros) {
  bool resetVisibleTimeReference = false;
  while (cursor_ < events.size() &&
         events[cursor_].songTimeMicros <= songTimeMicros) {
    percent_ = events[cursor_].noteStartPositionPercent;
    resetVisibleTimeReference = events[cursor_].resetVisibleTimeReference;
    ++cursor_;
  }
  return {.percent = percent_,
          .resetVisibleTimeReference = resetVisibleTimeReference};
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
    std::unique_ptr<ReplayPlayfieldPresentation> &presentation) {
  auto rendererReservation = rendererAccess.acquireExport();
  const ReplayGameplayFrameState frame =
      replayGameplayFrameState(plan, chart, replay, settings, 1, 0);
  PlayfieldVisualState initialState;
  initialState.clock = frame.clock;
  initialState.sceneStartMicros = frame.sceneStartMicros;
  initialState.playStartMicros = frame.playStartMicros;
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
                        exportWidth, exportHeight)},
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
  }
  return std::nullopt;
}

} // namespace replay_video_export
