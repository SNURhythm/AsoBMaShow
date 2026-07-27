#include "LegacyReplayPlaybackAdapter.h"
#include "../analysis/JudgedPlaybackContext.h"

#include <bit>
#include <cstdint>
#include <utility>

namespace replay {
namespace {

JudgedPlaybackData makeAdapterBase(
    const ReplayPlaybackData &playback,
    const result_persistence::PersistedChartResult &result,
    bms_parser::ChartMeta chartMeta) {
  const auto &setup = playback.setup;
  const auto &score = result.score;
  chartMeta.BmsPath = score.chartPath;
  chartMeta.MD5 = score.chartMd5;
  chartMeta.SHA256 = score.chartSha256;
  chartMeta.Title = score.chartTitle;
  chartMeta.Artist = score.chartArtist;
  chartMeta.KeyMode = result.keyMode;
  chartMeta.TotalNotes = score.maxScore / 2;
  chartMeta.LnMode = score.longNoteMode;

  JudgedPlaybackData adapted;
  adapted.chartMeta = std::move(chartMeta);
  adapted.setup = setup;
  adapted.touchSamples = playback.touchSamples;
  adapted.laneCoverEvents = playback.laneCoverEvents;
  adapted.context = analysis::playbackContextFrom(setup);
  return adapted;
}

bool sameFloat(float left, float right) noexcept {
  return std::bit_cast<std::uint32_t>(left) ==
         std::bit_cast<std::uint32_t>(right);
}

bool materializedFactsMatch(
    const MaterializedReplay &materialized,
    const result_persistence::PersistedChartResult &result) noexcept {
  const auto &attempt = materialized.attempt;
  const auto &score = result.score;
  if (attempt.score != score.score || attempt.maxCombo != score.maxCombo ||
      attempt.comboBreak != score.comboBreak ||
      attempt.judgeCounts[PGreat] != score.pGreat ||
      attempt.judgeCounts[Great] != score.great ||
      attempt.judgeCounts[Good] != score.good ||
      attempt.judgeCounts[Bad] != score.bad ||
      attempt.judgeCounts[Poor] != score.poor ||
      attempt.judgeCounts[Kpoor] != score.kPoor ||
      attempt.fast != score.fast || attempt.slow != score.slow ||
      !sameFloat(attempt.gauge, score.finalGauge) ||
      attempt.gaugeType != result.adoptedGaugeType ||
      attempt.clearTypeRank != score.clearType ||
      materialized.gaugeState.gaugeType != result.adoptedGaugeType ||
      !sameFloat(materialized.gaugeState.currentGauge, score.finalGauge) ||
      materialized.gaugeHistory.size() != result.adoptedGaugeHistory.size()) {
    return false;
  }
  for (std::size_t index = 0; index < materialized.gaugeHistory.size();
       ++index) {
    if (!sameFloat(materialized.gaugeHistory[index],
                   result.adoptedGaugeHistory[index])) {
      return false;
    }
  }
  if (result.judgementTiming.has_value() &&
      attempt.judgementTiming != result.judgementTiming->byJudgement) {
    return false;
  }
  return true;
}

} // namespace

std::optional<JudgedPlaybackData> makeLegacyPlaybackAdapter(
    const ReplayPlaybackData &playback,
    const result_persistence::PersistedChartResult &result,
    bms_parser::ChartMeta chartMeta) {
  if (!playback.legacy.has_value()) {
    return std::nullopt;
  }
  const auto &score = result.score;
  JudgedPlaybackData adapted =
      makeAdapterBase(playback, result, std::move(chartMeta));
  adapted.finalScore = score.score;
  adapted.maxCombo = score.maxCombo;
  adapted.finalGauge = score.finalGauge;
  adapted.clearType = score.clearType;
  adapted.events.reserve(playback.legacy->events.size());
  for (const auto &event : playback.legacy->events) {
    adapted.events.push_back(
        {.action = static_cast<ReplayEventAction>(event.action),
         .lane = event.lane,
         .noteTimeMicros = event.noteTimeMicros,
         .songTimeMicros = event.songTimeMicros,
         .judgeTimeMicros = event.judgeTimeMicros,
         .judgement = event.judgement,
         .diffMicros = event.diffMicros,
         .gauge = event.gauge,
         .gaugeType = event.gaugeType,
         .combo = event.combo,
         .score = event.score});
  }
  return adapted;
}

std::optional<JudgedPlaybackData> makeMaterializedPlaybackAdapter(
    const ReplayPlaybackData &playback,
    const MaterializedReplay &materialized,
    const gameplay::GameplayRulesetPolicy &policy,
    const result_persistence::PersistedChartResult &result,
    bms_parser::ChartMeta chartMeta) {
  if (!materializedFactsMatch(materialized, result)) {
    return std::nullopt;
  }
  JudgedPlaybackData adapted =
      makeAdapterBase(playback, result, std::move(chartMeta));
  adapted.context =
      analysis::playbackContextFrom(playback.setup, policy, adapted.chartMeta);
  adapted.finalScore = materialized.attempt.score;
  adapted.maxCombo = materialized.attempt.maxCombo;
  adapted.finalGauge = materialized.attempt.gauge;
  adapted.clearType = materialized.attempt.clearTypeRank;
  adapted.events.reserve(materialized.judgedEvents.size());
  for (const auto &event : materialized.judgedEvents) {
    adapted.events.push_back(
        {.action = static_cast<ReplayEventAction>(event.action),
         .lane = event.lane,
         .noteTimeMicros = event.noteTimeMicros,
         .songTimeMicros = event.songTimeMicros,
         .judgeTimeMicros = event.judgeTimeMicros,
         .judgement = event.judgement,
         .diffMicros = event.diffMicros,
         .gauge = event.gauge,
         .gaugeType = event.gaugeType,
         .combo = event.combo,
         .score = event.score});
  }
  return adapted;
}

} // namespace replay
