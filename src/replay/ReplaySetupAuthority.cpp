#include "ReplaySetupAuthority.h"

#include "../BmsMetadataText.h"
#include "../PlayOptionUtils.h"
#include "../scene/play/GameplayGaugeRules.h"
#include "../scene/play/GameplayScoreState.h"
#include "BeatorajaLongNoteMode.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace replay::setup_authority {
namespace {

Outcome resolved(ChartPlaybackSetup setup) {
  return {.status = Status::Resolved, .setup = std::move(setup)};
}

Outcome conflict(std::string field) {
  return {.status = Status::Conflict,
          .field = field,
          .diagnostic = "Replay setup " + field +
                        " differs from result provenance."};
}

Outcome invalid(std::string field, std::string diagnostic) {
  return {.status = Status::Invalid,
          .field = std::move(field),
          .diagnostic = std::move(diagnostic)};
}

std::optional<GaugeStateSnapshot> deterministicStartingGaugeState(
    const ChartPlaybackSetup &setup,
    std::optional<int> startingGaugePercent) {
  const auto ruleset = gameplayRulesetFromId(setup.playbackRulesetId);
  if (!ruleset.has_value()) {
    return std::nullopt;
  }
  bms_parser::ChartMeta chartMeta;
  chartMeta.KeyMode = setup.keyMode;
  GameplayScoreState state({
      .gaugeRules = compileGameplayGaugeRules(
          *ruleset, chartMeta, setup.gaugeProfile),
      .keyMode = setup.keyMode,
  });
  state.configureGauge(setup.initialGaugeType, setup.gaugeAutoShift,
                       setup.gaugeProfile,
                       setup.gaugeAutoShiftLowerBound);
  if (startingGaugePercent.has_value()) {
    state.setStartingGaugePercent(*startingGaugePercent);
  }
  return state.gaugeSnapshot();
}

bool equivalentStartingGaugeState(const GaugeStateSnapshot &actual,
                                  const GaugeStateSnapshot &expected,
                                  int keyMode,
                                  GameplayRuleset ruleset) {
  return actual.gaugeType == expected.gaugeType &&
         actual.selectedGaugeType == expected.selectedGaugeType &&
         actual.gaugeAutoShiftLowerBound ==
             expected.gaugeAutoShiftLowerBound &&
         resolveGaugeProfileForRuleset(ruleset, actual.gaugeProfile,
                                       keyMode) ==
             resolveGaugeProfileForRuleset(ruleset, expected.gaugeProfile,
                                           keyMode) &&
         actual.gaugeAutoShift == expected.gaugeAutoShift &&
         actual.currentGauge == expected.currentGauge &&
         actual.gaugeValues == expected.gaugeValues &&
         actual.gaugeSurvivalFailed == expected.gaugeSurvivalFailed;
}

bool fullSetupSource(Source source) {
  return source == Source::CapturedAttempt ||
         source == Source::AsoExtension;
}

} // namespace

