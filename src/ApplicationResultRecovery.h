#pragma once

#include "replay/ChartReplayPersistence.h"

#include <functional>

namespace application_result_recovery {

struct Dependencies {
  std::function<replay::ChartReplayRecoverySummary()> recover;
  std::function<void(const replay::ChartReplayRecoverySummary &)>
      reportWarning;
  std::function<void()> startProfileServices;
  std::function<void()> runReadyRuntime;
};

void execute(const Dependencies &dependencies);

} // namespace application_result_recovery
