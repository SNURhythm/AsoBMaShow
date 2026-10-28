#pragma once

#include "IrSubmission.h"
#include "../DurablePayloadLimits.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ir {

inline constexpr std::size_t kMaximumIrSubmissionSnapshotBytes =
    durable_payload::kMaximumIrSnapshotBytes;

struct IrSubmissionSnapshot {
  static constexpr int kSchemaVersion = 1;

  int schemaVersion = kSchemaVersion;
  IrSubmission submission;
  std::string fingerprint;

  bool operator==(const IrSubmissionSnapshot &) const = default;
};

[[nodiscard]] std::optional<IrSubmissionSnapshot>
captureIrSubmissionSnapshot(const result_persistence::ModernChartResult &result,
                            std::string &diagnostic) noexcept;

[[nodiscard]] std::optional<std::string>
serializeIrSubmissionSnapshot(const IrSubmissionSnapshot &snapshot,
                              std::string &diagnostic) noexcept;

// Verify the original snapshot fingerprint, then compare its facts to the
// authenticated owner using canonical lamp semantics. Preserve its wire bytes.
[[nodiscard]] std::optional<std::string>
serializeIrSubmissionSnapshot(const IrSubmissionSnapshot &snapshot,
                              const result_persistence::ModernChartResult &owner,
                              std::string &diagnostic) noexcept;

[[nodiscard]] std::optional<IrSubmissionSnapshot>
deserializeIrSubmissionSnapshot(std::string_view serialized,
                                std::string_view expectedFingerprint,
                                std::string &diagnostic) noexcept;

} // namespace ir
