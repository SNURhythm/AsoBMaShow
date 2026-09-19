#include "CourseRecordActions.h"

#include "../CourseConstraintUtils.h"
#include "../CoursePlaySession.h"
#include "../ModernResultRecallBuilder.h"
#include "../ReplayResultStateBuilder.h"
#include "../replay/CourseReplayConsumer.h"
#include "../repositories/ReplayRepository.h"

#include <algorithm>
#include <utility>

namespace course_records {

std::optional<CurrentCourseSelection> currentCourseSelectionFor(
    std::string_view currentCourseKey,
    const std::vector<ChartMetaRecord> &records,
    const result_persistence::ModernCourseResult &result) {
  if (currentCourseKey != result.courseKey || result.totalCharts <= 0 ||
      result.stages.size() > records.size()) {
    return std::nullopt;
  }
  const auto unavailable = [](const ChartMetaRecord &record) {
    return record.solidArchive || record.unavailable || record.meta.BmsPath.empty();
  };
  if (std::any_of(records.begin(),
                  records.begin() + static_cast<std::ptrdiff_t>(result.stages.size()),
                  unavailable)) {
    return std::nullopt;
  }
  CurrentCourseSelection selection{
      .records = records,
      .completeCourse = records.size() == static_cast<std::size_t>(result.totalCharts) &&
                        std::none_of(records.begin(), records.end(), unavailable),
  };
  selection.completedChartPaths.reserve(result.stages.size());
  for (std::size_t index = 0; index < result.stages.size(); ++index) {
    selection.completedChartPaths.push_back(records[index].meta.BmsPath);
  }
  return selection;
}

PreparedCourseResult prepareCourseResult(
    ReplayRepository &repository, std::string_view attemptId,
    const std::optional<CurrentCourseSelection> &currentSelection,
    bool retrySameAllowed, std::atomic_bool &cancelled) {
  try {
    if (cancelled.load()) return {};
    const auto exact = repository.LoadModernCourseResultByAttempt(attemptId);
    if (cancelled.load()) return {};
    if (exact.status != ModernCourseResultReadStatus::Loaded || !exact.record) {
      return {.diagnostic = exact.diagnostic.empty()
                                ? "saved course result was not found"
                                : exact.diagnostic};
    }
    if (!currentSelection || currentSelection->completedChartPaths.size() !=
                                 exact.record->result.stages.size()) {
      return {.diagnostic = "current course charts are unavailable"};
    }
    auto recalled = result_recall::BuildCourseResult(
        exact.record->result, cancelled, currentSelection->completedChartPaths);
    if (cancelled.load()) return {};
    if (!recalled.value || recalled.value->completedStages.empty()) {
      return {.diagnostic = recalled.diagnostic.empty()
                                ? "saved course result was not found"
                                : recalled.diagnostic};
    }
    auto view = std::move(*recalled.value);
    std::shared_ptr<CourseReplayData> resultBrowseReplayData;
    std::vector<std::shared_ptr<bms_parser::Chart>> resultBrowseReplayCharts;
    if (exact.record->replayFile) {
      auto consumer =
          replay::makeRuntimeCourseReplayConsumer(repository);
      auto replay = consumer.load(*exact.record,
                                  currentSelection->completedChartPaths,
                                  cancelled);
      if (replay.ready() && replay.replayData != nullptr &&
          replay.replayData->stages.size() == view.completedStages.size() &&
          replay.charts.size() == view.completedStages.size()) {
        resultBrowseReplayData = std::move(replay.replayData);
        resultBrowseReplayCharts.reserve(replay.charts.size());
        for (auto &chart : replay.charts) {
          resultBrowseReplayCharts.emplace_back(std::move(chart));
        }
      }
    }
    if (cancelled.load()) {
      return {};
    }
    auto session = std::make_shared<CoursePlaySession>();
    session->courseId = view.result.legacyCourseId;
    session->courseKey = view.result.courseKey;
    session->courseName = view.result.courseName;
    session->courseGroupName = view.result.courseGroupName;
    session->constraintJson = view.result.constraintJson;
    session->entries.resize(
        static_cast<std::size_t>(view.result.totalCharts));
    for (std::size_t index = 0; index < session->entries.size(); ++index) {
      session->entries[index].meta.TotalNotes =
          view.result.entryFacts[index].totalNotes;
      session->entries[index].meta.PlayLength =
          view.result.entryFacts[index].playLengthMicros;
    }
    for (std::size_t index = 0; index < currentSelection->records.size() &&
                                index < session->entries.size();
         ++index) {
      session->entries[index].meta = currentSelection->records[index].meta;
    }
    session->stageProvenance.resize(view.completedStages.size());
    session->completedResults.reserve(view.completedStages.size());
    session->ownedResultBrowseCharts.reserve(view.completedStages.size());
    session->ownedResultBrowseReplayCharts =
        std::move(resultBrowseReplayCharts);
    session->resultBrowseReplayData = std::move(resultBrowseReplayData);
    session->modernCourseChartPaths.reserve(view.completedStages.size());
    for (std::size_t index = 0; index < view.completedStages.size();
         ++index) {
      auto &stage = view.completedStages[index];
      session->entries[index].meta = stage.chart->Meta;
      const ReplayData *stageReplay = session->resultBrowseStageReplay(index);
      bms_parser::Chart *replayChart =
          session->resultBrowseReplayChart(index);
      session->completedResults.emplace_back(
          stage.chart->Meta, stage.state,
          stageReplay != nullptr && replayChart != nullptr
              ? replay_result::BuildSkinGameplayGraphState(
                    *replayChart, *stageReplay, stage.state)
              : replay_result::BuildSkinGameplayChartGraphState(
                    *stage.chart, stage.state));
      session->ownedResultBrowseCharts.push_back(stage.chart);
      session->stageProvenance[index] = stage.result.score.provenance;
      session->modernCourseChartPaths.push_back(stage.chart->Meta.BmsPath);
    }
    session->modernCourseAttemptId = view.result.attemptId;
    session->modernCoursePlayedAtUnixMillis = view.result.playedAtUnixMillis;
    session->modernCourseResultBrowsing = true;
    session->restoreFinalClearTypeForResult(view.result.clearType);
    session->modernCourseRetrySameAllowed =
        retrySameAllowed && currentSelection->completeCourse;
    session->gaugeType = view.result.initialGaugeType;
    session->gaugeProfile = view.result.gaugeProfile;
    session->gaugeAutoShift = view.result.gaugeAutoShift;
    session->gaugeAutoShiftLowerBound = view.result.gaugeAutoShiftLowerBound;
    session->longNoteMode = view.result.longNoteMode;
    session->requestedPlayOption = view.result.requestedPlayOption;
    session->assistOption = view.result.assistOption;
    session->constraints =
        courseConstraintSettingsFromJson(view.result.constraintJson).rules;
    session->maxCombo = view.result.maxCombo;
    session->carriedGauge =
        session->completedResults.back().state.gaugeSnapshot();
    if (const auto ruleset =
            gameplayRulesetFromId(view.result.provenance.ruleset.id)) {
      session->ruleset = *ruleset;
      session->rulesetDescriptor = view.result.provenance.ruleset;
    }

    if (cancelled.load()) return {};
    return {.session = std::move(session)};
  } catch (...) {
    if (cancelled.load()) return {};
    return {.diagnostic = "saved course result could not be recalled"};
  }
}

} // namespace course_records
