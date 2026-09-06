#include "ChartReplayCapture.h"

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

} // namespace

std::optional<ChartReplayPersistenceAttempt>
captureChartReplayPersistenceAttempt(const ChartReplayCapture &capture,
                                     std::string &diagnostic) noexcept {
  diagnostic.clear();
  try {
    std::string resultDiagnostic;
    if (!result_persistence::validateModernChartResult(capture.result,
                                                        resultDiagnostic)) {
      diagnostic = resultDiagnostic.empty() ? "modern result is invalid"
                                            : std::move(resultDiagnostic);
      return std::nullopt;
    }

    ChartReplayPersistenceAttempt attempt{
        .result = capture.result,
        .averageJudgeMicros = capture.averageJudgeMicros};
    std::string snapshotDiagnostic;
    attempt.irSnapshot =
        ir::captureIrSubmissionSnapshot(attempt.result, snapshotDiagnostic);
    if (!attempt.irSnapshot) {
      appendDiagnostic(diagnostic,
                       snapshotDiagnostic.empty()
                           ? "IR snapshot capture was unavailable"
                           : std::move(snapshotDiagnostic));
    }

    if (!capture.acceptedInput.has_value()) {
      appendDiagnostic(diagnostic, "raw replay input capture was unavailable");
      return attempt;
    }

    std::string setupDiagnostic;
    auto setup = captureLocalReplaySetup(capture.setupFacts,
                                         capture.result.score.provenance,
                                         setupDiagnostic);
    if (!setup) {
      appendDiagnostic(diagnostic,
                       setupDiagnostic.empty()
                           ? "replay setup capture was unavailable"
                           : std::move(setupDiagnostic));
      return attempt;
    }

    ReplayChartDocument document{
        .playback = {.setup = std::move(*setup),
                     .input = *capture.acceptedInput,
                     .touchSamples = capture.touchSamples,
                     .laneCoverEvents = capture.laneCoverEvents},
        .timeBounds = capture.timeBounds,
    };
    document.timeBounds = replayCaptureTimeBounds(
        document.timeBounds, document.playback.input,
        document.playback.touchSamples, document.playback.laneCoverEvents);
    attempt.replay = std::move(document);
    return attempt;
  } catch (...) {
    diagnostic = "chart replay completion capture failed";
    return std::nullopt;
  }
}

} // namespace replay
