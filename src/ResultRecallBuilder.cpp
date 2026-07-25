#include "ResultRecallBuilder.h"

#include "CourseConstraintUtils.h"
#include "PlayOptionUtils.h"
#include "ReplayResultStateBuilder.h"
#include "repositories/ChartStorageIdentity.h"

#include <exception>
#include <utility>

namespace result_recall {
namespace {

ReplayChartLoader effectiveLoader(ReplayChartLoader loader) {
  if (loader) {
    return loader;
  }
  return [](const ReplayData &replay, std::atomic_bool &cancelled) {
    std::filesystem::path path = replay.chartMeta.BmsPath;
    chart_storage_identity::ToAbsolutePath(path);
    return play_options::prepareReplayChart(path, replay, cancelled);
  };
}

bool replayOutcomeMatches(const bms_parser::ChartMeta &meta,
                          const RhythmState &state,
                          const ReplayData &replay) {
  const bool fullCombo = meta.TotalNotes > 0 && state.comboBreak == 0 &&
                         state.maxCombo >= meta.TotalNotes;
  const int reconstructedClear = clear_policy::fullComboRankForPlayback(
      state.getClearTypeRank(), fullCombo, replay.provenance.playback);
  return state.getScore() == replay.finalScore &&
         state.maxCombo == replay.maxCombo &&
         reconstructedClear == replay.clearType;
}

struct HistoricalIrBuildOutcome {
  std::optional<HistoricalIrContext> value;
  std::string diagnostic;
};

HistoricalIrBuildOutcome historicalIrFor(const ReplayResultRecord &record,
                                         const bms_parser::ChartMeta &meta,
                                         const RhythmState &state) {
  if (!record.attemptId.has_value()) {
    return {.diagnostic =
                "IR verification failed because the saved result has no "
                "attempt identity. This score cannot be uploaded safely."};
  }
  if (!record.attemptFingerprint.has_value() ||
      record.attemptFingerprint->empty()) {
    return {.diagnostic =
                "IR verification failed because the saved result has no "
                "integrity fingerprint. This score cannot be uploaded "
                "safely."};
  }
  if (record.playedAtUnixMillis <= 0) {
    return {.diagnostic =
                "IR verification failed because the saved result has no play "
                "completion time. This score cannot be uploaded safely."};
  }

  std::string diagnostic;
  auto attempt = legacy_result_persistence::makeLegacyChartResultAttempt(
      *record.attemptId, meta, state, record.replay.provenance,
      record.replay.chartMeta.LnMode, record.replay, diagnostic);
  if (!attempt.has_value()) {
    const std::string invariant =
        diagnostic.empty()
            ? "canonical score attempt could not be reconstructed"
            : diagnostic;
    return {.diagnostic =
                "IR verification failed: " + invariant +
                ". The saved replay no longer reproduces the original score, "
                "so it cannot be uploaded safely."};
  }
  if (attempt->payloadFingerprint != *record.attemptFingerprint) {
    return {.diagnostic =
                "IR verification failed because the stored fingerprint "
                "differs from the reconstructed score. The chart or replay "
                "metadata may have changed since the score was saved, so it "
                "cannot be uploaded safely."};
  }
  result_persistence::PersistedChartResult persisted{
      .attemptId = attempt->attemptId,
      .score = attempt->score,
      .keyMode = meta.KeyMode,
      .adoptedGaugeHistory = attempt->adoptedGaugeHistory,
      .judgementTiming = attempt->judgementTiming,
      .playedAtUnixMillis = record.playedAtUnixMillis,
  };
  persisted.resultFingerprint =
      result_persistence::resultFingerprint(persisted);
  auto submission = ir::makeIrSubmission(persisted);
  if (!submission.value.has_value()) {
    const std::string invariant =
        submission.diagnostic.empty()
            ? "canonical submission construction failed"
            : submission.diagnostic;
    return {.diagnostic = "IR submission validation failed: " + invariant +
                          ". This score cannot be uploaded safely."};
  }

  auto attemptPtr =
      std::make_shared<
          const legacy_result_persistence::LegacyChartResultAttempt>(
          std::move(*attempt));
  auto submissionPtr = std::make_shared<const ir::IrSubmission>(
      std::move(*submission.value));
  result_persistence::SaveOutcome saved{
      .state = result_persistence::SaveState::Saved,
      .receipt = result_persistence::StageReceipt{
          .attemptId = attemptPtr->attemptId,
          .resultId = record.replay.id,
          .createdAt = record.replay.createdAt,
          .scorePending = false}};
  return {.value = HistoricalIrContext{.attempt = std::move(attemptPtr),
                                       .submission = std::move(submissionPtr),
                                       .saveOutcome = std::move(saved)}};
}

} // namespace

ChartBuildOutcome BuildChartResult(ReplayResultRecord record,
                                   std::atomic_bool &cancelled,
                                   ReplayChartLoader loader) {
  try {
    auto loadChart = effectiveLoader(std::move(loader));
    auto chart = loadChart(record.replay, cancelled);
    if (chart == nullptr || cancelled.load()) {
      return {.diagnostic = "saved chart is unavailable"};
    }

    record.replay.chartMeta = chart->Meta;
    RhythmState state =
        replay_result::BuildResultState(*chart, record.replay);
    if (!replayOutcomeMatches(chart->Meta, state, record.replay)) {
      return {.diagnostic = "saved chart outcome does not match"};
    }
    auto historicalIr = historicalIrFor(record, chart->Meta, state);
    return {.value = ChartResult{.chart = std::move(chart),
                                 .replay = std::move(record.replay),
                                 .state = std::move(state),
                                 .historicalIr = std::move(historicalIr.value),
                                 .historicalIrDiagnostic =
                                     std::move(historicalIr.diagnostic)}};
  } catch (const std::exception &) {
    return {.diagnostic = "saved chart result could not be reconstructed"};
  } catch (...) {
    return {.diagnostic = "saved chart result could not be reconstructed"};
  }
}

CourseBuildOutcome BuildCourseResult(CourseReplayData replay,
                                     std::atomic_bool &cancelled,
                                     ReplayChartLoader loader) {
  if (replay.stages.empty() || replay.completedCharts <= 0 ||
      replay.totalCharts <= 0 ||
      replay.stages.size() !=
          static_cast<std::size_t>(replay.completedCharts) ||
      replay.completedCharts > replay.totalCharts ||
      replay.stages.size() > static_cast<std::size_t>(
                                 replay_summary_scan::
                                     kMaxCourseStagesPerCandidate)) {
    return {.diagnostic = "saved course stage count is invalid"};
  }

  try {
    auto loadChart = effectiveLoader(std::move(loader));
    auto session = std::make_shared<CoursePlaySession>();
    session->courseId = replay.courseId;
    session->courseKey = replay.courseKey;
    session->courseName = replay.courseName;
    session->courseGroupName = replay.courseGroupName;
    session->constraintJson = replay.constraintJson;
    session->snapshotRulesetFromReplay(replay.stages.front().replay);
    session->currentIndex = 0;
    session->gaugeType = replay.initialGaugeType;
    session->gaugeProfile = replay.gaugeProfile;
    session->gaugeAutoShift = replay.gaugeAutoShift;
    session->gaugeAutoShiftLowerBound = replay.gaugeAutoShiftLowerBound;
    session->longNoteMode = replay.longNoteMode;
    session->constraints =
        courseConstraintSettingsFromJson(replay.constraintJson).rules;
    session->requestedPlayOption = replay.requestedPlayOption;
    session->assistOption = replay.assistOption;
    session->autoKeySound = false;
    session->courseReplayPlayback = false;
    session->courseReplaySaved = true;
    session->courseScoreSaved = true;
    session->savedCourseReplayId = replay.id;
    session->entries.reserve(replay.stages.size());
    session->completedResults.reserve(replay.stages.size());
    session->ownedResultBrowseCharts.reserve(replay.stages.size());
    session->replayStages.reserve(replay.stages.size());

    std::optional<GaugeStateSnapshot> carriedGauge;
    for (std::size_t index = 0; index < replay.stages.size(); ++index) {
      ReplayData &stageReplay = replay.stages[index].replay;
      auto loadedChart = loadChart(stageReplay, cancelled);
      if (loadedChart == nullptr || cancelled.load()) {
        return {.diagnostic = "saved course stage is unavailable"};
      }
      stageReplay.chartMeta.BmsPath = loadedChart->Meta.BmsPath;
      auto chart = std::shared_ptr<bms_parser::Chart>(std::move(loadedChart));
      RhythmState state = replay_result::BuildResultState(
          *chart, stageReplay, replay.gaugeProfile,
          carriedGauge.has_value() ? &*carriedGauge : nullptr,
          session->carriedCombo, session->maxCombo);
      if (!replayOutcomeMatches(chart->Meta, state, stageReplay)) {
        return {.diagnostic = "saved course stage outcome does not match"};
      }

      session->entries.push_back({.meta = chart->Meta});
      session->completedResults.emplace_back(chart->Meta, state);
      session->recordStageProvenance(index, stageReplay.provenance);
      session->replayStages.push_back(replay.stages[index]);
      carriedGauge = state.gaugeSnapshot();
      session->carriedGauge = carriedGauge;
      session->carriedCombo = state.combo;
      session->maxCombo = std::max(session->maxCombo, state.maxCombo);
      session->ownedResultBrowseCharts.push_back(std::move(chart));
    }

    session->courseReplayData =
        std::make_shared<CourseReplayData>(std::move(replay));
    return {.value = CourseResult{.session = std::move(session)}};
  } catch (const std::exception &) {
    return {.diagnostic = "saved course result could not be reconstructed"};
  } catch (...) {
    return {.diagnostic = "saved course result could not be reconstructed"};
  }
}

} // namespace result_recall
