#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ir {

inline constexpr std::size_t kMaximumDiagnosticBytes = 512;

struct IrOutboxDraft {
  std::string providerId;
  std::string attemptId;
  std::string chartMd5;
  std::string chartSha256;
  std::string payloadJson;
  std::int64_t createdAtUnixMillis = 0;

  bool operator==(const IrOutboxDraft &) const = default;
};

struct IrOutboxEntry;

[[nodiscard]] std::string sanitizeDiagnostic(std::string_view value);

} // namespace ir
