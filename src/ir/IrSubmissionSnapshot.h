#pragma once

#include "IrSubmission.h"

#include <optional>
#include <string>
#include <string_view>

namespace ir {

struct IrSubmissionSnapshot {
  static constexpr int kSchemaVersion = 1;

  int schemaVersion = kSchemaVersion;
  IrSubmission submission;
  std::string fingerprint;

  bool operator==(const IrSubmissionSnapshot &) const = default;
};

[[nodiscard]] std::optional<IrSubmissionSnapshot> captureIrSubmissionSnapshot(
    const result_persistence::PersistedChartResult &result,
    std::string &diagnostic) noexcept;

[[nodiscard]] std::optional<std::string>
serializeIrSubmissionSnapshot(const IrSubmissionSnapshot &snapshot,
                              std::string &diagnostic) noexcept;

[[nodiscard]] std::optional<IrSubmissionSnapshot>
deserializeIrSubmissionSnapshot(std::string_view serialized,
                                std::string_view expectedFingerprint,
                                std::string &diagnostic) noexcept;

} // namespace ir
