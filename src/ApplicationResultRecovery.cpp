#include "ApplicationResultRecovery.h"

namespace application_result_recovery {

void execute(const Dependencies &dependencies) {
  const result_persistence::RecoverySummary summary = dependencies.recover();
  if (!summary.userMessage.empty()) {
    dependencies.reportWarning(summary);
  }
  if (dependencies.startProfileServices) {
    try {
      dependencies.startProfileServices();
    } catch (...) {
      // IR is optional. Local result recovery and the ready runtime remain
      // authoritative when service startup is unavailable.
    }
  }
  dependencies.runReadyRuntime();
}

} // namespace application_result_recovery
