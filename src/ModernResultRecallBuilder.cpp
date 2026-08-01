#include "ModernResultRecallBuilder.h"

#include "BmsMetadataText.h"
#include "PlayOptionUtils.h"
#include "ResultContracts.h"
#include "repositories/ChartStorageIdentity.h"

#include <exception>
#include <utility>

namespace result_recall {
namespace {

struct SavedChartSetup {
  int longNoteMode = long_note_mode::kUnknownValue;
  std::optional<unsigned int> randomSeed;
  std::optional<std::string> randomPrng;
  std::vector<int> randomValues;
  int expectedTotalNotes = 0;
  bool provenanceBacked = false;
};

std::optional<SavedChartSetup>
savedChartSetup(const result_persistence::ChartScoreWrite &score,
                std::string &diagnostic) {
  diagnostic.clear();
  const auto replayLongNoteMode =
      result_persistence::replaySetupLongNoteMode(score);
  if (!replayLongNoteMode.has_value()) {
    diagnostic = "saved chart setup has no unique provenance stage";
    return std::nullopt;
  }
  if (score.provenance.stages.empty()) {
    return SavedChartSetup{.longNoteMode = *replayLongNoteMode};
  }

  bms_parser::ChartMeta savedIdentity;
  savedIdentity.MD5 = score.chartMd5;
  savedIdentity.SHA256 = score.chartSha256;
  const ScoreStageProvenance *stage =
      score_provenance::uniqueStageForChart(score.provenance, savedIdentity);
  if (stage == nullptr) {
    diagnostic = "saved chart setup has no unique provenance stage";
    return std::nullopt;
  }
  const auto maximumScore =
      result_contract::maximumScoreForNotes(stage->totalNotes);
  if (!maximumScore || *maximumScore != score.maxScore) {
    diagnostic = "saved chart setup disagrees with result facts";
    return std::nullopt;
  }
  const auto randomSetup = score_provenance::savedChartRandomParseSetup(
      score.provenance, savedIdentity, diagnostic);
  if (!randomSetup) {
    return std::nullopt;
  }

  return SavedChartSetup{
      .longNoteMode = *replayLongNoteMode,
      .randomSeed = randomSetup->randomSeed,
      .randomPrng = randomSetup->randomPrng,
      .randomValues = randomSetup->randomValues.value_or(std::vector<int>{}),
      .expectedTotalNotes = stage->totalNotes,
      .provenanceBacked = true,
  };
}

std::unique_ptr<bms_parser::Chart>
loadSavedChart(const std::filesystem::path &path,
               const SavedChartSetup &setup, std::atomic_bool &cancelled,
               const ModernChartLoader &loader) {
  if (loader) {
    return loader(path, cancelled);
  }
  std::filesystem::path absolutePath = path;
  chart_storage_identity::ToAbsolutePath(absolutePath);
  const std::optional<std::vector<int>> randomValues =
      setup.provenanceBacked && !setup.randomValues.empty()
          ? std::optional(setup.randomValues)
          : std::nullopt;
  return play_options::parseChart(absolutePath, setup.randomSeed,
                                  setup.randomPrng, randomValues, cancelled,
                                  "result recall");
}

bool chartSetupAgrees(const SavedChartSetup &setup,
                      const bms_parser::Chart &chart) {
  if (!setup.provenanceBacked) {
    return true;
  }
  if ((setup.randomSeed && chart.Meta.RandomSeed != setup.randomSeed) ||
      (setup.randomPrng && chart.Meta.RandomPrng != setup.randomPrng) ||
      chart.Meta.RandomValues != setup.randomValues ||
      chart.Meta.TotalNotes != setup.expectedTotalNotes) {
    return false;
  }
  return !chartContainsLongNote(chart) ||
         normalizeChartLongNoteModeValue(chart.Meta.LnMode) ==
             setup.longNoteMode;
}

bool chartIdentityAgrees(const result_persistence::ChartScoreWrite &score,
                         int keyMode,
                         const bms_parser::ChartMeta &parsed) noexcept {
  using asobmshow::bms_metadata::normalizedHash;
  const result_contract::ChartIdentity expected{
      .md5 = score.chartMd5,
      .sha256 = score.chartSha256,
      .keyMode = keyMode,
  };
  const result_contract::ChartIdentity selected{
      .md5 = normalizedHash(parsed.MD5),
      .sha256 = normalizedHash(parsed.SHA256),
      .keyMode = parsed.KeyMode,
  };
  const auto maximumScore =
      result_contract::maximumScoreForNotes(parsed.TotalNotes);
  if (!result_contract::canonicalChartIdentity(selected,
                                               !expected.md5.empty()) ||
      result_contract::compareChartIdentity(expected, selected) !=
          result_contract::ChartIdentityMatch::Match ||
      parsed.TotalNotes <= 0 || !maximumScore ||
      *maximumScore != score.maxScore) {
    return false;
  }
  return true;
}

void applySavedDisplayFacts(const result_persistence::ChartScoreWrite &score,
                            int replayLongNoteMode,
                            bms_parser::ChartMeta &meta) {
  if (!score.chartTitle.empty()) {
    meta.Title = score.chartTitle;
  }
  if (!score.chartArtist.empty()) {
    meta.Artist = score.chartArtist;
  }
  meta.LnMode = replayLongNoteMode;
}

GameplayRuleset rulesetFor(const ScoreProvenance &provenance) noexcept {
  if (isSupportedRulesetDescriptor(provenance.ruleset)) {
    return gameplayRulesetFromId(provenance.ruleset.id)
        .value_or(GameplayRuleset::Beatoraja);
  }
  return GameplayRuleset::Beatoraja;
}

RhythmState resultStateFrom(
    bms_parser::Chart &chart, const result_persistence::ChartScoreWrite &score,
    GaugeType adoptedGaugeType, const std::vector<float> &adoptedGaugeHistory,
    const std::optional<result_persistence::ChartJudgementTiming> &timing,
    GaugeType selectedGaugeType, GaugeProfile gaugeProfile,
    GaugeAutoShiftMode gaugeAutoShift, GaugeType gaugeAutoShiftLowerBound,
    const std::string &assistOption) {
  RhythmState state(&chart, false, rulesetFor(score.provenance), gaugeProfile);
  state.configureGauge(selectedGaugeType, gaugeAutoShift, gaugeProfile,
                       gaugeAutoShiftLowerBound);
  if (score.provenance.startingGaugePercent.has_value()) {
    state.setStartingGaugePercent(*score.provenance.startingGaugePercent);
  }
  state.setAssistClearMark(clear_policy::assistClearMarkRequired(
      assist_options::isEnabled(assistOption), score.provenance.playback));

  state.resetJudgeCounts();
  state.judgeCount[PGreat] = score.pGreat;
  state.judgeCount[Great] = score.great;
  state.judgeCount[Good] = score.good;
  state.judgeCount[Bad] = score.bad;
  state.judgeCount[Poor] = score.poor;
  state.judgeCount[Kpoor] = score.kPoor;
  if (timing.has_value()) {
    for (int index = 0; index < JudgementCount; ++index) {
      state.judgementFastSlowCount[static_cast<Judgement>(index)] =
          timing->byJudgement[static_cast<std::size_t>(index)];
    }
  }
  state.fastCount = score.fast;
  state.slowCount = score.slow;
  state.combo = 0;
  state.maxCombo = score.maxCombo;
  state.comboBreak = score.comboBreak;

  state.gaugeType = adoptedGaugeType;
  state.currentGauge = score.finalGauge;
  const int adoptedIndex = gaugeTypeIndex(adoptedGaugeType);
  state.gaugeValues[static_cast<std::size_t>(adoptedIndex)] = score.finalGauge;
  if (gaugeIsSurvival(adoptedGaugeType, state.gaugeProfile) &&
      score.finalGauge <= 0.0F) {
    state.gaugeSurvivalFailed[static_cast<std::size_t>(adoptedIndex)] = true;
  }
  state.gaugeHistory = adoptedGaugeHistory;
  state.gaugeHistoryFor(adoptedGaugeType) = adoptedGaugeHistory;
  return state;
}

RhythmState
chartResultState(bms_parser::Chart &chart,
                 const result_persistence::ModernChartResult &result) {
  const ScoreProvenance &provenance = result.score.provenance;
  return resultStateFrom(
      chart, result.score, result.adoptedGaugeType, result.adoptedGaugeHistory,
      result.judgementTiming, provenance.gaugeType, provenance.gaugeProfile,
      provenance.gaugeAutoShift, provenance.gaugeAutoShiftLowerBound,
      provenance.assistOption);
}

RhythmState courseStageResultState(
    bms_parser::Chart &chart,
    const result_persistence::ModernCourseResult &course,
    const result_persistence::ModernCourseStageResult &stage) {
  return resultStateFrom(chart, stage.score, stage.adoptedGaugeType,
                         stage.adoptedGaugeHistory, stage.judgementTiming,
                         course.initialGaugeType, course.gaugeProfile,
                         course.gaugeAutoShift, course.gaugeAutoShiftLowerBound,
                         course.assistOption);
}

} // namespace

ModernChartBuildOutcome
BuildChartResult(result_persistence::ModernChartResult result,
                 std::atomic_bool &cancelled, ModernChartLoader loader) {
  const std::filesystem::path storedPath = result.score.chartPath;
  return BuildChartResult(std::move(result), cancelled, storedPath,
                          std::move(loader));
}

ModernChartBuildOutcome BuildChartResult(
    result_persistence::ModernChartResult result, std::atomic_bool &cancelled,
    const std::filesystem::path &currentPath, ModernChartLoader loader) {
  try {
    std::string validationDiagnostic;
    if (!result_persistence::validateModernChartResult(result,
                                                       validationDiagnostic)) {
      return {.diagnostic =
                  "saved chart result is invalid: " + validationDiagnostic};
    }

    auto setup = savedChartSetup(result.score, validationDiagnostic);
    if (!setup) {
      return {.diagnostic = validationDiagnostic};
    }
    auto chart = loadSavedChart(currentPath, *setup, cancelled, loader);
    if (chart == nullptr || cancelled.load()) {
      return {.diagnostic = "saved chart is unavailable"};
    }
    if (setup->provenanceBacked) {
      applyEffectiveLongNoteModeToChart(*chart, setup->longNoteMode);
    }
    if (!chartSetupAgrees(*setup, *chart)) {
      return {.diagnostic = "saved chart setup does not match"};
    }
    if (!chartIdentityAgrees(result.score, result.keyMode, chart->Meta)) {
      return {.diagnostic = "saved chart identity does not match"};
    }

    applySavedDisplayFacts(result.score, setup->longNoteMode, chart->Meta);
    RhythmState state = chartResultState(*chart, result);
    return {.value = ModernChartResultView{.chart = std::move(chart),
                                           .result = std::move(result),
                                           .state = std::move(state)}};
  } catch (const std::exception &) {
    return {.diagnostic = "saved chart result could not be recalled"};
  } catch (...) {
    return {.diagnostic = "saved chart result could not be recalled"};
  }
}

ModernCourseBuildOutcome
BuildCourseResult(result_persistence::ModernCourseResult result,
                  std::atomic_bool &cancelled, ModernChartLoader loader) {
  std::vector<std::filesystem::path> storedPaths;
  storedPaths.reserve(result.stages.size());
  for (const auto &stage : result.stages) {
    storedPaths.push_back(stage.score.chartPath);
  }
  return BuildCourseResult(std::move(result), cancelled, storedPaths,
                           std::move(loader));
}

ModernCourseBuildOutcome BuildCourseResult(
    result_persistence::ModernCourseResult result, std::atomic_bool &cancelled,
    std::span<const std::filesystem::path> currentPaths,
    ModernChartLoader loader) {
  try {
    std::string validationDiagnostic;
    if (!result_persistence::validateModernCourseResult(result,
                                                        validationDiagnostic)) {
      return {.diagnostic =
                  "saved course result is invalid: " + validationDiagnostic};
    }
    if (currentPaths.size() != result.stages.size()) {
      return {.diagnostic =
                  "current course selection does not cover saved stages"};
    }

    std::vector<ModernCourseStageView> completedStages;
    completedStages.reserve(result.stages.size());
    for (std::size_t index = 0; index < result.stages.size(); ++index) {
      const auto &stage = result.stages[index];
      auto setup = savedChartSetup(stage.score, validationDiagnostic);
      if (!setup) {
        return {.diagnostic = validationDiagnostic};
      }
      auto loaded =
          loadSavedChart(currentPaths[index], *setup, cancelled, loader);
      if (loaded == nullptr || cancelled.load()) {
        return {.diagnostic = "saved course stage is unavailable"};
      }
      if (setup->provenanceBacked) {
        applyEffectiveLongNoteModeToChart(*loaded, setup->longNoteMode);
      }
      if (!chartSetupAgrees(*setup, *loaded)) {
        return {.diagnostic = "saved course stage setup does not match"};
      }
      if (!chartIdentityAgrees(stage.score, stage.keyMode, loaded->Meta)) {
        return {.diagnostic = "saved course stage identity does not match"};
      }
      applySavedDisplayFacts(stage.score, setup->longNoteMode, loaded->Meta);
      auto chart = std::shared_ptr<bms_parser::Chart>(std::move(loaded));
      RhythmState state = courseStageResultState(*chart, result, stage);
      completedStages.push_back({.chart = std::move(chart),
                                 .result = stage,
                                 .state = std::move(state)});
    }

    return {.value = ModernCourseResultView{.result = std::move(result),
                                            .completedStages =
                                                std::move(completedStages)}};
  } catch (const std::exception &) {
    return {.diagnostic = "saved course result could not be recalled"};
  } catch (...) {
    return {.diagnostic = "saved course result could not be recalled"};
  }
}

} // namespace result_recall
