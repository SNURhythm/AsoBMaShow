#include "CourseReplayConsumer.h"

#include "../CourseConstraintUtils.h"
#include "../CoursePlaySession.h"

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

void appendDiagnostic(std::string &destination, std::string_view value) {
  if (value.empty()) {
    return;
  }
  if (!destination.empty()) {
    destination += " ";
  }
  destination += value;
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

std::shared_ptr<CoursePlaySession> makeCourseReplayLaunchSession(
    CourseReplayConsumerOutcome outcome, CourseReplayLaunchMode mode,
    bool renderTouchPoints, bool renderGhosts) {
  if (!outcome.ready() || outcome.charts.size() !=
                              outcome.replayData->stages.size()) {
    return nullptr;
  }

  auto replayData = std::move(outcome.replayData);
  auto session = std::make_shared<CoursePlaySession>();
  session->courseId = replayData->courseId;
  session->courseKey = replayData->courseKey;
  session->courseName = replayData->courseName;
  session->courseGroupName = replayData->courseGroupName;
  session->constraintJson = replayData->constraintJson;
  const auto &savedResult = outcome.context.verified->result;
  session->entries.resize(
      static_cast<std::size_t>(savedResult.totalCharts));
  for (std::size_t index = 0; index < session->entries.size(); ++index) {
    session->entries[index].meta.TotalNotes =
        savedResult.entryFacts[index].totalNotes;
    session->entries[index].meta.PlayLength =
        savedResult.entryFacts[index].playLengthMicros;
  }
  for (std::size_t index = 0; index < replayData->stages.size(); ++index) {
    session->entries[index].meta = replayData->stages[index].replay.chartMeta;
  }
  session->snapshotRulesetFromReplay(replayData->stages.front().replay);
  const CourseConstraintSettings constraintSettings =
      courseConstraintSettingsFromJson(replayData->constraintJson);
  session->gaugeType = replayData->initialGaugeType;
  session->gaugeProfile = replayData->gaugeProfile;
  session->gaugeAutoShift = replayData->gaugeAutoShift;
  session->gaugeAutoShiftLowerBound =
      replayData->gaugeAutoShiftLowerBound;
  session->longNoteMode = replayData->longNoteMode;
  session->constraints = constraintSettings.rules;
  session->requestedPlayOption = replayData->requestedPlayOption;
  session->assistOption = replayData->assistOption;
  session->autoKeySound = false;
  session->preparedCourseCharts = std::move(outcome.charts);
  session->replayTouchVisualizationEnabled = renderTouchPoints;
  session->replayGhostRenderingEnabled = renderGhosts;
  if (mode == CourseReplayLaunchMode::Watch) {
    session->courseReplayPlayback = true;
    session->courseReplayData = std::move(replayData);
  } else {
    session->courseReplayPlayback = false;
    session->courseRetrySameData = std::move(replayData);
  }
  return session;
}

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
      const auto &savedStage = listedResult.stages[index];
      const ReplayChartIdentity savedIdentity{
          .md5 = savedStage.score.chartMd5,
          .sha256 = savedStage.score.chartSha256,
          .keyMode = savedStage.keyMode,
      };
      auto chart = dependencies_.parseBaseChart(
          completedChartPaths[index], savedIdentity,
          savedStage.score.provenance, cancelled, diagnostic);
      if (!chart || cancelled.load()) {
        return failure(CourseReplayConsumerState::ChartUnavailable,
                       diagnostic.empty()
                           ? "A selected course chart could not be parsed."
                           : std::move(diagnostic));
      }
      const auto expectedLongNoteMode =
          result_persistence::replaySetupLongNoteMode(
              listedResult.stages[index].score);
      if (!expectedLongNoteMode) {
        return failure(CourseReplayConsumerState::InvalidRequest,
                       "A saved course stage has no unique replay setup.");
      }
      parsedFacts.stages.push_back(makeParsedCourseReplayStageFacts(
          chart->Meta, *expectedLongNoteMode,
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
    std::vector<CourseReplayMaterializedStage> materializedStages;
    std::vector<result_persistence::ModernCourseStageResult> judgedStages;
    preparedCharts.reserve(parsedCharts.size());
    compatibilityStages.reserve(parsedCharts.size());
    materializedStages.reserve(parsedCharts.size());
    judgedStages.reserve(parsedCharts.size());
    std::optional<CourseContinuationState> continuation;
    ReplayPlaybackCarryState carry;
    std::string playbackDiagnostic;

    for (std::size_t index = 0; index < parsedCharts.size(); ++index) {
      const auto &savedStage = verified.result.stages[index];
      const auto &stagePlayback = verified.document.playback.stages[index];
      const auto expectedLongNoteMode =
          result_persistence::replaySetupLongNoteMode(savedStage.score);
      if (!expectedLongNoteMode) {
        return failure(CourseReplayConsumerState::InvalidRequest,
                       "A saved course stage has no unique replay setup.",
                       std::move(context));
      }
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

      const auto preparedFacts = makeParsedCourseReplayStageFacts(
          prepared->Meta, *expectedLongNoteMode,
          stagePlayback.setup.hasUndefinedLongNotes);
      const ReplayChartIdentity savedIdentity{
          .md5 = savedStage.score.chartMd5,
          .sha256 = savedStage.score.chartSha256,
          .keyMode = savedStage.keyMode,
      };
      if (compareReplayChartIdentity(preparedFacts.chart, savedIdentity) !=
              ReplayChartMatch::Match ||
          preparedFacts.longNoteMode != *expectedLongNoteMode) {
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
          stageDocument, savedChartResult, *prepared, carry);
      if (!materialized.playable() || !materialized.judgedResult ||
          !materialized.initialGaugeState || !materialized.finalGaugeState) {
        return failure(CourseReplayConsumerState::MaterializationFailed,
                       materialized.diagnostic.empty()
                           ? "Course replay stage could not produce playback data."
                           : std::move(materialized.diagnostic),
                       std::move(context));
      }
      if (materialized.state ==
          ReplayPlaybackMaterializationState::ResultMismatch) {
        appendDiagnostic(
            playbackDiagnostic,
            materialized.diagnostic.empty()
                ? "Course replay stage result differs from the saved result."
                : materialized.diagnostic);
      }
      if (!finalGaugeAgrees(materialized)) {
        appendDiagnostic(
            playbackDiagnostic,
            "Course replay stage gauge differs from its judged result.");
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
           .setup = stagePlayback.setup});
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
      materializedStages.push_back(
          {.initialGaugeState = *materialized.initialGaugeState,
           .finalGaugeState = *materialized.finalGaugeState,
           .judgedResult = *materialized.judgedResult,
           .endingCombo = materialized.endingCombo});
      compatibilityStages.push_back(
          {.replay = std::move(*materialized.replayData),
           .restMicrosAfterStage =
               verified.document.playback.restMicrosAfterStage[index]});
      preparedCharts.push_back(std::move(prepared));
    }

    if (!continuation || !continuation->complete()) {
      return failure(CourseReplayConsumerState::ContinuationFailed,
                     "Course replay continuation did not complete.",
                     std::move(context));
    }
    if (continuation->score != verified.result.finalScore ||
        continuation->maximumScore !=
            completedPrefixMaximumScore(verified.result) ||
        continuation->maximumCombo != verified.result.maxCombo ||
        continuation->gauge.currentGauge != verified.result.finalGauge) {
      appendDiagnostic(
          playbackDiagnostic,
          "Materialized course aggregate differs from its saved result.");
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
      appendDiagnostic(playbackDiagnostic, courseAgreement.diagnostic);
    }

    auto replayData =
        makeCompatibilityCourse(verified, std::move(compatibilityStages));
    return {.state = CourseReplayConsumerState::Ready,
            .context = std::move(context),
            .charts = std::move(preparedCharts),
            .materializedStages = std::move(materializedStages),
            .replayData = std::move(replayData),
            .continuation = std::move(continuation),
            .diagnostic = std::move(playbackDiagnostic)};
  } catch (...) {
    return failure(CourseReplayConsumerState::MaterializationFailed,
                   "Course replay preparation failed.");
  }
}

} // namespace replay
