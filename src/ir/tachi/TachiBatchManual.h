#pragma once

#include "../IrDriver.h"

#include <cstddef>
#include <string_view>

namespace ir::tachi {

inline constexpr std::string_view kProviderId = "tachi";
inline constexpr std::size_t kMaximumPayloadBytes = 64 * 1024;

[[nodiscard]] BuildDraftOutcome
buildBatchManualDraft(const IrSubmission &submission) noexcept;

} // namespace ir::tachi
