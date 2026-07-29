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
    const result_persistence::ModernChartResult &result) noexcept {
  try {
    const ReplayChartIdentity expected{.md5 = result.score.chartMd5,
                                       .sha256 = result.score.chartSha256,
                                       .keyMode = result.keyMode};
    if (compareReplayChartIdentity(replay.playback.setup.chart, expected) !=
        ReplayChartMatch::Match) {
      return disagreement(ChartReplayAgreementIssue::ChartIdentity,
                          "replay chart identity differs from the result");
    }
    const auto expectedLongNoteMode =
        result_persistence::replaySetupLongNoteMode(result.score);
    if (!expectedLongNoteMode ||
        replay.playback.setup.longNoteMode != *expectedLongNoteMode) {
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
    return disagreement(ChartReplayAgreementIssue::SharedSetup,
                        "chart replay binding comparison failed");
  }
}

} // namespace replay
