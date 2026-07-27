#pragma once

#include "JudgedPlaybackData.h"
#include "../ScoreProvenance.h"
#include "../scene/play/GameplayRulesetPolicy.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace analysis {

[[nodiscard]] inline PlaybackAnalysisContext playbackContextFrom(
    const ScoreProvenance &source, const bms_parser::ChartMeta &chartMeta) {
  PlaybackAnalysisContext result{
      .ruleset = source.ruleset,
      .playback = source.playback,
      .judgeWindowScalePercent = source.judgeWindowScalePercent,
      .startingGaugePercent = source.startingGaugePercent,
      .clubMode = source.clubMode,
  };
  const ScoreStageProvenance *stage =
      score_provenance::uniqueStageForChart(source, chartMeta);
  if (stage == nullptr) {
    return result;
  }
  result.candidateSelection = stage->candidateSelection;
  PlaybackPolicySnapshot policy{
      .chartMd5 = stage->chartMd5,
      .chartSha256 = stage->chartSha256,
      .longNoteMode = stage->longNoteMode,
      .sourceJudgeRank = stage->sourceJudgeRank,
      .totalNotes = stage->totalNotes,
      .authoredGaugeTotal = stage->authoredGaugeTotal,
      .effectiveGaugeTotal = stage->effectiveGaugeTotal,
      .candidateSelection = stage->candidateSelection,
  };
  policy.effectiveJudgeWindows.reserve(stage->effectiveJudgeWindows.size());
  for (const auto &window : stage->effectiveJudgeWindows) {
    policy.effectiveJudgeWindows.push_back({
        .context = window.context,
        .judgement = window.judgement,
        .earlyMicros = window.earlyMicros,
        .lateMicros = window.lateMicros,
    });
  }
  result.policy = std::move(policy);
  return result;
}

// Builds the in-memory retry projection for a result that was loaded without
// replay events. Score provenance owns the completed attempt's setup and chart
// branch; JudgedPlaybackData remains an ephemeral consumer model.
[[nodiscard]] inline JudgedPlaybackData retrySourceFromProvenance(
    bms_parser::ChartMeta chartMeta, const ScoreProvenance &source) {
  JudgedPlaybackData result;
  result.chartMeta = std::move(chartMeta);
  result.randomSeed = result.chartMeta.RandomSeed;
  result.randomPrng = result.chartMeta.RandomPrng;
  result.randomValues = result.chartMeta.RandomValues;

  if (const ScoreStageProvenance *stage =
          score_provenance::uniqueStageForChart(source, result.chartMeta)) {
    result.randomSeed.reset();
    if (stage->chartRandomSeed.has_value() &&
        *stage->chartRandomSeed <=
            std::numeric_limits<unsigned int>::max()) {
      result.randomSeed =
          static_cast<unsigned int>(*stage->chartRandomSeed);
    }
    result.randomPrng = stage->chartRandomPrng;
    result.randomValues = stage->chartRandomValues;
    result.chartMeta.RandomSeed = result.randomSeed;
    result.chartMeta.RandomPrng = result.randomPrng;
    result.chartMeta.RandomValues = result.randomValues;
  }

  result.autoPlay = source.autoPlay;
  result.playOption = source.player1.option;
  result.playOptionSeed = source.player1.seed;
  result.playOption2 = source.player2.option;
  result.playOption2Seed = source.player2.seed;
  result.assistOption = source.assistOption;
  result.initialGaugeType = source.gaugeType;
  result.gaugeProfile = source.gaugeProfile;
  result.gaugeAutoShift = source.gaugeAutoShift;
  result.gaugeAutoShiftLowerBound = source.gaugeAutoShiftLowerBound;
  result.context = playbackContextFrom(source, result.chartMeta);
  return result;
}

[[nodiscard]] inline PlaybackAnalysisContext playbackContextFrom(
    const replay::ChartPlaybackSetup &setup) {
  RulesetDescriptor descriptor = RulesetDescriptor::Legacy();
  if (const auto ruleset = gameplayRulesetFromId(setup.playbackRulesetId)) {
    const auto supported = RulesetDescriptor::For(*ruleset);
    if (supported.version == setup.playbackRulesetRevision) {
      descriptor = supported;
    }
  }
  return {
      .ruleset = std::move(descriptor),
      .playback = {.percent = setup.playbackRatePercent,
                   .mode = setup.playbackMode},
      .candidateSelection = setup.candidateSelection,
      .judgeWindowScalePercent = setup.judgeWindowScalePercent,
      .startingGaugePercent =
          static_cast<int>(std::lround(setup.startingGaugePercent)),
      .clubMode = setup.clubMode,
  };
}

[[nodiscard]] inline PlaybackAnalysisContext playbackContextFrom(
    const replay::ChartPlaybackSetup &setup,
    const gameplay::GameplayRulesetPolicy &policy,
    const bms_parser::ChartMeta &chartMeta) {
  auto result = playbackContextFrom(setup);
  PlaybackPolicySnapshot snapshot{
      .chartMd5 = setup.chartMd5,
      .chartSha256 = setup.chartSha256,
      .longNoteMode = setup.longNoteMode,
      .sourceJudgeRank = chartMeta.Rank,
      .totalNotes = policy.gauge.totalNotes,
      .authoredGaugeTotal =
          chartMeta.HasTotal ? std::optional<double>(chartMeta.Total)
                             : std::nullopt,
      .effectiveGaugeTotal = policy.gauge.effectiveTotal,
      .candidateSelection = policy.judge.rules().candidateSelection,
  };
  constexpr std::array contexts{
      gameplay::JudgeWindowContext::Normal,
      gameplay::JudgeWindowContext::Scratch,
      gameplay::JudgeWindowContext::LongNoteTail,
      gameplay::JudgeWindowContext::LongScratchTail,
  };
  for (std::size_t contextIndex = 0; contextIndex < contexts.size();
       ++contextIndex) {
    for (const auto &window :
         policy.judge.rules().contexts[contextIndex].windows) {
      snapshot.effectiveJudgeWindows.push_back({
          .context = contexts[contextIndex],
          .judgement = window.judgement,
          .earlyMicros = window.earlyMicros,
          .lateMicros = window.lateMicros,
      });
    }
  }
  result.policy = std::move(snapshot);
  return result;
}

[[nodiscard]] inline ScoreStageProvenance scoreStageFrom(
    const PlaybackPolicySnapshot &source) {
  ScoreStageProvenance result{
      .chartMd5 = source.chartMd5,
      .chartSha256 = source.chartSha256,
      .longNoteMode = source.longNoteMode,
      .sourceJudgeRank = source.sourceJudgeRank,
      .totalNotes = source.totalNotes,
      .authoredGaugeTotal = source.authoredGaugeTotal,
      .effectiveGaugeTotal = source.effectiveGaugeTotal,
      .candidateSelection = source.candidateSelection,
  };
  result.effectiveJudgeWindows.reserve(source.effectiveJudgeWindows.size());
  for (const auto &window : source.effectiveJudgeWindows) {
    result.effectiveJudgeWindows.push_back({
        .context = window.context,
        .judgement = window.judgement,
        .earlyMicros = window.earlyMicros,
        .lateMicros = window.lateMicros,
    });
  }
  return result;
}

} // namespace analysis
