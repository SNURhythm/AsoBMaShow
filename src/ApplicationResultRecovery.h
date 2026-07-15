#pragma once

#include "ResultPersistenceCoordinator.h"

#include <functional>

namespace application_result_recovery {

struct Dependencies {
  std::function<result_persistence::RecoverySummary()> recover;
  std::function<void(const result_persistence::RecoverySummary &)>
      reportWarning;
  std::function<void()> runReadyRuntime;
};

void execute(const Dependencies &dependencies);

} // namespace application_result_recovery
