#include "CourseReplayConsumer.h"

#include "../CourseConstraintUtils.h"
#include "../CoursePlaySession.h"
#include "../LongNoteModeUtils.h"

#include <numeric>
#include <utility>

namespace replay {
namespace {

CourseReplayConsumerOutcome failure(
    CourseReplayConsumerState state, std::string diagnostic,
    CourseReplayContextOutcome context = {}) {
  return {.state = state,
          .context = std::move(context),
          .diagnostic = std::move(diagnostic)};
}

ReplaySetupSource setupSource(ReplayStageDecodeSource source) noexcept {
  return source == ReplayStageDecodeSource::AsoExtension
             ? ReplaySetupSource::AsoExtension
             : ReplaySetupSource::StockBeatoraja;
}

result_persistence::ModernChartResult chartResultForStage(
    const result_persistence::ModernCourseResult &course,
    std::size_t index) {
  const auto &stage = course.stages[index];
  result_persistence::ModernChartResult result{
      .attemptId = course.attemptId,
      .score = stage.score,
      .keyMode = stage.keyMode,
      .adoptedGaugeType = stage.adoptedGaugeType,
      .adoptedGaugeHistory = stage.adoptedGaugeHistory,
      .judgementTiming = stage.judgementTiming,
      .playedAtUnixMillis = course.playedAtUnixMillis,
  };
  result.resultFingerprint =
      result_persistence::modernResultFingerprint(result);
  return result;
}

result_persistence::ModernCourseStageResult stageResultForChart(
    int stageIndex,
    const result_persistence::ModernChartResult &chartResult) {
  return {
      .stageIndex = stageIndex,
      .score = chartResult.score,
      .keyMode = chartResult.keyMode,
      .adoptedGaugeType = chartResult.adoptedGaugeType,
      .adoptedGaugeHistory = chartResult.adoptedGaugeHistory,
      .judgementTiming = chartResult.judgementTiming,
  };
}

bool finalGaugeAgrees(
    const ReplayPlaybackMaterializationOutcome &materialized) noexcept {
  return materialized.judgedResult && materialized.finalGaugeState &&
         materialized.finalGaugeState->gaugeType ==
             materialized.judgedResult->adoptedGaugeType &&
         materialized.finalGaugeState->currentGauge ==
             materialized.judgedResult->score.finalGauge;
}

std::int64_t completedPrefixMaximumScore(
    const result_persistence::ModernCourseResult &result) noexcept {
  std::int64_t value = 0;
  for (const auto &stage : result.stages) {
    value += stage.score.maxScore;
  }
  return value;
}

std::optional<result_persistence::ModernCourseResult>
rebuildJudgedCourseResult(
    const result_persistence::ModernCourseResult &saved,
    std::vector<result_persistence::ModernCourseStageResult> stages,
    std::string &diagnostic) {
  result_persistence::ModernCourseResultCapture capture{
      .attemptId = saved.attemptId,
      .courseKey = saved.courseKey,
      .legacyCourseId = saved.legacyCourseId,
      .courseName = saved.courseName,
      .courseGroupName = saved.courseGroupName,
      .constraintJson = saved.constraintJson,
      .requestedPlayOption = saved.requestedPlayOption,
      .assistOption = saved.assistOption,
      .initialGaugeType = saved.initialGaugeType,
      .gaugeProfile = saved.gaugeProfile,
      .gaugeAutoShift = saved.gaugeAutoShift,
      .gaugeAutoShiftLowerBound = saved.gaugeAutoShiftLowerBound,
      .longNoteMode = saved.longNoteMode,
      .clearType = saved.clearType,
      .stages = std::move(stages),
      .entryFacts = saved.entryFacts,
      .playedAtUnixMillis = saved.playedAtUnixMillis,
  };
  auto judged =
      result_persistence::captureModernCourseResult(capture, diagnostic);
  if (judged) {
    judged->resultId = saved.resultId;
  }
  return judged;
}

std::shared_ptr<CourseReplayData> makeCompatibilityCourse(
    const VerifiedCourseReplay &verified,
    std::vector<CourseReplayStageData> stages) {
  const auto &result = verified.result;
  return std::make_shared<CourseReplayData>(CourseReplayData{
      // This is a memory-only adapter, not a legacy replay row identifier.
      .id = 0,
      .courseId = result.legacyCourseId,
      .courseKey = result.courseKey,
      .courseName = result.courseName,
      .courseGroupName = result.courseGroupName,
      .constraintJson = result.constraintJson,
      .requestedPlayOption = result.requestedPlayOption,
      .assistOption = result.assistOption,
      .initialGaugeType = result.initialGaugeType,
      .gaugeProfile = result.gaugeProfile,
      .gaugeAutoShift = result.gaugeAutoShift,
      .gaugeAutoShiftLowerBound = result.gaugeAutoShiftLowerBound,
      .longNoteMode = result.longNoteMode,
      .finalScore = result.finalScore,
      .maxCombo = result.maxCombo,
      .finalGauge = result.finalGauge,
      .clearType = result.clearType,
      .completedCharts = result.completedCharts,
      .totalCharts = result.totalCharts,
      .stages = std::move(stages),
      .provenance = result.provenance,
  });
}

} // namespace

CourseReplayConsumer::CourseReplayConsumer(
    CourseReplayConsumerDependencies dependencies)
    : dependencies_(std::move(dependencies)) {}

CourseReplayConsumerOutcome CourseReplayConsumer::load(
    const ModernCourseResultRecord &listedRecord,
    const std::vector<std::filesystem::path> &completedChartPaths,
    std::atomic_bool &cancelled) const noexcept {
  try {
    std::string diagnostic;
    const auto &listedResult = listedRecord.result;
    if (cancelled.load() || !dependencies_.parseBaseChart ||
        !dependencies_.loadContext || !dependencies_.prepareChart ||
        !dependencies_.materializeStage ||
        !result_persistence::validateModernCourseResult(listedResult,
                                                        diagnostic) ||
        completedChartPaths.size() != listedResult.stages.size() ||
        completedChartPaths.empty()) {
      return failure(CourseReplayConsumerState::InvalidRequest,
                     diagnostic.empty()
                         ? "Course replay consumer request is incomplete."
                         : std::move(diagnostic));
    }

    std::vector<std::unique_ptr<bms_parser::Chart>> parsedCharts;
    ParsedCourseReplayFacts parsedFacts;
    parsedCharts.reserve(completedChartPaths.size());
    parsedFacts.stages.reserve(completedChartPaths.size());
    for (std::size_t index = 0; index < completedChartPaths.size(); ++index) {
      if (completedChartPaths[index].empty()) {
        return failure(CourseReplayConsumerState::InvalidRequest,
                       "A selected course chart path is empty.");
      }
      auto chart = dependencies_.parseBaseChart(completedChartPaths[index],
                                                cancelled);
      if (!chart || cancelled.load()) {
        return failure(CourseReplayConsumerState::ChartUnavailable,
                       "A selected course chart could not be parsed.");
      }
      parsedFacts.stages.push_back(makeParsedCourseReplayStageFacts(
          chart->Meta, listedResult.stages[index].score.longNoteMode,
          chartContainsUndefinedLongNote(*chart)));
      parsedCharts.push_back(std::move(chart));
    }

    auto context = dependencies_.loadContext(listedResult.attemptId,
                                             parsedFacts);
    if (!context.replayAvailable() || !context.verified) {
      return failure(CourseReplayConsumerState::ReplayUnavailable,
                     context.diagnostic.empty() ? "Course replay is unavailable."
                                                : context.diagnostic,
                     std::move(context));
    }
    if (context.verified->result != listedResult) {
      return failure(CourseReplayConsumerState::StaleResult,
                     "The selected course result changed while loading.",
                     std::move(context));
    }

    const VerifiedCourseReplay &verified = *context.verified;
    if (verified.document.playback.stages.size() != parsedCharts.size() ||
        verified.document.timeBounds.size() != parsedCharts.size() ||
        verified.stageSources.size() != parsedCharts.size()) {
      return failure(CourseReplayConsumerState::ReplayUnavailable,
                     "Verified course replay has an inconsistent stage shape.",
                     std::move(context));
    }

    std::vector<std::unique_ptr<bms_parser::Chart>> preparedCharts;
    std::vector<CourseReplayStageData> compatibilityStages;
    std::vector<result_persistence::ModernCourseStageResult> judgedStages;
    preparedCharts.reserve(parsedCharts.size());
    compatibilityStages.reserve(parsedCharts.size());
    judgedStages.reserve(parsedCharts.size());
    std::optional<CourseContinuationState> continuation;
    ReplayPlaybackCarryState carry;

    for (std::size_t index = 0; index < parsedCharts.size(); ++index) {
      const auto &savedStage = verified.result.stages[index];
      const auto &stagePlayback = verified.document.playback.stages[index];
      auto prepared = dependencies_.prepareChart(
          completedChartPaths[index], stagePlayback.setup,
          savedStage.score.provenance, parsedCharts[index]->Meta, cancelled,
          diagnostic);
      if (!prepared || cancelled.load()) {
        return failure(CourseReplayConsumerState::SetupUnavailable,
                       diagnostic.empty()
                           ? "A course replay stage setup could not be reproduced."
                           : std::move(diagnostic),
                       std::move(context));
      }

      const ReplayChartIdentity preparedIdentity{
          .md5 = prepared->Meta.MD5,
          .sha256 = prepared->Meta.SHA256,
          .keyMode = prepared->Meta.KeyMode,
      };
      const ReplayChartIdentity savedIdentity{
          .md5 = savedStage.score.chartMd5,
          .sha256 = savedStage.score.chartSha256,
          .keyMode = savedStage.keyMode,
      };
      if (compareReplayChartIdentity(preparedIdentity, savedIdentity) !=
              ReplayChartMatch::Match ||
          long_note_mode::normalizeValue(prepared->Meta.LnMode) !=
              savedStage.score.longNoteMode) {
        return failure(CourseReplayConsumerState::PreparedStageMismatch,
                       "A prepared course stage differs from its saved result.",
                       std::move(context));
      }

      const ReplayChartDocument stageDocument{
          .playback = stagePlayback,
          .timeBounds = verified.document.timeBounds[index],
      };
      const auto savedChartResult =
          chartResultForStage(verified.result, index);
      auto materialized = dependencies_.materializeStage(
          stageDocument, setupSource(verified.stageSources[index]),
          savedChartResult, *prepared, carry);
      if (!materialized.matched() || !materialized.replayData ||
          !materialized.judgedResult ||
          !materialized.initialGaugeState ||
          !finalGaugeAgrees(materialized)) {
        const auto state =
            materialized.state ==
                    ReplayPlaybackMaterializationState::ResultMismatch ||
                    (materialized.matched() &&
                     !finalGaugeAgrees(materialized))
                ? CourseReplayConsumerState::ResultMismatch
                : CourseReplayConsumerState::MaterializationFailed;
        return failure(state,
                       materialized.diagnostic.empty()
                           ? "Course replay stage judging did not match."
                           : std::move(materialized.diagnostic),
                       std::move(context));
      }
      const auto stageAgreement =
          result_persistence::compareModernChartResultFacts(
              savedChartResult, *materialized.judgedResult);
      if (!stageAgreement.agrees()) {
        return failure(CourseReplayConsumerState::ResultMismatch,
                       stageAgreement.diagnostic, std::move(context));
      }

      if (!continuation) {
        const auto started = startCourseContinuation(
            {.totalStages = verified.result.stages.size(),
             .initialGauge = *materialized.initialGaugeState,
             .constraints = {
                 .beatorajaConstraintIds =
                     beatorajaCourseConstraintIdsFromJson(
                         verified.result.constraintJson),
                 .longNoteMode = verified.result.longNoteMode,
             }});
        if (!started.ready()) {
          return failure(CourseReplayConsumerState::ContinuationFailed,
                         "Course replay continuation could not start.",
                         std::move(context));
        }
        continuation = *started.state;
      }

      const auto advanced = advanceCourseContinuation(
          *continuation,
          {.stageIndex = index,
           .score = materialized.judgedResult->score.score,
           .maximumScore = materialized.judgedResult->score.maxScore,
           .combo = materialized.endingCombo,
           .maximumCombo = materialized.judgedResult->score.maxCombo,
           .gauge = *materialized.finalGaugeState,
           .adoptedGauge = materialized.judgedResult->adoptedGaugeType,
           .restMicrosAfterStage =
               verified.document.playback.restMicrosAfterStage[index],
           .setup = stagePlayback.setup},
          setupSource(verified.stageSources[index]));
      if (!advanced.advanced()) {
        return failure(CourseReplayConsumerState::ContinuationFailed,
                       "Course replay carried state is inconsistent.",
                       std::move(context));
      }
      continuation = *advanced.state;
      carry = {.gauge = continuation->gauge,
               .combo = continuation->combo,
               .maximumCombo = continuation->maximumCombo};
      judgedStages.push_back(stageResultForChart(
          static_cast<int>(index), *materialized.judgedResult));
      compatibilityStages.push_back(
          {.replay = std::move(*materialized.replayData),
           .restMicrosAfterStage =
               verified.document.playback.restMicrosAfterStage[index]});
      preparedCharts.push_back(std::move(prepared));
    }

    if (!continuation || !continuation->complete() ||
        continuation->score != verified.result.finalScore ||
        continuation->maximumScore !=
            completedPrefixMaximumScore(verified.result) ||
        continuation->maximumCombo != verified.result.maxCombo ||
        continuation->gauge.currentGauge != verified.result.finalGauge) {
      return failure(CourseReplayConsumerState::ResultMismatch,
                     "Materialized course aggregate differs from its result.",
                     std::move(context));
    }

    auto judgedCourse = rebuildJudgedCourseResult(
        verified.result, std::move(judgedStages), diagnostic);
    if (!judgedCourse) {
      return failure(CourseReplayConsumerState::MaterializationFailed,
                     diagnostic.empty()
                         ? "Materialized course result could not be rebuilt."
                         : std::move(diagnostic),
                     std::move(context));
    }
    const auto courseAgreement =
        result_persistence::compareModernCourseResultFacts(
            verified.result, *judgedCourse);
    if (!courseAgreement.agrees()) {
      return failure(CourseReplayConsumerState::ResultMismatch,
                     courseAgreement.diagnostic, std::move(context));
    }

    auto replayData =
        makeCompatibilityCourse(verified, std::move(compatibilityStages));
    return {.state = CourseReplayConsumerState::Ready,
            .context = std::move(context),
            .charts = std::move(preparedCharts),
            .replayData = std::move(replayData),
            .continuation = std::move(continuation)};
  } catch (...) {
    return failure(CourseReplayConsumerState::MaterializationFailed,
                   "Course replay preparation failed.");
  }
}

} // namespace replay
