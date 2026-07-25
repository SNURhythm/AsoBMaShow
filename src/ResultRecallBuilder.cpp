#include "ResultRecallBuilder.h"

#include "CourseConstraintUtils.h"
#include "PlayOptionUtils.h"
#include "repositories/ChartStorageIdentity.h"

#include <exception>
#include <utility>

namespace result_recall {
namespace {

ResultChartLoader effectiveLoader(ResultChartLoader loader) {
  if (loader) {
    return loader;
  }
  return [](const result_persistence::PersistedChartResult &result,
            std::atomic_bool &cancelled) {
    std::filesystem::path path = result.score.chartPath;
    chart_storage_identity::ToAbsolutePath(path);
    return play_options::parseChart(path, cancelled, "saved result");
  };
}

GameplayRuleset rulesetFor(const ScoreProvenance &provenance) {
  if (isSupportedRulesetDescriptor(provenance.ruleset)) {
    return gameplayRulesetFromId(provenance.ruleset.id)
        .value_or(GameplayRuleset::Beatoraja);
  }
  return GameplayRuleset::Beatoraja;
}

void applyStoredMeta(bms_parser::ChartMeta &meta,
                     const result_persistence::PersistedChartResult &result) {
  const auto &score = result.score;
  meta.BmsPath = score.chartPath;
  meta.MD5 = score.chartMd5;
  meta.SHA256 = score.chartSha256;
  meta.Title = score.chartTitle;
  meta.Artist = score.chartArtist;
  meta.KeyMode = result.keyMode;
  meta.TotalNotes = score.maxScore / 2;
  meta.LnMode = score.longNoteMode;
}

RhythmState stateFrom(const result_persistence::PersistedChartResult &result,
                      bms_parser::Chart &chart,
                      GaugeProfile profile = GaugeProfile::Standard) {
  const auto &score = result.score;
  RhythmState state(&chart, false, rulesetFor(score.provenance), profile);
  state.resetJudgeCounts();
  state.judgeCount[PGreat] = score.pGreat;
  state.judgeCount[Great] = score.great;
  state.judgeCount[Good] = score.good;
  state.judgeCount[Bad] = score.bad;
  state.judgeCount[Poor] = score.poor;
  state.judgeCount[Kpoor] = score.kPoor;
  state.combo = score.comboBreak == 0 ? score.maxCombo : 0;
  state.maxCombo = score.maxCombo;
  state.comboBreak = score.comboBreak;
  state.fastCount = score.fast;
  state.slowCount = score.slow;
  if (result.judgementTiming.has_value()) {
    for (int index = 0; index < JudgementCount; ++index) {
      state.judgementFastSlowCount[static_cast<Judgement>(index)] =
          result.judgementTiming->byJudgement[static_cast<std::size_t>(index)];
    }
  }

  const GaugeType recordedGauge = score.provenance.gaugeType;
  state.gaugeType = recordedGauge;
  state.selectedGaugeType = recordedGauge;
  state.currentGauge = score.finalGauge;
  state.gaugeValues[gaugeTypeIndex(recordedGauge)] = score.finalGauge;
  state.gaugeHistory = result.adoptedGaugeHistory;
  state.gaugeHistoryFor(recordedGauge) = result.adoptedGaugeHistory;
  state.restoreReadOnlyResultClearType(score.clearType);
  return state;
}

result_persistence::PersistedChartResult stageResultFor(
    const result_persistence::PersistedCourseResult &course,
    const result_persistence::PersistedCourseStageResult &stage) {
  return {
      .score = stage.score,
      .keyMode = stage.keyMode,
      .adoptedGaugeHistory = stage.adoptedGaugeHistory,
      .judgementTiming = stage.judgementTiming,
      .playedAtUnixMillis = course.playedAtUnixMillis,
  };
}

} // namespace

ChartBuildOutcome BuildChartResult(
    result_persistence::PersistedChartResult result,
    std::atomic_bool &cancelled, ResultChartLoader loader) {
  std::string diagnostic;
  if (!result_persistence::validatePersistedChartResult(result, diagnostic) ||
      result.resultFingerprint.empty()) {
    return {.diagnostic =
                "saved chart result is invalid: " +
                (diagnostic.empty() ? std::string("fingerprint is missing")
                                    : diagnostic)};
  }
  if (cancelled.load()) {
    return {.diagnostic = "saved chart is unavailable"};
  }

  try {
    auto loadChart = effectiveLoader(std::move(loader));
    auto chart = loadChart(result, cancelled);
    if (chart == nullptr || cancelled.load()) {
      return {.diagnostic = "saved chart is unavailable"};
    }
    applyStoredMeta(chart->Meta, result);
    RhythmState state = stateFrom(result, *chart);
    return {.value = ChartResult{.chart = std::move(chart),
                                 .result = std::move(result),
                                 .state = std::move(state)}};
  } catch (const std::exception &) {
    return {.diagnostic = "saved chart result could not be loaded"};
  } catch (...) {
    return {.diagnostic = "saved chart result could not be loaded"};
  }
}

CourseBuildOutcome BuildCourseResult(
    result_persistence::PersistedCourseResult result,
    std::atomic_bool &cancelled, ResultChartLoader loader) {
  std::string diagnostic;
  if (!result_persistence::validatePersistedCourseResult(result, diagnostic) ||
      result.resultFingerprint.empty()) {
    return {.diagnostic =
                "saved course result is invalid: " +
                (diagnostic.empty() ? std::string("fingerprint is missing")
                                    : diagnostic)};
  }
  if (cancelled.load()) {
    return {.diagnostic = "saved course stage is unavailable"};
  }

  try {
    auto loadChart = effectiveLoader(std::move(loader));
    auto session = std::make_shared<CoursePlaySession>();
    session->courseId = result.legacyCourseId;
    session->courseKey = result.courseKey;
    session->courseName = result.courseName;
    session->courseGroupName = result.courseGroupName;
    session->constraintJson = result.constraintJson;
    session->rulesetDescriptor = result.provenance.ruleset;
    session->ruleset = rulesetFor(result.provenance);
    session->currentIndex = 0;
    session->gaugeType = result.initialGaugeType;
    session->gaugeProfile = result.gaugeProfile;
    session->gaugeAutoShift = result.gaugeAutoShift;
    session->gaugeAutoShiftLowerBound = result.gaugeAutoShiftLowerBound;
    session->longNoteMode = result.longNoteMode;
    session->constraints =
        courseConstraintSettingsFromJson(result.constraintJson).rules;
    session->requestedPlayOption = result.requestedPlayOption;
    session->playOption = result.requestedPlayOption;
    session->assistOption = result.assistOption;
    session->autoKeySound = false;
    session->courseReplayPlayback = false;
    session->courseReplaySaved = false;
    session->courseScoreSaved = true;
    session->savedCourseReplayId = result.resultId;
    session->entries.reserve(result.stages.size());
    session->completedResults.reserve(result.stages.size());
    session->ownedResultBrowseCharts.reserve(result.stages.size());

    for (const auto &stage : result.stages) {
      auto persistedStage = stageResultFor(result, stage);
      auto loadedChart = loadChart(persistedStage, cancelled);
      if (loadedChart == nullptr || cancelled.load()) {
        return {.diagnostic = "saved course stage is unavailable"};
      }
      applyStoredMeta(loadedChart->Meta, persistedStage);
      auto chart = std::shared_ptr<bms_parser::Chart>(std::move(loadedChart));
      RhythmState state = stateFrom(persistedStage, *chart,
                                    result.gaugeProfile);
      session->entries.push_back({.meta = chart->Meta});
      session->completedResults.emplace_back(chart->Meta, state);
      session->recordStageProvenance(
          static_cast<std::size_t>(stage.stageIndex), stage.score.provenance);
      session->maxCombo = std::max(session->maxCombo, stage.score.maxCombo);
      session->ownedResultBrowseCharts.push_back(std::move(chart));
    }

    return {.value = CourseResult{.session = std::move(session),
                                  .result = std::move(result)}};
  } catch (const std::exception &) {
    return {.diagnostic = "saved course result could not be loaded"};
  } catch (...) {
    return {.diagnostic = "saved course result could not be loaded"};
  }
}

} // namespace result_recall
