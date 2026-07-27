#pragma once

#include "JudgedPlaybackResultState.h"
#include "../PlayOptionUtils.h"
#include "../replay/LegacyReplayPlaybackAdapter.h"
#include "../replay/ReplayPlaybackMaterializer.h"
#include "../repositories/ReplayRepository.h"
#include "../scene/play/GamePlayStartOptions.h"

#include <atomic>
#include <optional>

namespace replay {

struct JudgedPlaybackAnalysisOptions {
  AppSettings::NotePriorityMode notePriorityMode =
      AppSettings::NotePriorityMode::Lowest;
  CourseConstraintRules courseConstraints;
  ReplayMaterializationSeed materializationSeed;
};

// Advances the compact cross-stage state from a validated judged projection.
// This keeps course consumers from reimplementing legacy-versus-native carry
// behavior when a later native BRD stage must be materialized.
[[nodiscard]] inline ReplayMaterializationSeed
materializationSeedAfterJudgedPlayback(
    const ReplayMaterializationSeed &current, bms_parser::Chart &chart,
    const JudgedPlaybackData &playback,
    std::optional<GaugeProfile> gaugeProfile = std::nullopt) {
  const GaugeStateSnapshot *carriedGauge =
      current.carriedGauge.has_value() ? &*current.carriedGauge : nullptr;
  RhythmState state =
      analysis::BuildResultState(chart, playback, gaugeProfile, carriedGauge,
                                 current.carriedCombo, current.carriedMaxCombo);
  return {
      .carriedGauge = state.gaugeSnapshot(),
      .carriedCombo = state.combo,
      .carriedMaxCombo = state.maxCombo,
  };
}

// Derives the judged projection for an already prepared replay chart. Native
// BRD input remains the playback authority; migration-backed tracks use their
// isolated compatibility adapter. The projection is only for analysis and
// presentation consumers.
[[nodiscard]] inline std::optional<JudgedPlaybackData>
makeJudgedPlaybackForAnalysis(
    const ReplayPlaybackData &playback,
    const result_persistence::PersistedChartResult &result,
    bms_parser::Chart &replayChart,
    JudgedPlaybackAnalysisOptions options = {}) {
  if (playback.legacy.has_value()) {
    return makeLegacyPlaybackAdapter(playback, result, replayChart,
                                     options.materializationSeed);
  }

  auto sharedPlayback = std::make_shared<const ReplayPlaybackData>(playback);
  StartOptions startOptions;
  applyCourseReplayPlaybackToStartOptions(startOptions, sharedPlayback,
                                          options.courseConstraints);
  const auto policy = buildGameplayRulesetPolicyAtPlayStart(
      startOptions, replayChart.Meta, options.notePriorityMode);
  if (!policy.built()) {
    return std::nullopt;
  }
  const auto materialized = materializeReplay(
      playback, replayChart, *policy.policy, options.materializationSeed);
  if (!materialized.materialized()) {
    return std::nullopt;
  }
  return makeMaterializedPlaybackAdapter(
      playback, *materialized.value, *policy.policy, result, replayChart.Meta);
}

// Loads a file-backed replay and derives the narrow judged projection used by
// ghosts, pacemakers, practice analytics, and rendering. The projection is
// never a persistence or IR source.
[[nodiscard]] inline std::optional<JudgedPlaybackData>
loadJudgedPlaybackForAnalysis(ReplayRepository &repository, int resultId,
                              const bms_parser::ChartMeta &chartMeta) {
  auto loaded = repository.loadChartReplayPlayback(resultId);
  if (loaded.status != ChartReplayPlaybackReadOutcome::Status::Loaded ||
      !loaded.result.has_value() || !loaded.playback.has_value()) {
    return std::nullopt;
  }

  const auto &playback = *loaded.playback;
  std::atomic_bool cancelled = false;
  auto replayChart =
      play_options::prepareReplayChart(chartMeta.BmsPath, playback, cancelled);
  if (replayChart == nullptr || cancelled) {
    return std::nullopt;
  }

  return makeJudgedPlaybackForAnalysis(playback, *loaded.result, *replayChart);
}

} // namespace replay
