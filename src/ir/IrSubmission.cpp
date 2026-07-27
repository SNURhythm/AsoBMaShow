#include "IrSubmission.h"

#include "../CanonicalDigest.h"
#include "../DurablePayloadLimits.h"
#include "../Uuid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ir {
namespace {

bool isKnownClearRank(int value) noexcept {
  constexpr std::array ranks{
      kClearTypeFailedRank,
      kClearTypeAssistedEasyClearRank,
      kClearTypeLightAssistedEasyClearRank,
      kClearTypeEasyClearRank,
      kClearTypeNormalClearRank,
      kClearTypeHardClearRank,
      kClearTypeExHardClearRank,
      kClearTypeFullComboRank,
  };
  return std::ranges::find(ranks, value) != ranks.end();
}

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
        submission.keyMode <= 0 ||
        (!submission.chartMd5.empty() &&
         !canonical_digest::isCanonicalLowerHex(submission.chartMd5, 32)) ||
        !canonical_digest::isCanonicalLowerHex(submission.chartSha256, 64) ||
        submission.playedAtUnixMillis <= 0) {
      diagnostic = "IR submission identity is invalid";
      return false;
    }
    const std::array counts{
        submission.score,      submission.maxScore,   submission.maxCombo,
        submission.comboBreak, submission.pGreat,     submission.great,
        submission.good,       submission.bad,        submission.poor,
        submission.kPoor,      submission.fast,       submission.slow,
        submission.pGreatFast, submission.pGreatSlow, submission.earlyPGreat,
        submission.latePGreat, submission.earlyGreat, submission.lateGreat,
        submission.earlyGood,  submission.lateGood,   submission.earlyBad,
        submission.lateBad,    submission.earlyPoor,  submission.latePoor,
    };
    if (std::ranges::any_of(counts, [](int value) { return value < 0; }) ||
        submission.maxScore <= 0 || submission.score > submission.maxScore ||
        submission.maxScore % 2 != 0 ||
        submission.maxCombo > submission.maxScore / 2 ||
        static_cast<std::int64_t>(submission.pGreat) * 2LL + submission.great !=
            submission.score ||
        !std::isfinite(submission.finalGauge) || submission.finalGauge < 0.0F ||
        !isKnownClearRank(submission.clearType) ||
        !durable_payload::withinLimit(
            submission.gaugeHistory.size(),
            durable_payload::kMaximumResultGaugeSamples) ||
        std::ranges::any_of(
            submission.gaugeHistory,
            [](float value) { return !std::isfinite(value); })) {
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
