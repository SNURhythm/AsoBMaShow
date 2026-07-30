#include "IrSubmission.h"

#include "../ResultContracts.h"
#include "../Uuid.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace ir {
namespace {

bool timingPairMatches(int early, int late, int total) noexcept {
  return static_cast<std::int64_t>(early) + static_cast<std::int64_t>(late) ==
         static_cast<std::int64_t>(total);
}

} // namespace

bool validateIrSubmission(const IrSubmission &submission,
                          std::string &diagnostic) noexcept {
  try {
    diagnostic.clear();
    if (!uuid::isCanonicalLowerV4(submission.attemptId) ||
        !result_contract::isValidKeyMode(submission.keyMode) ||
        !result_contract::canonicalChartHashes(submission.chartMd5,
                                               submission.chartSha256, false) ||
        submission.playedAtUnixMillis <= 0) {
      diagnostic = "IR submission identity is invalid";
      return false;
    }
    const result_contract::ResultOutcomeFacts outcome{
        .score = submission.score,
        .maxScore = submission.maxScore,
        .maxCombo = submission.maxCombo,
        .comboBreak = submission.comboBreak,
        .pGreat = submission.pGreat,
        .great = submission.great,
        .good = submission.good,
        .bad = submission.bad,
        .poor = submission.poor,
        .kPoor = submission.kPoor,
        .fast = submission.fast,
        .slow = submission.slow,
        .finalGauge = submission.finalGauge,
        .clearType = submission.clearType,
        .gaugeHistory = submission.gaugeHistory,
    };
    const std::array timingCounts{
        submission.pGreatFast, submission.pGreatSlow, submission.earlyPGreat,
        submission.latePGreat, submission.earlyGreat, submission.lateGreat,
        submission.earlyGood,  submission.lateGood,   submission.earlyBad,
        submission.lateBad,    submission.earlyPoor,  submission.latePoor,
    };
    if (!result_contract::validResultOutcome(
            outcome, static_cast<std::int64_t>(submission.maxScore) / 2LL) ||
        std::ranges::any_of(timingCounts,
                            [](int value) { return value < 0; })) {
      diagnostic = "IR submission result facts are invalid";
      return false;
    }
    if (submission.judgementTimingBreakdownAvailable) {
      if (!timingPairMatches(submission.earlyPGreat, submission.latePGreat,
                             submission.pGreat) ||
          !timingPairMatches(submission.earlyGreat, submission.lateGreat,
                             submission.great) ||
          !timingPairMatches(submission.earlyGood, submission.lateGood,
                             submission.good) ||
          !timingPairMatches(submission.earlyBad, submission.lateBad,
                             submission.bad) ||
          !timingPairMatches(submission.earlyPoor, submission.latePoor,
                             submission.poor) ||
          submission.pGreatFast > submission.pGreat ||
          submission.pGreatSlow > submission.pGreat ||
          static_cast<std::int64_t>(submission.pGreatFast) +
                  submission.pGreatSlow >
              submission.pGreat) {
        diagnostic = "IR submission timing facts are inconsistent";
        return false;
      }
    } else if (submission.pGreatFast != 0 || submission.pGreatSlow != 0 ||
               submission.earlyPGreat != 0 || submission.latePGreat != 0 ||
               submission.earlyGreat != 0 || submission.lateGreat != 0 ||
               submission.earlyGood != 0 || submission.lateGood != 0 ||
               submission.earlyBad != 0 || submission.lateBad != 0 ||
               submission.earlyPoor != 0 || submission.latePoor != 0) {
      diagnostic = "IR submission has timing facts without a breakdown";
      return false;
    }
    std::string provenanceDiagnostic;
    if (!serializeValidatedScoreProvenance(submission.provenance,
                                           provenanceDiagnostic)) {
      diagnostic = "IR submission provenance is invalid";
      return false;
    }
    return true;
  } catch (...) {
    diagnostic = "IR submission validation failed";
    return false;
  }
}

} // namespace ir
