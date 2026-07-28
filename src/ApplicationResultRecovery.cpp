#include "ApplicationResultRecovery.h"

namespace application_result_recovery {

void execute(const Dependencies &dependencies) {
  const replay::ChartReplayRecoverySummary summary = dependencies.recover();
  if (summary.pending != 0 || summary.conflicts != 0) {
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
