#include "CourseReplayCapture.h"

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

    ReplayCourseDocument document;
    document.playback.stages.reserve(capture.stages.size());
    document.playback.restMicrosAfterStage.reserve(capture.stages.size());
    document.timeBounds.reserve(capture.stages.size());
    for (std::size_t index = 0; index < capture.stages.size(); ++index) {
      const auto &raw = capture.stages[index];
      if (!raw.playback.has_value()) {
        appendDiagnostic(diagnostic,
                         "raw replay input capture was unavailable at stage " +
                             std::to_string(index + 1));
        return attempt;
      }
      const ReplayTimeBounds timeBounds = replayCaptureTimeBounds(
          raw.timeBounds, raw.playback->input, raw.playback->touchSamples,
          raw.playback->laneCoverEvents);
      attempt.pathInput.hasUndefinedLongNotes =
          attempt.pathInput.hasUndefinedLongNotes ||
          raw.playback->setup.hasUndefinedLongNotes;
      document.playback.stages.push_back(*raw.playback);
      document.playback.restMicrosAfterStage.push_back(
          raw.restMicrosAfterStage);
      document.timeBounds.push_back(timeBounds);
    }

    attempt.replay = std::move(document);
    return attempt;
  } catch (...) {
    diagnostic = "course replay completion capture failed";
    return std::nullopt;
  }
}

} // namespace replay
