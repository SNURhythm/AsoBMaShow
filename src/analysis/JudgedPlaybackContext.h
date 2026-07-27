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
      .startingGaugePercent = source.startingGaugePercent,
  };
  const ScoreStageProvenance *stage =
      score_provenance::uniqueStageForChart(source, chartMeta);
  if (stage == nullptr) {
    return result;
  }
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
  replay::ChartPlaybackSetup setup{
      .chartMd5 = result.chartMeta.MD5,
      .chartSha256 = result.chartMeta.SHA256,
      .keyMode = result.chartMeta.KeyMode,
      .longNoteMode = result.chartMeta.LnMode >= 1 && result.chartMeta.LnMode <= 3
                          ? result.chartMeta.LnMode
                          : 0,
      .hasUndefinedLongNotes = replay::hasUndefinedLongNotesForReplay(
          result.chartMeta.LnMode, result.chartMeta.TotalLongNotes,
          result.chartMeta.TotalBackSpinNotes),
      .randomSeed = result.chartMeta.RandomSeed,
      .randomPrng = result.chartMeta.RandomPrng,
      .randomValues = result.chartMeta.RandomValues,
      .playOption = source.player1.option,
      .playOptionSeed = source.player1.seed,
      .playOption2 = source.player2.option,
      .playOption2Seed = source.player2.seed,
      .assistOption = source.assistOption,
      .initialGaugeType = source.gaugeType,
      .gaugeProfile = source.gaugeProfile,
      .gaugeAutoShift = source.gaugeAutoShift,
      .gaugeAutoShiftLowerBound = source.gaugeAutoShiftLowerBound,
      .playbackRulesetId = source.ruleset.id,
      .playbackRulesetRevision = source.ruleset.version,
      .playbackRatePercent = source.playback.percent,
      .playbackMode = source.playback.mode,
      .judgeWindowScalePercent = source.judgeWindowScalePercent,
      .startingGaugePercent =
          source.startingGaugePercent.has_value()
              ? static_cast<float>(*source.startingGaugePercent)
              : gaugeInitialValue(source.gaugeType, source.gaugeProfile),
      .clubMode = source.clubMode,
  };

  if (const ScoreStageProvenance *stage =
          score_provenance::uniqueStageForChart(source, result.chartMeta)) {
    setup.chartMd5 = stage->chartMd5;
    setup.chartSha256 = stage->chartSha256;
    setup.longNoteMode = stage->longNoteMode;
    setup.randomSeed.reset();
    if (stage->chartRandomSeed.has_value() &&
        *stage->chartRandomSeed <=
            std::numeric_limits<unsigned int>::max()) {
      setup.randomSeed =
          static_cast<unsigned int>(*stage->chartRandomSeed);
    }
    setup.randomPrng = stage->chartRandomPrng;
    setup.randomValues = stage->chartRandomValues;
    setup.doublePlayOption = stage->doublePlayOption.value_or(
        replay::DoublePlayOption::Normal);
    setup.candidateSelection = stage->candidateSelection;
  }

  result.autoPlay = source.autoPlay;
  result.setup = std::move(setup);
  result.chartMeta.RandomSeed = result.setup.randomSeed;
  result.chartMeta.RandomPrng = result.setup.randomPrng;
  result.chartMeta.RandomValues = result.setup.randomValues;
  result.context = playbackContextFrom(source, result.chartMeta);
  return result;
}

[[nodiscard]] inline PlaybackAnalysisContext playbackContextFrom(
    const replay::ChartPlaybackSetup &setup) {
  return {
      .ruleset = rulesetDescriptorFromReplayIdentity(
          setup.playbackRulesetId, setup.playbackRulesetRevision),
      .startingGaugePercent =
          static_cast<int>(std::lround(setup.startingGaugePercent)),
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
