#include "ReplayVideoGameplayPreflight.h"

#include "Judge.h"

namespace replay_video_export {

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
    const AppSettings &settings, const PlayfieldPresentationConfig &configuration,
    audio::PlaybackRate playback, skin::UiLogicalRect safeUiBounds,
    IGameplayBgaSubmitter &bga, GameplaySkinSessionServices skinServices,
    std::unique_ptr<ReplayPlayfieldPresentation> &presentation) {
  Judge judge(chart.Meta.Rank);
  auto created = ReplayPlayfieldPresentation::create({
      .chart = chart,
      .timingWindows = judge.timingWindows,
      .configuration = configuration,
      .settings = settings,
      .playback = playback,
      .bga = bga,
      .skinServices = std::move(skinServices),
      .skinInput = {.safeUiBounds = safeUiBounds},
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

std::optional<ReplayVideoExportResult>
preflightCourseReplayGameplayPresentations(
    std::vector<CourseReplayGameplayPreflightStage> &stages,
    IGameplayBgaSubmitter &bga, const AppSettings &settings) {
  for (auto &stage : stages) {
    if (const auto failure = preflightReplayGameplayPresentation(
            stage.chart, stage.replay, settings, stage.configuration,
            stage.playback, stage.safeUiBounds, bga,
            std::move(stage.skinServices), stage.presentation)) {
      return failure;
    }
  }
  return std::nullopt;
}

} // namespace replay_video_export
