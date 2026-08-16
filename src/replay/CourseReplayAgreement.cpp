#include "CourseReplayAgreement.h"

#include "ReplaySetupProvenance.h"

#include <utility>

namespace replay {
namespace {

CourseReplayAgreement disagreement(CourseReplayAgreementIssue issue,
                                   std::string diagnostic,
                                   std::size_t stageIndex = 0) {
  return {.issue = issue,
          .stageIndex = stageIndex,
          .diagnostic = std::move(diagnostic)};
}

} // namespace

CourseReplayAgreement compareCourseReplayPathToResult(
    const CoursePathInput &path,
    const result_persistence::ModernCourseResult &result,
    const ReplayLimits &limits) noexcept {
  try {
    std::string diagnostic;
    const auto expected =
        courseReplayPathInputForResult(result, path.hasUndefinedLongNotes);
    if (path.stageSha256 != expected.stageSha256 ||
        path.longNoteMode != expected.longNoteMode ||
        path.beatorajaConstraintIds != expected.beatorajaConstraintIds) {
      return disagreement(
          CourseReplayAgreementIssue::Path,
          "course replay path facts differ from the saved result");
    }
    if (!courseStem(path, diagnostic, limits)) {
      return disagreement(CourseReplayAgreementIssue::Path,
                          diagnostic.empty()
                              ? "course replay path facts are invalid"
                              : std::move(diagnostic));
    }
    return {};
  } catch (...) {
    return disagreement(CourseReplayAgreementIssue::Path,
                        "course replay path agreement validation failed");
  }
}

CourseReplayAgreement compareCourseReplayToResult(
    const ReplayCourseDocument &replay,
    const result_persistence::ModernCourseResult &result) noexcept {
  try {
    if (replay.playback.stages.size() != result.stages.size()) {
      return disagreement(
          CourseReplayAgreementIssue::CourseShape,
          "course replay completed prefix differs from the saved result");
    }

    for (std::size_t index = 0; index < result.stages.size(); ++index) {
      const auto &setup = replay.playback.stages[index].setup;
      const auto &stage = result.stages[index];
      const auto expectedLongNoteMode =
          result_persistence::replaySetupLongNoteMode(stage.score);
      const ReplayChartIdentity expected{.md5 = stage.score.chartMd5,
                                         .sha256 = stage.score.chartSha256,
                                         .keyMode = stage.keyMode};
      if (compareReplayChartIdentity(setup.chart, expected) !=
          ReplayChartMatch::Match) {
        return disagreement(
            CourseReplayAgreementIssue::StageIdentity,
            "course replay stage identity differs from the saved result",
            index);
      }
      if (!expectedLongNoteMode ||
          setup.longNoteMode != *expectedLongNoteMode) {
        return disagreement(
            CourseReplayAgreementIssue::LongNoteMode,
            "course replay stage long-note mode differs from the saved result",
            index);
      }
      if (setup.player1.option != result.requestedPlayOption ||
          setup.assistOption != assist_options::normalize(result.assistOption) ||
          setup.initialGaugeType != result.initialGaugeType ||
          setup.gaugeProfile != result.gaugeProfile ||
          setup.gaugeAutoShift != result.gaugeAutoShift ||
          setup.gaugeAutoShiftLowerBound !=
              result.gaugeAutoShiftLowerBound ||
          !replaySetupAgreesWithProvenance(setup,
                                           stage.score.provenance)) {
        return disagreement(
            CourseReplayAgreementIssue::SharedSetup,
            "course replay stage setup differs from the saved result", index);
      }
    }
    return {};
  } catch (...) {
    return disagreement(CourseReplayAgreementIssue::SharedSetup,
                        "course replay binding comparison failed");
  }
}

} // namespace replay
