#include "ApplicationResultRecovery.h"

namespace application_result_recovery {

void execute(const Dependencies &dependencies) {
  const result_persistence::RecoverySummary summary = dependencies.recover();
  if (!summary.userMessage.empty()) {
    dependencies.reportWarning(summary);
  }
  dependencies.runReadyRuntime();
}

} // namespace application_result_recovery
