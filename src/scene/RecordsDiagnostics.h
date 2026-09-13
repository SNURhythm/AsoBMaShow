#pragma once

#include "../ir/IrOutboxModels.h"

#include <string>
#include <string_view>

namespace replay_records {

inline std::string diagnosticOr(std::string_view diagnostic,
                                std::string_view fallback) {
  std::string result = ir::sanitizeDiagnostic(diagnostic);
  if (result.empty()) {
    result = ir::sanitizeDiagnostic(fallback);
  }
  return result;
}

} // namespace replay_records
