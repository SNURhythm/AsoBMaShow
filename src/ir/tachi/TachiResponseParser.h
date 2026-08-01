#pragma once

#include "../IrDriver.h"

#include <string_view>

namespace ir::tachi {

inline constexpr std::size_t kMaximumImportIdBytes = 128;

[[nodiscard]] bool isValidImportId(std::string_view value) noexcept;

[[nodiscard]] DeliveryOutcome
parseImmediateImportResponse(std::string_view body) noexcept;

[[nodiscard]] DeliveryOutcome
parseDeferredImportResponse(std::string_view body,
                            std::string_view requestOrigin) noexcept;

[[nodiscard]] DeliveryOutcome
parsePollStatusResponse(std::string_view body) noexcept;

[[nodiscard]] std::string
parseResponseDescription(std::string_view body) noexcept;

} // namespace ir::tachi
