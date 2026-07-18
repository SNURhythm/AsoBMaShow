#pragma once

#include "../IrDriver.h"

#include <cstddef>
#include <string_view>

namespace ir::tachi {

inline constexpr std::string_view kProviderId = "tachi";
inline constexpr std::size_t kMaximumPayloadBytes = 64 * 1024;

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

[[nodiscard]] BuildDraftOutcome
buildBatchManualDraft(const IrSubmission &submission) noexcept;

} // namespace ir::tachi
