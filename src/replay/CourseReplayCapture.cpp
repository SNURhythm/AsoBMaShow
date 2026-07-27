#include "CourseReplayCapture.h"

#include "ReplaySetupProvenance.h"

#include <utility>

namespace replay {
namespace {

void appendDiagnostic(std::string &destination, std::string source) {
  if (source.empty()) {
    return;
  }
  if (!destination.empty()) {
    destination += "; ";
  }
  destination += std::move(source);
}

bool setupAgrees(const ReplaySetup &setup,
                 const result_persistence::ModernCourseResult &result,
                 const result_persistence::ModernCourseStageResult &stage,
                 const CourseContinuationConstraints &constraints) noexcept {
  const ReplayChartIdentity expected{.md5 = stage.score.chartMd5,
                                     .sha256 = stage.score.chartSha256,
                                     .keyMode = stage.keyMode};
  return compareReplayChartIdentity(setup.chart, expected) ==
             ReplayChartMatch::Match &&
         setup.longNoteMode == stage.score.longNoteMode &&
         setup.longNoteMode == constraints.longNoteMode &&
         setup.initialGaugeType == result.initialGaugeType &&
         setup.gaugeProfile == result.gaugeProfile &&
         setup.gaugeAutoShift == result.gaugeAutoShift &&
         setup.gaugeAutoShiftLowerBound ==
             result.gaugeAutoShiftLowerBound &&
         replaySetupAgreesWithProvenance(setup, stage.score.provenance);
}

} // namespace

std::optional<CapturedCourseReplayAttempt>
captureCourseReplayAttempt(const CourseReplayCapture &capture,
                           std::string &diagnostic) noexcept {
  diagnostic.clear();
  try {
    std::string resultDiagnostic;
    if (!result_persistence::validateModernCourseResult(capture.result,
                                                        resultDiagnostic)) {
      diagnostic = resultDiagnostic.empty() ? "modern course result is invalid"
                                            : std::move(resultDiagnostic);
      return std::nullopt;
    }

    CapturedCourseReplayAttempt attempt{.result = capture.result};
    attempt.pathInput.longNoteMode = capture.result.longNoteMode;
    attempt.pathInput.beatorajaConstraintIds =
        capture.constraints.beatorajaConstraintIds;
    attempt.pathInput.stageSha256.reserve(capture.result.stages.size());
    for (const auto &stage : capture.result.stages) {
      attempt.pathInput.stageSha256.push_back(stage.score.chartSha256);
    }

    if (capture.constraints.longNoteMode != capture.result.longNoteMode ||
        capture.stages.size() != capture.result.stages.size()) {
      appendDiagnostic(diagnostic,
                       "raw course replay shape differs from its result");
      return attempt;
    }

    ReplayCourseDocument document;
    document.playback.stages.reserve(capture.stages.size());
    document.playback.restMicrosAfterStage.reserve(capture.stages.size());
    document.timeBounds.reserve(capture.stages.size());
    std::vector<ReplaySetupSource> sources;
    sources.reserve(capture.stages.size());
    for (std::size_t index = 0; index < capture.stages.size(); ++index) {
      const auto &raw = capture.stages[index];
      if (!raw.playback.has_value()) {
        appendDiagnostic(diagnostic,
                         "raw replay input capture was unavailable at stage " +
                             std::to_string(index + 1));
        return attempt;
      }
      const auto validation = validateReplayPlayback(
          *raw.playback, ReplaySetupSource::LocalCapture, raw.timeBounds);
      if (!validation.valid() ||
          !validCourseRestMicros(raw.restMicrosAfterStage) ||
          !setupAgrees(raw.playback->setup, capture.result,
                       capture.result.stages[index], capture.constraints)) {
        appendDiagnostic(diagnostic,
                         "raw course replay is invalid at stage " +
                             std::to_string(index + 1));
        return attempt;
      }
      attempt.pathInput.hasUndefinedLongNotes =
          attempt.pathInput.hasUndefinedLongNotes ||
          raw.playback->setup.hasUndefinedLongNotes;
      document.playback.stages.push_back(*raw.playback);
      document.playback.restMicrosAfterStage.push_back(
          raw.restMicrosAfterStage);
      document.timeBounds.push_back(raw.timeBounds);
      sources.push_back(ReplaySetupSource::LocalCapture);
    }

    const auto courseValidation = validateCourseReplayPlayback(
        document.playback, sources, document.timeBounds);
    std::string pathDiagnostic;
    if (!courseValidation.valid() ||
        !courseStem(attempt.pathInput, pathDiagnostic)) {
      appendDiagnostic(diagnostic,
                       pathDiagnostic.empty()
                           ? "captured course replay violates its envelope"
                           : std::move(pathDiagnostic));
      return attempt;
    }
    attempt.replay = std::move(document);
    return attempt;
  } catch (...) {
    diagnostic = "course replay completion capture failed";
    return std::nullopt;
  }
}

} // namespace replay
