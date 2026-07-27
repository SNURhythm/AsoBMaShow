#include "IrSubmission.h"

#include "IrOutboxModels.h"
#include "../ResultPersistenceModel.h"

namespace ir {

IrSubmissionBuildOutcome
makeIrSubmission(const result_persistence::ChartResultAttempt &attempt,
                 std::int64_t playedAtUnixMillis) noexcept {
  std::string diagnostic;
  const auto projected =
      result_persistence::projectModernResultFromLegacyAttempt(
          attempt, playedAtUnixMillis, diagnostic);
  if (!projected) {
    return {.diagnostic = sanitizeDiagnostic(diagnostic)};
  }
  return makeIrSubmission(*projected);
}

} // namespace ir
