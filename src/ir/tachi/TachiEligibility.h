#pragma once

#include "../IrDriver.h"

#include <optional>
#include <string>
#include <string_view>

namespace ir::tachi {

inline constexpr std::string_view kProviderId = "tachi";

// Preserve already verified v4 scores and frozen payloads under their own
// identity. This does not revive older, previously unsupported revisions.
[[nodiscard]] inline constexpr bool
supportsVerifiedLr2Revision(int revision) noexcept {
  return revision == 4 || revision == RulesetDescriptor::kCurrentVersion;
}

struct SubmissionEligibilityOutcome {
  SubmissionEligibilityReason reason =
      SubmissionEligibilityReason::InvalidSubmission;
  std::string diagnostic;

  [[nodiscard]] bool eligible() const noexcept {
    return reason == SubmissionEligibilityReason::Eligible;
  }
};

[[nodiscard]] SubmissionEligibilityOutcome
validateBokutachiEligibility(const IrSubmission &submission) noexcept;

[[nodiscard]] bool
isReplayEligibleForBokutachi(std::string_view attemptId,
                             bool hasCanonicalAttemptFingerprint,
                             const bms_parser::ChartMeta &meta,
                             const ScoreProvenance &provenance) noexcept;

[[nodiscard]] bool shouldShowReplayUploadMarker(
    std::string_view attemptId, bool hasCanonicalAttemptFingerprint,
    const bms_parser::ChartMeta &meta, const ScoreProvenance &provenance,
    std::optional<IrOutboxState> outboxState) noexcept;

} // namespace ir::tachi
