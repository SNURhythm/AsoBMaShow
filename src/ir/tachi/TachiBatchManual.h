#pragma once

#include "../IrDriver.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

struct TachiOutboxBatchDocument {
  std::vector<std::int64_t> rowIds;
  std::string playtype;
  std::string payloadJson;
};

enum class BuildTachiOutboxBatchStatus { Built, Invalid };

struct BuildTachiOutboxBatchOutcome {
  BuildTachiOutboxBatchStatus status = BuildTachiOutboxBatchStatus::Invalid;
  std::optional<TachiOutboxBatchDocument> document;
  std::string diagnostic;
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

[[nodiscard]] BuildDraftOutcome
buildBatchManualDraft(const IrSubmission &submission) noexcept;

[[nodiscard]] BuildTachiOutboxBatchOutcome
buildBatchManualOutboxDocument(std::span<const IrOutboxEntry> entries) noexcept;

} // namespace ir::tachi
