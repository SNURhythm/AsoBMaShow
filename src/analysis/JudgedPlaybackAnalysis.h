#pragma once

#include "../PlayOptionUtils.h"
#include "../replay/LegacyReplayPlaybackAdapter.h"
#include "../replay/ReplayPlaybackMaterializer.h"
#include "../repositories/ReplayRepository.h"
#include "../scene/play/GamePlayStartOptions.h"

#include <atomic>
#include <optional>

namespace replay {

// Derives the judged projection for an already prepared replay chart. Raw BRD
// input remains the playback authority; this value is only for analysis and
// presentation consumers.
[[nodiscard]] inline std::optional<JudgedPlaybackData>
makeJudgedPlaybackForAnalysis(
    const ReplayPlaybackData &playback,
    const result_persistence::PersistedChartResult &result,
    const bms_parser::Chart &replayChart) {
  if (playback.legacy.has_value()) {
    return makeLegacyPlaybackAdapter(playback, result, replayChart.Meta);
  }

  auto sharedPlayback = std::make_shared<const ReplayPlaybackData>(playback);
  StartOptions startOptions;
  applyReplayPlaybackToStartOptions(startOptions, sharedPlayback);
  const auto policy = buildGameplayRulesetPolicyAtPlayStart(
      startOptions, replayChart.Meta, AppSettings::NotePriorityMode::Lowest);
  if (!policy.built()) {
    return std::nullopt;
  }
  const auto materialized =
      materializeReplay(playback, replayChart, *policy.policy);
  if (!materialized.materialized()) {
    return std::nullopt;
  }
  return makeMaterializedPlaybackAdapter(playback, *materialized.value,
                                         *policy.policy, result,
                                         replayChart.Meta);
}

// Loads a file-backed replay and derives the narrow judged projection used by
// ghosts, pacemakers, practice analytics, and rendering. The projection is
// never a persistence or IR source.
[[nodiscard]] inline std::optional<JudgedPlaybackData>
loadJudgedPlaybackForAnalysis(
    ReplayRepository &repository, int resultId,
    const bms_parser::ChartMeta &chartMeta) {
  auto loaded = repository.loadChartReplayPlayback(resultId);
  if (loaded.status != ChartReplayPlaybackReadOutcome::Status::Loaded ||
      !loaded.result.has_value() || !loaded.playback.has_value()) {
    return std::nullopt;
  }

  const auto &playback = *loaded.playback;
  if (playback.legacy.has_value()) {
    return makeLegacyPlaybackAdapter(playback, *loaded.result, chartMeta);
  }

  std::atomic_bool cancelled = false;
  auto replayChart = play_options::prepareReplayChart(
      chartMeta.BmsPath, playback, cancelled);
  if (replayChart == nullptr || cancelled) {
    return std::nullopt;
  }

  return makeJudgedPlaybackForAnalysis(playback, *loaded.result, *replayChart);
}

} // namespace replay
