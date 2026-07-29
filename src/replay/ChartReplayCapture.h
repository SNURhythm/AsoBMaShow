#pragma once

#include "ChartReplayPersistence.h"

#include <optional>
#include <string>
#include <vector>

namespace replay {

struct ChartReplayCapture {
  result_persistence::ModernChartResult result;
  LocalReplaySetupFacts setupFacts;
  std::optional<std::vector<InputTransition>> acceptedInput;
  std::vector<ReplayTouchSample> touchSamples;
  std::vector<ReplayLaneCoverEvent> laneCoverEvents;
  ReplayTimeBounds timeBounds;
};

[[nodiscard]] std::optional<ChartReplayPersistenceAttempt>
captureChartReplayPersistenceAttempt(const ChartReplayCapture &capture,
                                     std::string &diagnostic) noexcept;

} // namespace replay
