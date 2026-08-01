#pragma once

#include "TachiEligibility.h"
#include "../IrDriver.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ir::tachi {

inline constexpr std::size_t kMaximumPayloadBytes = 64 * 1024;

struct TachiOutboxBatchDocument {
  std::vector<std::int64_t> rowIds;
  std::string playtype;
  std::string payloadJson;
};

enum class BuildTachiOutboxBatchStatus { Built, Invalid };

struct BuildTachiOutboxBatchOutcome {
  BuildTachiOutboxBatchStatus status = BuildTachiOutboxBatchStatus::Invalid;
  std::optional<TachiOutboxBatchDocument> document;
  std::optional<std::int64_t> rejectedRowId;
  std::string diagnostic;
};

[[nodiscard]] BuildDraftOutcome
buildBatchManualDraft(const IrSubmission &submission) noexcept;

[[nodiscard]] BuildTachiOutboxBatchOutcome
buildBatchManualOutboxDocument(std::span<const IrOutboxEntry> entries) noexcept;

} // namespace ir::tachi