Outcome resolveForResult(ChartPlaybackSetup setup,
                         const result_persistence::ChartScoreWrite &score,
                         int expectedKeyMode, Source source,
                         bool carriedStartingGaugeAllowed) {
  using asobmshow::bms_metadata::normalizedHash;

  if (source != Source::CapturedAttempt && source != Source::AsoExtension &&
      source != Source::Stock) {
    return invalid("source", "Replay setup source is not recognized.");
  }
  const bool hasFullSetup = fullSetupSource(source);
  if (score.longNoteMode < 0 || score.longNoteMode > 3) {
    return invalid("long-note mode",
                   "Result long-note mode is not recognized.");
  }
  bool longNoteModeMatches = setup.longNoteMode == score.longNoteMode;
  if (!hasFullSetup) {
    const auto setupStockMode = stockLongNoteMode(setup.longNoteMode);
    const auto scoreStockMode = stockLongNoteMode(score.longNoteMode);
    longNoteModeMatches = setupStockMode.has_value() &&
                          scoreStockMode.has_value() &&
                          setupStockMode == scoreStockMode;
  }
  if (setup.chartSha256 != score.chartSha256) {
    return conflict("chart SHA-256");
  }
  if (setup.keyMode != expectedKeyMode) {
    return conflict("key mode");
  }
  if (!longNoteModeMatches) {
    return conflict("long-note mode");
  }
  if (hasFullSetup &&
      normalizedHash(setup.chartMd5) != normalizedHash(score.chartMd5)) {
    return conflict("chart MD5");
  }

  const auto &provenance = score.provenance;
  if (provenance.eligibility == ScoreEligibility::LegacyUnverified) {
    return resolved(std::move(setup));
  }
  if (!isSupportedRulesetDescriptor(provenance.ruleset)) {
    return invalid(
        "ruleset descriptor",
        "Result provenance gameplay ruleset descriptor is not supported.");
  }

  if (!hasFullSetup) {
    setup.chartMd5 = normalizedHash(score.chartMd5);
    setup.longNoteMode = score.longNoteMode;
  }

  const ScoreStageProvenance *stage = nullptr;
  if (!provenance.stages.empty()) {
    bms_parser::ChartMeta chartMeta;
    chartMeta.MD5 = score.chartMd5;
    chartMeta.SHA256 = score.chartSha256;
    stage = score_provenance::uniqueStageForChart(provenance, chartMeta);
    if (stage == nullptr) {
      return invalid("stage identity",
                     "Result provenance does not identify one chart stage.");
    }
    if (stage->longNoteMode != score.longNoteMode) {
      return invalid(
          "provenance long-note mode",
          "Result provenance long-note mode differs from its score.");
    }
  }

  if (setup.initialGaugeType != provenance.gaugeType) {
    return conflict("initial gauge");
  }
  if (play_options::normalizePlayOption(
          setup.playOption.value_or("NORMAL")) !=
      play_options::normalizePlayOption(provenance.player1.option)) {
    return conflict("player-one option");
  }
  if (setup.playOptionSeed != provenance.player1.seed) {
    return conflict("player-one option seed");
  }
  if (play_options::normalizePlayOption(
          setup.playOption2.value_or("NORMAL")) !=
      play_options::normalizePlayOption(provenance.player2.option)) {
    return conflict("player-two option");
  }
  if (setup.playOption2Seed != provenance.player2.seed) {
    return conflict("player-two option seed");
  }
  if (stage != nullptr && setup.randomValues != stage->chartRandomValues) {
    return conflict("chart random values");
  }
  if (stage != nullptr && stage->doublePlayOption.has_value() &&
      setup.doublePlayOption != *stage->doublePlayOption) {
    return conflict("double-play option");
  }

  if (hasFullSetup) {
    const auto ruleset = gameplayRulesetFromId(provenance.ruleset.id);
    if (!ruleset.has_value()) {
      return invalid("ruleset ID",
                     "Result provenance ruleset is not supported.");
    }
    if (resolveGaugeProfileForRuleset(
            *ruleset, setup.gaugeProfile, setup.keyMode) !=
        resolveGaugeProfileForRuleset(
            *ruleset, provenance.gaugeProfile, setup.keyMode)) {
      return conflict("gauge profile");
    }
    if (setup.gaugeAutoShift != provenance.gaugeAutoShift) {
      return conflict("gauge auto shift");
    }
    if (setup.gaugeAutoShiftLowerBound !=
        provenance.gaugeAutoShiftLowerBound) {
      return conflict("gauge auto-shift lower bound");
    }
    if (assist_options::normalize(setup.assistOption) !=
        assist_options::normalize(provenance.assistOption)) {
      return conflict("assist option");
    }
    if (setup.playbackRulesetId != provenance.ruleset.id) {
      return conflict("ruleset ID");
    }
    if (setup.playbackRulesetRevision != provenance.ruleset.version) {
      return conflict("ruleset revision");
    }
    if (setup.playbackRatePercent != provenance.playback.percent) {
      return conflict("playback percentage");
    }
    if (setup.playbackMode != provenance.playback.mode) {
      return conflict("playback mode");
    }
    if (setup.judgeWindowScalePercent !=
        provenance.judgeWindowScalePercent) {
      return conflict("judge-window scale");
    }
    if (setup.clubMode != provenance.clubMode) {
      return conflict("club mode");
    }
  } else {
    setup.gaugeProfile = provenance.gaugeProfile;
    setup.gaugeAutoShift = provenance.gaugeAutoShift;
    setup.gaugeAutoShiftLowerBound = provenance.gaugeAutoShiftLowerBound;
    setup.assistOption = assist_options::normalize(provenance.assistOption);
    setup.playbackRulesetId = provenance.ruleset.id;
    setup.playbackRulesetRevision = provenance.ruleset.version;
    setup.playbackRatePercent = provenance.playback.percent;
    setup.playbackMode = provenance.playback.mode;
    setup.judgeWindowScalePercent = provenance.judgeWindowScalePercent;
    setup.clubMode = provenance.clubMode;
    if (provenance.startingGaugePercent.has_value()) {
      setup.startingGaugePercent =
          static_cast<float>(*provenance.startingGaugePercent);
    }
  }

  if (stage != nullptr && hasFullSetup) {
    if (normalizedHash(setup.chartMd5) !=
        normalizedHash(stage->chartMd5)) {
      return conflict("provenance-stage chart MD5");
    }
    const std::optional<std::uint64_t> randomSeed =
        setup.randomSeed.has_value()
            ? std::optional<std::uint64_t>(*setup.randomSeed)
            : std::nullopt;
    if (randomSeed != stage->chartRandomSeed) {
      return conflict("chart random seed");
    }
    if (setup.randomPrng != stage->chartRandomPrng) {
      return conflict("chart random PRNG");
    }
    if (setup.candidateSelection != stage->candidateSelection) {
      return conflict("candidate selection");
    }
  } else if (stage != nullptr) {
    if (stage->chartRandomSeed.has_value() &&
        *stage->chartRandomSeed >
            std::numeric_limits<unsigned int>::max()) {
      return invalid("chart random seed",
                     "Result provenance random seed cannot be replayed.");
    }
    setup.randomSeed =
        stage->chartRandomSeed.has_value()
            ? std::optional<unsigned int>(
                  static_cast<unsigned int>(*stage->chartRandomSeed))
            : std::nullopt;
    setup.randomPrng = stage->chartRandomPrng;
    setup.candidateSelection = stage->candidateSelection;
  }

  if (!carriedStartingGaugeAllowed) {
    const auto expectedState = deterministicStartingGaugeState(
        setup, provenance.startingGaugePercent);
    if (!expectedState.has_value()) {
      return invalid("ruleset ID",
                     "Replay starting gauge ruleset is not supported.");
    }
    if (setup.startingGaugeState.has_value()) {
      const auto ruleset = gameplayRulesetFromId(setup.playbackRulesetId);
      if (!ruleset.has_value()) {
        return invalid("ruleset ID",
                       "Replay starting gauge ruleset is not supported.");
      }
      if (!equivalentStartingGaugeState(
              *setup.startingGaugeState, *expectedState, setup.keyMode,
              *ruleset)) {
        return conflict("starting gauge state");
      }
    } else {
      const bool scalarMatches = provenance.startingGaugePercent.has_value()
                                     ? std::lround(
                                           setup.startingGaugePercent) ==
                                           *provenance.startingGaugePercent
                                     : std::fabs(
                                           setup.startingGaugePercent -
                                           expectedState->currentGauge) <=
                                           0.0001F;
      if (hasFullSetup && !scalarMatches) {
        return conflict("starting gauge");
      }
      setup.startingGaugeState = *expectedState;
      setup.startingGaugePercent = expectedState->currentGauge;
    }
  } else if (provenance.startingGaugePercent.has_value()) {
    const float effectiveStartingGauge =
        setup.startingGaugeState.has_value()
            ? setup.startingGaugeState->currentGauge
            : setup.startingGaugePercent;
    if (std::lround(effectiveStartingGauge) !=
        *provenance.startingGaugePercent) {
      return conflict("starting gauge");
    }
  }
  if (setup.startingGaugeState.has_value()) {
    setup.startingGaugePercent = setup.startingGaugeState->currentGauge;
  }
  return resolved(std::move(setup));
}

} // namespace replay::setup_authority
