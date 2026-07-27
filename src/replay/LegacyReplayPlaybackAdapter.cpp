#include "LegacyReplayPlaybackAdapter.h"
#include "LegacyReplayInputProjection.h"
#include "../analysis/JudgedPlaybackContext.h"
#include "../analysis/JudgedPlaybackResultState.h"

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>

namespace replay {
namespace {

JudgedPlaybackData
makeAdapterBase(const ReplayPlaybackData &playback,
                const result_persistence::PersistedChartResult &result,
                bms_parser::ChartMeta chartMeta) {
  const auto &setup = playback.setup;
  const auto &score = result.score;
  if (chartMeta.BmsPath.empty()) {
    chartMeta.BmsPath = score.chartPath;
  }
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

struct ReplayResultFacts {
  std::array<int, JudgementCount> judgeCounts{};
  std::array<JudgementFastSlowCount, JudgementCount> judgementTiming{};
  int maxCombo = 0;
  int comboBreak = 0;
  int score = 0;
  int fast = 0;
  int slow = 0;
  float gauge = 0.0F;
  GaugeType gaugeType = GaugeType::Normal;
  int clearTypeRank = kClearTypeFailedRank;
  GaugeType snapshotGaugeType = GaugeType::Normal;
  float snapshotGauge = 0.0F;
  std::span<const float> gaugeHistory;
  bool hasClearTypeFact = true;
};

ReplayResultFacts
materializedResultFacts(const MaterializedReplay &materialized) noexcept {
  return {
      .judgeCounts = materialized.attempt.judgeCounts,
      .judgementTiming = materialized.attempt.judgementTiming,
      .maxCombo = materialized.attempt.maxCombo,
      .comboBreak = materialized.attempt.comboBreak,
      .score = materialized.attempt.score,
      .fast = materialized.attempt.fast,
      .slow = materialized.attempt.slow,
      .gauge = materialized.attempt.gauge,
      .gaugeType = materialized.attempt.gaugeType,
      .clearTypeRank = materialized.attempt.clearTypeRank,
      .snapshotGaugeType = materialized.gaugeState.gaugeType,
      .snapshotGauge = materialized.gaugeState.currentGauge,
      .gaugeHistory = materialized.gaugeHistory,
  };
}

ReplayResultFacts
legacyResultFacts(const RhythmState &state,
                  std::span<const float> adoptedGaugeHistory) noexcept {
  ReplayResultFacts facts{
      .maxCombo = state.maxCombo,
      .comboBreak = state.comboBreak,
      .score = state.getScore(),
      .fast = state.fastCount,
      .slow = state.slowCount,
      .gauge = state.currentGauge,
      .gaugeType = state.gaugeType,
      .clearTypeRank = state.getClearTypeRank(),
      .snapshotGaugeType = state.gaugeType,
      .snapshotGauge = state.currentGauge,
      .gaugeHistory = adoptedGaugeHistory,
      .hasClearTypeFact = false,
  };
  for (int index = 0; index < JudgementCount; ++index) {
    const auto judgement = static_cast<Judgement>(index);
    if (const auto count = state.judgeCount.find(judgement);
        count != state.judgeCount.end()) {
      facts.judgeCounts[static_cast<std::size_t>(index)] = count->second;
    }
    if (const auto timing = state.judgementFastSlowCount.find(judgement);
        timing != state.judgementFastSlowCount.end()) {
      facts.judgementTiming[static_cast<std::size_t>(index)] = timing->second;
    }
  }
  return facts;
}

bool replayResultFactsMatch(
    const ReplayResultFacts &facts,
    const result_persistence::PersistedChartResult &result) noexcept {
  const auto &score = result.score;
  if (facts.score != score.score || facts.maxCombo != score.maxCombo ||
      facts.comboBreak != score.comboBreak ||
      facts.judgeCounts[PGreat] != score.pGreat ||
      facts.judgeCounts[Great] != score.great ||
      facts.judgeCounts[Good] != score.good ||
      facts.judgeCounts[Bad] != score.bad ||
      facts.judgeCounts[Poor] != score.poor ||
      facts.judgeCounts[Kpoor] != score.kPoor || facts.fast != score.fast ||
      facts.slow != score.slow || !sameFloat(facts.gauge, score.finalGauge) ||
      facts.gaugeType != result.adoptedGaugeType ||
      facts.snapshotGaugeType != result.adoptedGaugeType ||
      !sameFloat(facts.snapshotGauge, score.finalGauge) ||
      (facts.hasClearTypeFact && facts.clearTypeRank != score.clearType) ||
      facts.gaugeHistory.size() != result.adoptedGaugeHistory.size()) {
    return false;
  }
  for (std::size_t index = 0; index < facts.gaugeHistory.size(); ++index) {
    if (!sameFloat(facts.gaugeHistory[index],
                   result.adoptedGaugeHistory[index])) {
      return false;
    }
  }
  if (result.judgementTiming.has_value() &&
      facts.judgementTiming != result.judgementTiming->byJudgement) {
    return false;
  }
  return true;
}

bool materializedFactsMatch(
    const MaterializedReplay &materialized,
    const result_persistence::PersistedChartResult &result) noexcept {
  return replayResultFactsMatch(materializedResultFacts(materialized), result);
}

} // namespace

std::optional<JudgedPlaybackData> makeLegacyPlaybackAdapter(
    const ReplayPlaybackData &playback,
    const result_persistence::PersistedChartResult &result,
    bms_parser::Chart &chart, ReplayMaterializationSeed seed) {
  if (!playback.legacy.has_value()) {
    return std::nullopt;
  }
  const auto projectedInput =
      projectLegacyReplayInput(playback.legacy->events, playback.setup.keyMode);
  if (!projectedInput.has_value() || projectedInput->input != playback.input ||
      projectedInput->stockScratchDirectionBestEffort !=
          playback.legacy->stockScratchDirectionBestEffort) {
    return std::nullopt;
  }
  const auto &score = result.score;
  JudgedPlaybackData adapted = makeAdapterBase(playback, result, chart.Meta);
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
  const GaugeStateSnapshot *carriedGauge =
      seed.carriedGauge.has_value() ? &*seed.carriedGauge : nullptr;
  try {
    auto reconstructed = analysis::BuildValidatedResultState(
        chart, adapted, playback.setup.gaugeProfile, carriedGauge,
        seed.carriedCombo, seed.carriedMaxCombo);
    if (!reconstructed.has_value() ||
        !replayResultFactsMatch(
            legacyResultFacts(reconstructed->state,
                              reconstructed->adoptedGaugeHistory),
            result)) {
      return std::nullopt;
    }
  } catch (const std::invalid_argument &) {
    return std::nullopt;
  }
  return adapted;
}

std::optional<JudgedPlaybackData> makeMaterializedPlaybackAdapter(
    const ReplayPlaybackData &playback, const MaterializedReplay &materialized,
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
