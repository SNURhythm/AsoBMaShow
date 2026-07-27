#include "ChartReplayAgreement.h"

#include "ReplaySetupProvenance.h"

#include <utility>

namespace replay {
namespace {

ChartReplayAgreement disagreement(ChartReplayAgreementIssue issue,
                                  std::string diagnostic) {
  return {.issue = issue, .diagnostic = std::move(diagnostic)};
}

} // namespace

ChartReplayAgreement compareChartReplayToResult(
    const ReplayChartDocument &replay,
    const result_persistence::ModernChartResult &result,
    ReplaySetupSource source) noexcept {
  try {
    std::string diagnostic;
    if (!result_persistence::validateModernChartResult(result, diagnostic)) {
      return disagreement(ChartReplayAgreementIssue::Result,
                          diagnostic.empty() ? "modern result is invalid"
                                             : std::move(diagnostic));
    }
    const auto playback =
        validateReplayPlayback(replay.playback, source, replay.timeBounds);
    if (!playback.valid()) {
      return disagreement(ChartReplayAgreementIssue::Replay,
                          "captured replay playback is invalid");
    }
    const ReplayChartIdentity expected{.md5 = result.score.chartMd5,
                                       .sha256 = result.score.chartSha256,
                                       .keyMode = result.keyMode};
    if (compareReplayChartIdentity(replay.playback.setup.chart, expected) !=
        ReplayChartMatch::Match) {
      return disagreement(ChartReplayAgreementIssue::ChartIdentity,
                          "replay chart identity differs from the result");
    }
    if (replay.playback.setup.longNoteMode != result.score.longNoteMode) {
      return disagreement(ChartReplayAgreementIssue::LongNoteMode,
                          "replay long-note mode differs from the result");
    }
    if (!replaySetupAgreesWithProvenance(replay.playback.setup,
                                         result.score.provenance)) {
      return disagreement(ChartReplayAgreementIssue::SharedSetup,
                          "replay setup differs from result provenance");
    }
    return {};
  } catch (...) {
    return disagreement(ChartReplayAgreementIssue::Replay,
                        "chart replay agreement validation failed");
  }
}

} // namespace replay
